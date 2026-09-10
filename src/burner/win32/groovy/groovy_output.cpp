// Groovy MiSTer - the sender.
//
// This file and groovy_input.cpp are the only places that reach groovymister.h, and therefore
// <winsock2.h>, both through the module-internal groovy_internal.h. main.cpp includes the
// Winsock 1.1 <winsock.h>, which MSVC will not tolerate alongside <winsock2.h> in one
// translation unit, so keep the boundary where it is.
//
// Threading: none. FBNeo renders into a CPU buffer, so there is no GPU readback to hide and
// nothing to gain from a sender thread. Everything here runs on the emulation thread, which is
// the synchronous shape the Groovy client is designed around: one thread owns CmdInit,
// CmdSwitchres, CmdBlit, CmdAudio and WaitSync.

#include "burner.h"

#include "groovy_output.h"
#include "groovy_config.h"
#include "groovy_switchres.h"
#include "groovy_pixels.h"
#include "groovy_modeline.h"
#include "groovy_log.h"

#include "groovy_internal.h"	// declares the shared `gm` instance defined below

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// For the raw CMD_CLOSE socket below. groovymister.h brings in <winsock2.h> on Windows and
// <sys/socket.h>/<netinet/in.h> on POSIX, but not these.
#ifndef _WIN32
#include <arpa/inet.h>	// inet_addr
#include <unistd.h>		// close
#include <errno.h>
#endif

using namespace GroovyMiSTer;

// Shared with groovy_input.cpp via groovy_internal.h. Not static for that reason.
GroovyMister gm;


static bool   bSessionOpen  = false;	// CmdInit succeeded
static bool   bStreaming    = false;	// a modeline is in effect and blits are going out

// Sticky for the life of the process: did we EVER get a session up? The close path gates on
// this rather than on bSessionOpen, because the whole point of that path is to be immune to
// confusion about the current state.
static bool   bEverConnected = false;

// Has this session's CMD_CLOSE already gone out? Exactly one is allowed; the core ignores any
// that arrive with no session open.
static bool   bRawCloseSent  = false;

// A deliberate close must stay closed.
//
// Frames do not stop the instant we close: OnClose posts WM_QUIT, which lands only when the loop
// next dequeues it, and QuarkEnd sets bMediaExit, which run.cpp acts on inside its PeekMessage
// branch. Without this latch the next rendered frame re-opens the session - GroovySessionClose()
// leaves nLastConnectFailMs at 0, which SessionRetryDue() reads as "never failed, retry now" -
// and the process then exits with the core still connected and holding our last frame.
//
// Cleared only on an edge that means a new session is wanted (see the top of GroovyFrameReady()).
// The internal SessionClose("params changed") reconnect does not set it, since that path must be
// able to come straight back up.
static bool   bShutdownRequested = false;

// The endpoint we ACTUALLY opened with. The close goes here rather than to whatever the config
// currently says, so editing the host (or applying the settings dialog) mid-session cannot leave
// us closing a session on one address that we opened on another.
static char   szOpenHost[64] = "";
static INT32  nOpenPort      = 0;

// Keepalive for an idle session.
//
// We advertise GM_CAP_KEEPALIVE at CmdInit, which licenses the core to close a session that stays
// silent for its OSD idle timeout. That is what frees the user's CRT when this process is killed
// without sending CMD_CLOSE. The obligation is ours: send something whenever we are alive and not
// blitting. Offline that starts the moment a menu opens, because OnEnterIdle() only pumps RunIdle
// when kNetGame, so a Win32 menu or modal dialog stops the frame loop entirely.
//
// nLastWireMs is stamped by the blit and the audio path, so during emulation the gate below never
// opens and no extra packets go out.
static UINT32 nLastWireMs = 0;

// 2s against the shortest timeout the OSD offers (5s), which we cannot read. Callers must poll
// faster than this: a tick landing just under the threshold defers the send by a whole period.
// scrn.cpp polls at 250ms, so worst-case silence is 2.25s, or 4.5s if one datagram is lost.
#define GROOVY_KEEPALIVE_IDLE_MS 2000

// Dead-session detection.
//
// We can be disconnected without being told: a lost keepalive lets the idle timeout fire, and any
// CMD_INIT from any address takes the session over. The core then refuses session-scoped commands
// while disconnected, so our blits are dropped and frameEcho simply stops advancing while we keep
// streaming into nothing.
//
// Two tests, because "did the echo change" alone is not enough. That is what the client's own
// reconnect watchdog uses, and it treats any larger value as progress, so a corrupt or reordered
// echo resets its counter in exactly the case it is most needed. The second test asks whether the
// echo is plausible for what we sent: it may lag us by pipeline depth, but it can never lead us.
//
// The stale test is the one that normally fires. The plausibility test costs nothing and bounds
// the damage if a core ever hands back nonsense, which reaches DiffTimeRaster() and turns into a
// multi-second busy-spin inside WaitSync.
static UINT32 nLastEchoSeen   = 0;
static UINT32 nNoEchoBlits    = 0;
static UINT32 nBadEchos       = 0;
static UINT32 nLastReconEpoch = 0;
static bool   bSessionDead    = false;

// Well above the client's own 10-blit watchdog so it gets first refusal on a transient fault.
#define GROOVY_DEAD_BLITS 60

// How far the echo may lead what we sent before it stops being an acknowledgement. Generous: the
// core normally runs one frame BEHIND us (measured at 2041/2042 samples), so any lead at all is
// already odd and this only has to reject nonsense.
#define GROOVY_ECHO_SLACK 16

// Consecutive implausible echoes before we act, so one corrupt or reordered datagram cannot kill a
// healthy session.
#define GROOVY_BAD_ECHOS 5

// Reset our own frame counter for a new session.
//
// The client zeroes its fpga.* raster state at the top of every CmdInit, so a dead session's
// counters can no longer cross a reconnect. It has no notion of nBlitFrame, which is ours, so
// this is the part nothing upstream can do for us.
//
// It matters because the resync in GroovyFrameReady() adopts fpga.frame. Blitting a counter from
// a previous session into a core that restarted at zero gives DiffTimeRaster() a spread of
// thousands, and WaitSync accumulates sleepTime += diffRaster on every iteration - half a frame
// period of busy-spin per frame of divergence, which is minutes.
//
// Call on every session start and on every observed internal reconnect (reconnectEpoch() change).
static void ResetClientFrameState();

// Above this many frames of (frameEcho - frame), refuse to hand the state to WaitSync at all. The
// implied sleep is spread/2 frame periods, so 8 caps it at roughly 67ms at 60Hz. A legitimate lead
// is 1, occasionally 2 with frame delay; we run one frame ahead of the display, never thousands.
//
// The client carries the same clamp internally now. This copy is kept for the diagnostic - see the
// call site in GroovyWaitSync() for why ours is the one that reaches a support log.
#define GROOVY_RASTER_MAX_SPREAD 8

static bool   bSwitchresUp  = false;
static bool   bHaveModeline = false;
static Modeline curModeline;
static UINT32 nBlitFrame    = 0;

// The library zeroes its own fpga.* on every CmdInit now (resetSessionState(), see above) - this
// only has to reset the counter it doesn't know about.
static void ResetClientFrameState()
{
	nBlitFrame = 0;
}

// Session parameters are baked into CMD_INIT and cannot change mid-session: a codec, RGB
// mode, audio rate or MTU change needs a full re-init. Remember what we opened with so we
// can notice and reconnect.
static INT32 nOpenCodec = -1, nOpenRgbMode = -1, nOpenMtu = -1;
static INT32 nOpenSoundRate = -1, nOpenSoundChan = -1;

// Status
static char   szState[96]     = "disabled";
static UINT32 nFramesSent     = 0;
static UINT32 nGateRefusals   = 0;
static UINT32 nAudioBytesSent = 0;
static INT32  nLastSrcW = 0, nLastSrcH = 0, nLastSrcFps = 0, nLastSrcDepth = 0;
static UINT32 nLastBlitBytes  = 0;

// Only nag the user once per distinct refusal, or a refused mode would spam the OSD 60
// times a second.
static GroovyModeResult eLastRefusal = GROOVY_MODE_OK;

// ---------------------------------------------------------------------------
// Frame-cost instrumentation
// ---------------------------------------------------------------------------
//
// Under netplay the answer to "Groovy is eating my frame budget" is to TELL the user, never to
// silently reconfigure mid-match. That makes the measurement the deliverable, so it has to be
// honest: wall-clock, on the emulation thread, separated into the two things that can cost.

static double dPackBlitMs   = 0.0;	// pack + encode + post, inside GroovyFrameReady()
static double dSyncMs       = 0.0;	// total time inside WaitSync, whichever regime

// The two halves of dSyncMs.
//
// WaitSync does two different jobs depending on who owns the frame clock, and charging both to one
// counter makes the readout meaningless. When we pace, sleeping out the remainder of the frame
// period is the job, and by construction that is most of a frame. When something else paces
// (netplay), sleepTime should be 0, so every millisecond in there is overhead - and that is the
// number that says whether Groovy is eating a match's frame budget.
static double dPaceSleepMs    = 0.0;	// last frame, when we owned the clock: intentional
static double dSyncOverheadMs = 0.0;	// last frame, when we did not: chargeable

// Charged and cleared by AccumulateFrameCost(), so a frame that never reached WaitSync bills
// nothing rather than re-billing the previous frame's figure.
static double dPendingOverheadMs = 0.0;

static double dWorstFrameMs    = 0.0;	// worst chargeable cost, over a rolling window
static double dWorstOverheadMs = 0.0;	// worst sync overhead alone, same window
static double dOverheadSumMs   = 0.0;	// for the mean over that window
static UINT32 nBudgetMissed = 0;	// frames whose chargeable cost blew the budget fraction
static UINT32 nCostSamples  = 0;
static UINT64 nWorstResetMs = 0;

// A frame is "missed" when our work alone eats this much of the frame period. Not a hard
// failure - emulation, rollback and runahead all still need their share - but past this the
// user should know.
#define GROOVY_BUDGET_FRACTION 0.5

static UINT64 GroovyTickNow()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return (UINT64)t.QuadPart;
}

static double GroovyTickMsSince(UINT64 nStart)
{
	static double dTicksPerMs = 0.0;
	if (dTicksPerMs == 0.0) {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		dTicksPerMs = (double)f.QuadPart / 1000.0;
	}
	return (double)(GroovyTickNow() - nStart) / dTicksPerMs;
}

static void AccumulateFrameCost()
{
	// Only CHARGEABLE time counts against the budget - see dPaceSleepMs / dSyncOverheadMs.
	// Consume the pending overhead so a frame that never reached WaitSync bills nothing
	// instead of re-billing the previous frame's figure.
	const double dOverhead = dPendingOverheadMs;
	dPendingOverheadMs = 0.0;

	const double dTotal = dPackBlitMs + dOverhead;
	if (dTotal > dWorstFrameMs) dWorstFrameMs = dTotal;
	if (dOverhead > dWorstOverheadMs) dWorstOverheadMs = dOverhead;
	dOverheadSumMs += dOverhead;
	nCostSamples++;

	const double dFrameMs = (nAppVirtualFps > 0) ? (100000.0 / (double)nAppVirtualFps) : 16.6667;
	if (dTotal > dFrameMs * GROOVY_BUDGET_FRACTION) nBudgetMissed++;

	// Decay the worst-case every ~5s so a single startup hitch does not sit in the readout
	// forever, and report sustained trouble to the OSD - the only surface visible during a
	// match, since the settings dialog pauses the game.
	const UINT64 nNowMs = (UINT64)timeGetTime();
	if (nWorstResetMs == 0) nWorstResetMs = nNowMs;
	if (nNowMs - nWorstResetMs >= 5000) {
		// Drained every window regardless of kNetGame, so a count cannot carry over from a
		// previous netplay session into an offline one.
		const UINT32 nIdleCalls = QuarkTakeIdleCount();

		if (nCostSamples > 0) {
			const double dMissPct  = 100.0 * (double)nBudgetMissed / (double)nCostSamples;
			const double dMeanOver = dOverheadSumMs / (double)nCostSamples;

			// "sync overhead" is the netplay-relevant figure and should sit near zero during a
			// match; "pacing sleep" is reported for context and is expected to be most of a
			// frame whenever we own the clock.
			GroovyLog(GROOVY_LOG_INFO,
			          "frame cost [%s]: packblit %.2fms, sync overhead %.2fms (mean %.2f worst %.2f), "
			          "pacing sleep %.2fms, worst chargeable %.2fms, %u/%u frames over %.0f%% budget",
			          GroovyPacingActive() ? "groovy-paced" : "external clock",
			          dPackBlitMs, dSyncOverheadMs, dMeanOver, dWorstOverheadMs,
			          dPaceSleepMs, dWorstFrameMs, nBudgetMissed, nCostSamples,
			          GROOVY_BUDGET_FRACTION * 100.0);

			// @groovy diagnostic: does the sync overhead above cost netplay anything?
			//
			// The spin should be the complement of this frame loop's own work, so it is large
			// exactly when GGPO is not the bottleneck: high overhead alongside a high idle count
			// and few rollbacks, and the reverse when the peer is struggling. Two hardware
			// sessions on an identical modeline measured 0.23ms and 9.62ms mean, and the fast one
			// was the session full of GGPO stalls and frameskips. On that reading the spin only
			// consumes time nothing else wanted; these counters are what would show otherwise.
			if (kNetGame) {
				GroovyLog(GROOVY_LOG_INFO,
				          "netplay load: %u ggpo_idle calls over %u frames (%.1f/frame), "
				          "rollbacks %d (%d frames)",
				          nIdleCalls, nCostSamples, (double)nIdleCalls / (double)nCostSamples,
				          nRollbackCount, nRollbackFrames);
			}

			if (dMissPct >= 20.0) {
				TCHAR szMsg[128];
				_sntprintf(szMsg, 127, _T("Groovy: using %.1fms/frame - consider LZ4 or RGB565"),
				           dWorstFrameMs);
				szMsg[127] = _T('\0');
				VidSNewShortMsg(szMsg, 0xFFBF3F, 4000, 0);
			}
		}
		dWorstFrameMs    = 0.0;
		dWorstOverheadMs = 0.0;
		dOverheadSumMs   = 0.0;
		nBudgetMissed = 0;
		nCostSamples  = 0;
		nWorstResetMs = nNowMs;
	}
}

// The raster line each blit syncs to.
//
// vCountSync 0 means automatic frame delay, and that calculation rests entirely on
// m_emulationTime, which only WaitSync() writes and which measures wall time between WaitSync
// calls. Once something else owns the frame clock, that interval is the whole frame period, the
// calculation degenerates to vSync = 1 permanently, and the automatic part is a fiction. Under an
// external clock we pick the line explicitly instead.
static uint16_t EffectiveVCountSync()
{
	if (kNetGame && nGroovyVCountSync == 0) {
		return 1;						// raster-chase from the top
	}
	return (uint16_t)nGroovyVCountSync;
}

static void SetState(const char* pszFmt, ...)
{
	va_list args;
	va_start(args, pszFmt);
	vsnprintf(szState, sizeof(szState) - 1, pszFmt, args);
	va_end(args);
	szState[sizeof(szState) - 1] = '\0';
}

// The Groovy client's log sink. Installed before CmdInit - FBNeo is a GUI app, so without
// this a failed handshake is completely invisible.
static void GroovyClientLogSink(const char* pszMsg)
{
	GroovyLogRaw(GROOVY_LOG_ERROR, pszMsg);
}

// ---------------------------------------------------------------------------
// Audio rate mapping
// ---------------------------------------------------------------------------

// CMD_INIT accepts only these rates, and bakes the choice into the session.
static bool GroovyAudioRateOkay(INT32 nRate)
{
	return nRate == 22050 || nRate == 44100 || nRate == 48000;
}

// Connect back-off. See the call site in GroovyFrameReady() for why this matters.
#define GROOVY_RETRY_INTERVAL_MS 5000

static UINT32 nLastConnectFailMs = 0;
static bool   bConnectGaveUp     = false;	// netplay: one failure and we stand down for good

static bool SessionRetryDue()
{
	if (bConnectGaveUp) return false;
	if (nLastConnectFailMs == 0) return true;			// never failed yet

	const UINT32 nNow = (UINT32)timeGetTime();
	return (nNow - nLastConnectFailMs) >= GROOVY_RETRY_INTERVAL_MS;
}

static void NoteConnectFailed()
{
	nLastConnectFailMs = (UINT32)timeGetTime();
	if (nLastConnectFailMs == 0) nLastConnectFailMs = 1;	// 0 means "never"

	if (kNetGame) {
		bConnectGaveUp = true;
		GroovyLog(GROOVY_LOG_ERROR,
		          "connect failed during netplay - standing down for this session rather than "
		          "retrying inside the frame loop");
	}
}


// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

static void SessionClose(const char* pszWhy)
{
	if (bSessionOpen) {
		// CmdClose must come from the thread that owns the socket. We are single-threaded,
		// so that is always true here.
		gm.CmdClose();
		GroovyLog(GROOVY_LOG_ERROR, "session closed (%s)", pszWhy ? pszWhy : "");
	}
	bSessionOpen  = false;
	bStreaming    = false;
	bHaveModeline = false;
	nBlitFrame    = 0;
	nOpenCodec = nOpenRgbMode = nOpenMtu = -1;
	nOpenSoundRate = nOpenSoundChan = -1;
	eLastRefusal  = GROOVY_MODE_OK;
}

// Bring the modeline calculator up.
//
// This must happen before the first GroovySwitchresResolve(), not inside SessionOpen():
// resolve answers UNREADY while switchres is down, and UNREADY is a "retry later" that does
// not open a session - so initialising it inside SessionOpen deadlocks and nothing ever
// streams. Idempotent, so calling it every frame costs a bool test.
static bool EnsureSwitchres()
{
	if (bSwitchresUp) return true;

	GroovyLogSetLevel(nGroovyLogLevel);

	if (!GroovySwitchresInit()) {
		SetState("switchres init failed");
		return false;
	}
	bSwitchresUp = true;
	return true;
}

static bool SessionOpen()
{
	if (bSessionOpen) return true;

	GroovyLogSetLevel(nGroovyLogLevel);

	if (!EnsureSwitchres()) return false;

	// NLC is RGB888 only, and CmdInit rejects any other pixel format rather than streaming a
	// picture the core cannot decode. GroovyConfigApply() normalises the pair on every path
	// that can set either value, so this should never fire; it is here because the cost of
	// being wrong is a refused session with no obvious cause.
	if (nGroovyCodec == GROOVY_CODEC_NLC && nGroovyRgbMode != RGB_888) {
		GroovyLogAlways("NLC requires RGB888 - correcting rgbMode %d at session open",
		                (int)nGroovyRgbMode);
		nGroovyRgbMode = RGB_888;
	}

	// Everything below must precede CmdInit.
	gm_set_log_sink(GroovyClientLogSink);

	// The client's verbose level 2 is PER FRAME by design - it emits an "echo"/"ACK" pair for
	// every blit (groovymister.cpp DiffTimeRaster/getACK). A 4271-line file from one session
	// showed what that does to a log meant to be event-driven. Cap it at 1 whenever the file
	// sink is on: level 1 still gives the ~2s RIO telemetry summary, which is the part with
	// diagnostic value, and the debugger output is unaffected.
	INT32 nClientVerbose = nGroovyLogLevel;
	if (bGroovyLogToFile && nClientVerbose > 1) {
		nClientVerbose = 1;
		GroovyLog(GROOVY_LOG_ERROR,
		          "client verbosity capped at 1 for the file log (level 2 is per-frame); "
		          "turn file logging off to use level 2");
	}
	gm.setVerbose((uint8_t)nClientVerbose);

	if (bGroovyUseInputs) {
		gm.BindInputs(_TtoA(szGroovyHost), (uint16_t)nGroovyInputPort);
		gm.setInputCaps(GM_CAP_INPUTS_V2);
	}

	if (nGroovyCodec == GROOVY_CODEC_NLC) {
		gm.setNlcPack((uint8_t)nGroovyNlcPack);
		gm.setNearLevel((uint8_t)nGroovyNearLevel);
	}
	gm.setAutoReconnect((uint8_t)(bGroovyAutoReconnect ? 1 : 0));

	// Opt in to the core's idle timeout. Without this the core never closes a silent session,
	// and a process killed without sending CMD_CLOSE - which is how our launcher ends a match -
	// leaves its last frame on the user's CRT until something else reconnects. Opting in is the
	// promise GroovyKeepAlive() keeps. It is independent of setInputCaps() and rides the same
	// CMD_INIT caps byte.
	gm.setKeepAlive(1);

	// Audio is fixed for the life of the session.
	INT32 nSoundRate = 0, nSoundChan = 0;
	if (nGroovyAudioMode != GROOVY_AUDIO_OFF && bAudOkay && nBurnSoundRate > 0) {
		if (GroovyAudioRateOkay(nBurnSoundRate)) {
			nSoundRate = nBurnSoundRate;
			nSoundChan = 2;					// FBNeo always produces interleaved stereo
		} else {
			GroovyLog(GROOVY_LOG_ERROR,
			          "audio disabled: %dHz is not one of 22050/44100/48000, and the rate is "
			          "baked into CMD_INIT", (int)nBurnSoundRate);
		}
	}

	GroovyLog(GROOVY_LOG_ERROR, "connecting to %s:%d (codec %d, rgb %d, mtu %d, audio %d/%d)",
	          _TtoA(szGroovyHost), (int)nGroovyPort, (int)nGroovyCodec, (int)nGroovyRgbMode,
	          (int)nGroovyMtu, (int)nSoundRate, (int)nSoundChan);

	const int nRet = gm.CmdInit(_TtoA(szGroovyHost), (uint16_t)nGroovyPort,
	                            (int)nGroovyCodec, (uint32_t)nSoundRate, (uint8_t)nSoundChan,
	                            (uint8_t)nGroovyRgbMode, (uint16_t)nGroovyMtu);
	if (nRet != 0) {
		SetState("no ACK from %s - wrong IP, core not running, firewall, or MTU",
		         _TtoA(szGroovyHost));
		GroovyLog(GROOVY_LOG_ERROR, "CmdInit failed (%d): %s", nRet, szState);
		// CmdClose is idempotent and safe even when CmdInit never succeeded; calling it
		// keeps the inputs socket tidy.
		gm.CmdClose();
		NoteConnectFailed();
		return false;
	}

	bSessionOpen   = true;
	bEverConnected = true;
	bRawCloseSent  = false;		// this session gets its own close
	nLastConnectFailMs = 0;

	// Fresh liveness state, and re-arm the keepalive clock so a slow CmdInit does not look like
	// 2s of silence the moment we connect.
	bSessionDead    = false;
	nLastEchoSeen   = 0;
	nNoEchoBlits    = 0;
	nBadEchos       = 0;
	nLastReconEpoch = gm.reconnectEpoch();
	nLastWireMs     = (UINT32)timeGetTime();

	// CmdInit does not do this for us - nBlitFrame is ours.
	ResetClientFrameState();

	// Remember where we connected, so a mid-session edit to the host cannot aim the close
	// somewhere other than where we opened.
	strncpy(szOpenHost, _TtoA(szGroovyHost), sizeof(szOpenHost) - 1);
	szOpenHost[sizeof(szOpenHost) - 1] = '\0';
	nOpenPort = nGroovyPort;
	bHaveModeline  = false;
	nBlitFrame     = 0;
	nOpenCodec     = nGroovyCodec;
	nOpenRgbMode   = nGroovyRgbMode;
	nOpenMtu       = nGroovyMtu;
	nOpenSoundRate = nSoundRate;
	nOpenSoundChan = nSoundChan;

	if (bGroovyUseInputs) {
		gm.ResendInputSubscribe();		// UDP-loss insurance; also what fixes reconnects
	}

	// getInputCaps() reports what the core granted, not what we asked for. A core older than
	// version 2 drops the whole caps byte and the keepalive promise with it. That is harmless,
	// since such a core has no idle timeout either, but this is the only way to tell.
	if (!(gm.getInputCaps() & GM_CAP_KEEPALIVE)) {
		GroovyLogAlways("keepalive opt-in not granted (caps 0x%02X) - this core will hold the "
		                "session open even if we die", (unsigned)gm.getInputCaps());
	}

	// One bounded line per session, unconditional: an open with no matching close is then readable
	// at a glance without the user having switched file logging on beforehand.
	GroovyLogAlways("session OPEN -> %s:%d (codec=%d rgb=%d caps=0x%02X) [%s]",
	                szOpenHost, (int)nOpenPort, (int)nGroovyCodec, (int)nGroovyRgbMode,
	                (unsigned)gm.getInputCaps(), GroovyBuildStamp());


	// Record the netplay context once, at the top of the log, so a support question about a
	// match can be answered without guessing at how the session was configured.
	if (kNetGame) {
		GroovyLog(GROOVY_LOG_ERROR,
		          "netplay session: %s, %.2fHz, runahead %d, vCountSync %d (GGPO owns the frame clock)",
		          kNetSpectator ? "spectator/replay" : "player",
		          nAppVirtualFps / 100.0, (int)nVidRunahead, (int)EffectiveVCountSync());

		if (nGroovyVCountSync == 0) {
			GroovyLog(GROOVY_LOG_ERROR,
			          "automatic frame delay is not meaningful under an external clock - "
			          "using raster line 1. Set a explicit sync line to override.");
		}

		// Informational only. Never override the user's latency choices mid-match.
		if (nVidRunahead >= 2) {
			GroovyLog(GROOVY_LOG_ERROR,
			          "note: runahead %d costs an extra emulated frame plus a full state "
			          "save/load per displayed frame, on the same thread as the encode. If the "
			          "frame cost readout looks high, this and the codec are the two knobs.",
			          (int)nVidRunahead);
		}
	}

	SetState("connected");
	return true;
}

// ---------------------------------------------------------------------------
// Raw one-byte commands, sent on a socket of our own
// ---------------------------------------------------------------------------

#ifdef _WIN32
 #define GROOVY_SOCK         SOCKET
 #define GROOVY_SOCK_INVALID INVALID_SOCKET
 #define GroovySockClose(s)  ::closesocket(s)
 #define GroovySockError()   ((int)::WSAGetLastError())
#else
 #define GROOVY_SOCK         int
 #define GROOVY_SOCK_INVALID (-1)
 #define GroovySockClose(s)  ::close(s)
 #define GroovySockError()   (errno)
#endif

#define GROOVY_CMD_CLOSE      1
#define GROOVY_CMD_GET_STATUS 5

// Send a 1-byte command on a fresh socket, never the client's. Returns 1 if the stack accepted it,
// 0 if the send failed, -1 if we could not even open a socket. pszWhat only labels the log line.
//
// Neither of the client's own one-byte senders works here on Windows. CmdInit creates its socket
// with WSA_FLAG_REGISTERED_IO and then connects it; CmdSendClose() calls plain sendto() on that
// socket and discards the return value, which fails every time because RIO sockets are not
// supported alongside the standard Winsock calls. CmdSendKeepAlive() uses the RIO send path
// properly, but a keepalive fires precisely when we are not blitting, which is when nothing is
// draining the send completion queue; a long enough idle stretch fills it and RIOSend then fails
// silently. Both are correct on POSIX, where the socket is plain and unconnected.
//
// A fresh socket avoids all of it: not RIO-registered, not connected, nothing being deregistered
// underneath it, and blocking, so sendto returns only once the stack owns the datagram. Winsock is
// up for the whole process from main.cpp's WSAStartup, independent of the client's refcount, so
// this still works after teardownVideo() has called WSACleanup(). The core accepts these from any
// source: it switches on byte 0 and never looks at the sender.
static int GroovySendRawCmd(const char* pszHost, int nPort, char cCmd, const char* pszWhat)
{
	if (pszHost == NULL || pszHost[0] == '\0') return -1;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons((unsigned short)nPort);
	addr.sin_addr.s_addr = inet_addr(pszHost);
	if (addr.sin_addr.s_addr == INADDR_NONE) {
		GroovyLogAlways("%s: '%s' is not a usable address", pszWhat, pszHost);
		return -1;
	}

	const GROOVY_SOCK s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == GROOVY_SOCK_INVALID) {
		GroovyLogAlways("%s: could not open a socket (error %d)", pszWhat, GroovySockError());
		return -1;
	}

	const int nRet = (int)::sendto(s, &cCmd, 1, 0, (struct sockaddr*)&addr, sizeof(addr));
	if (nRet != 1) {
		GroovyLogAlways("%s: sendto failed (ret %d, error %d)", pszWhat, nRet, GroovySockError());
	}

	GroovySockClose(s);
	return (nRet == 1) ? 1 : 0;
}

// Exactly one CMD_CLOSE per session, latched on bRawCloseSent.
//
// Repeating an unacknowledged datagram is the obvious hardening and it gains nothing here: the
// core ignores a close that arrives with no session open, so the second and third are discarded.
//
// The residual risk we accept is a close landing mid-payload, where it is dispatched through the
// UDP-loss recovery path and lost if the compressed frame length happens to be 1 (mod 1472). That
// is roughly 0.07% of frames.

void GroovySessionClose(const char* pszReason)
{
	// Reached from several exit paths, some of them more than once. Everything below is
	// idempotent, and it must stay fast: on the MENU_QUIT path this runs inside DrvExit(),
	// which Fightcade executes BEFORE QuarkEnd() -> ggpo_close_session() (scrn.cpp:945-952).
	// Anything slow here delays the match result reaching the server.
	const UINT64 nStart = GroovyTickNow();
	const bool bWasOpen = bSessionOpen;

	// Logged on the way in, unconditionally, and before the bWasOpen branch below. "Ran and found
	// no session" and "never ran at all" are different diagnoses - the first is a bug here, the
	// second means the process was killed and nothing in-process can help - and a trace inside the
	// branch cannot tell them apart. GroovyLogAlways rather than GroovyLog, so the record survives
	// the user not having switched file logging on; it is a handful of lines per process run.
	GroovyLogAlways("close: entered (%s) - sessionOpen=%d clientConnected=%d everConnected=%d [%s]",
	                pszReason ? pszReason : "unspecified",
	                (int)bWasOpen, (int)(gm.isConnected() ? 1 : 0), (int)bEverConnected,
	                GroovyBuildStamp());

	// No more frames may re-open this session. Set BEFORE anything below can yield, and before
	// the teardown, because the whole failure mode is a frame slipping in behind us.
	bShutdownRequested = true;

	// Latched: the core would treat extra datagrams as extra closes. See bRawCloseSent.
	//
	// Aimed at szOpenHost/nOpenPort - what SessionOpen() actually connected to - not at the live
	// config, which the settings dialog or a mid-session edit could have moved underneath us.
	if (bEverConnected && !bRawCloseSent) {
		bRawCloseSent = true;
		if (GroovySendRawCmd(szOpenHost, nOpenPort, GROOVY_CMD_CLOSE, "close") > 0) {
			GroovyLogAlways("close: raw CMD_CLOSE accepted by stack -> %s:%d",
			                szOpenHost, (int)nOpenPort);
		} else {
			GroovyLogAlways("close: raw CMD_CLOSE FAILED - the core will hold the last frame");
		}
	}

	// gm.CmdSendClose() is not called as well: it is a no-op on Windows, which is why
	// GroovySendRawCmd() exists. CmdClose() inside SessionClose() still runs, because we need its
	// local teardown of the RIO buffers, socket and WSACleanup, and there is no entry point that
	// does that without also posting its own CMD_CLOSE. That post is the one that never arrives,
	// so the core sees exactly one close either way.
	SessionClose("requested");
	nLastConnectFailMs = 0;
	bConnectGaveUp     = false;
	if (bSwitchresUp) {
		GroovySwitchresExit();
		bSwitchresUp = false;
	}
	SetState("%s", bGroovyEnabled ? "idle" : "disabled");

	// Timed because it sits on Fightcade's shutdown path. If this is ever more than a
	// millisecond or two, it is delaying the match result and needs looking at.
	GroovyLogAlways("close: done (%s) in %.2fms",
	                pszReason ? pszReason : "unspecified", GroovyTickMsSince(nStart));
}

// Called after every blit. Decides whether the core is still there. See the block comment on
// nLastEchoSeen for why "did the echo change" is not a sufficient test on its own.
static void CheckSessionAlive()
{
	if (bSessionDead) return;

	// An internal reconnect resets the core's counters, so treat it as a fresh start rather than
	// reading the discontinuity as death. This is what reconnectEpoch() is for.
	const UINT32 nEpoch = gm.reconnectEpoch();
	if (nEpoch != nLastReconEpoch) {
		GroovyLog(GROOVY_LOG_ERROR, "client reconnected internally (epoch %u) - modeline replayed, "
		          "frame counters reset", nEpoch);
		nLastReconEpoch = nEpoch;
		nLastEchoSeen   = 0;
		nNoEchoBlits    = 0;
		nBadEchos       = 0;

		// The client's internal reconnect never passes through SessionOpen(), so this is the only
		// place that reset happens for it - and it re-inits without clearing fpga.* either.
		ResetClientFrameState();
		return;
	}

	const UINT32 nEcho = gm.fpga.frameEcho;

	// The echo acknowledges a frame we sent, so it may lag us by pipeline depth but can never
	// meaningfully lead us. Require a few in a row, so one corrupt or reordered datagram cannot
	// kill a healthy session.
	if (nEcho > nBlitFrame + GROOVY_ECHO_SLACK) {
		nBadEchos++;
	} else {
		nBadEchos = 0;
		if (nEcho != nLastEchoSeen) {
			nLastEchoSeen = nEcho;
			nNoEchoBlits  = 0;
		} else {
			nNoEchoBlits++;		// not advancing: the core has gone quiet
		}
	}

	const bool bGarbage = (nBadEchos    >= GROOVY_BAD_ECHOS);
	const bool bStale   = (nNoEchoBlits >= GROOVY_DEAD_BLITS);
	if (!bGarbage && !bStale) return;

	bSessionDead = true;
	GroovyLogAlways("session DEAD: %s (echo %u, sent %u) - no further blits until reconnect",
	                bGarbage ? "core returned implausible frame echoes"
	                         : "core stopped acknowledging",
	                nEcho, nBlitFrame);

	// Hand control to the slower back-off. The client's watchdog retries CmdInit every second and
	// each failed attempt blocks on getACK, which stutters a live match once a second for a MiSTer
	// that may be gone for good. Largely belt-and-braces, since CmdClose() clears m_initHost and
	// the watchdog needs that too.
	gm.setAutoReconnect(0);

	// Closes our side and stops GroovyFrameSync() calling WaitSync, which is what keeps an
	// implausible echo out of its spin loop.
	SessionClose("core stopped responding");

	// Take the normal back-off. Without it the next frame sees bSessionOpen false with
	// nLastConnectFailMs at 0, reads that as "never failed, retry now", and re-inits immediately.
	// This gives 5s offline and stands down entirely in netplay, matching the connect path.
	NoteConnectFailed();
}

void GroovyKeepAlive()
{
	// Nothing to hold open, or we deliberately closed it.
	if (!bSessionOpen || bShutdownRequested) return;

	// The gate is elapsed wire time, not a fixed period. nLastWireMs is stamped by every blit and
	// every audio frame, so during emulation this test never passes and no extra packet goes out.
	// Callers must poll faster than the threshold; polling at it would let a tick land just under
	// and defer the send by a whole extra period.
	const UINT32 nNow = (UINT32)timeGetTime();
	if ((UINT32)(nNow - nLastWireMs) < GROOVY_KEEPALIVE_IDLE_MS) return;

	nLastWireMs = nNow;		// stamp first: a failed send must not retry every tick

	// CMD_GET_STATUS is one byte with no side effects, and any datagram resets the core's
	// activity timer.
	GroovySendRawCmd(szOpenHost, nOpenPort, GROOVY_CMD_GET_STATUS, "keepalive");

	// Not logged per send: at one every 2s an idle session would fill the log. The
	// once-per-transition line is enough to show the mechanism is alive.
	GroovyLogOnChange(GROOVY_LOG_INFO, GROOVY_LOGKEY_KEEPALIVE,
	                  "keepalive: holding the session open while idle (%d ms threshold)",
	                  GROOVY_KEEPALIVE_IDLE_MS);
}

bool GroovyIsStreaming()
{
	return bStreaming;
}

// Module-internal (groovy_internal.h): what input polling gates on.
bool GroovyInternalIsConnected()
{
	return bSessionOpen;
}

bool GroovyWants32Bit()
{
	// Decided during VidInit(), long before a session exists, so this must not depend on one.
	// RGB565 on the wire suits FBNeo's existing 16bpp render, where the pack is a straight memcpy,
	// so only RGB888 asks for the depth change.
	//
	// bDrvOkay is load-bearing, not defensive. Netplay calls MediaInit() before DrvInit(), so it
	// runs a full VidInit() with no driver loaded - something offline never does, because DrvInit
	// suppresses it by faking bVidOkay. Without this guard the render depth changes during that
	// init too, where the corresponding guards in dx9AltTextureInit are themselves bDrvOkay and
	// therefore dead, silently overriding the user's Force 16-bit setting.
	const bool bWants = (bGroovyEnabled != 0) && bDrvOkay && nGroovyRgbMode == RGB_888;

	GroovyLogOnChange(GROOVY_LOG_ERROR, GROOVY_LOGKEY_VIDEOINIT,
	                  "video init: bDrvOkay=%d rgbMode=%d -> render depth %s",
	                  (int)(bDrvOkay ? 1 : 0), (int)nGroovyRgbMode,
	                  bWants ? "32bpp (Groovy)" : "16bpp (stock)");
	return bWants;
}

bool GroovySuppressHostVSync()
{
	// Same bDrvOkay reasoning as GroovyWants32Bit(): never influence the pre-driver VidInit.
	//
	// Netplay is excluded again. Phase 6 removed that term on the argument that a blocking
	// Present() adds jitter to the stream - true, but it was reasoning rather than evidence,
	// and the cost of being wrong is out of proportion: it alters D3D present parameters
	// (COPY vs FLIPEX) with no fallback and, windowed, no error text at all
	// (vid_directx9.cpp:1845-1856). GGPO owns the clock in netplay; leave the user's vsync
	// choice alone there.
	const bool bSuppress = (bGroovyEnabled != 0) && bDrvOkay && !kNetGame;

	GroovyLogOnChange(GROOVY_LOG_ERROR, GROOVY_LOGKEY_VSYNC,
	                  "video init: host vsync suppression %s (bDrvOkay=%d kNetGame=%d)",
	                  bSuppress ? "ON" : "off", (int)(bDrvOkay ? 1 : 0), (int)(kNetGame ? 1 : 0));
	return bSuppress;
}

// The CMD_INIT parameters that cannot change mid-session, so a change here means reconnect.
//
// KNOWN GAP: host and port are not checked, and nothing in the settings dialog's apply path closes
// the session either - so changing the MiSTer's IP while streaming does nothing at all, and we
// keep sending to the address we opened with until the session ends by other means. Left as-is
// deliberately; adding them here is the fix, and it is safe now that nothing else depends on a
// live session's endpoint being immovable.
static bool SessionParamsStale()
{
	return bSessionOpen
	    && (nOpenCodec != nGroovyCodec || nOpenRgbMode != nGroovyRgbMode
	        || nOpenMtu != nGroovyMtu);
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------

static void ReportRefusal(GroovyModeResult eResult, const GroovyModeDecision& dec)
{
	if (eResult == eLastRefusal) return;		// already said so; do not spam the OSD
	eLastRefusal = eResult;
	nGateRefusals++;

	switch (eResult) {
		case GROOVY_MODE_NO_MODELINE:
			SetState("no modeline for %dx%d @ %.2fHz on '%s'", (int)nLastSrcW, (int)nLastSrcH,
			         nLastSrcFps / 100.0, GroovySwitchresActivePreset());
			break;
		case GROOVY_MODE_GATED:
			SetState("mode refused: %s", GateResultText(dec.gate));
			break;
		case GROOVY_MODE_SIZE_MISMATCH:
			SetState("mode refused: switchres stretched %dx%d, we do not scale",
			         (int)nLastSrcW, (int)nLastSrcH);
			break;
		default:
			return;
	}

	GroovyLog(GROOVY_LOG_ERROR, "%s", szState);

	// Say it on screen too. A user who is not told reports a refusal as a hang.
	TCHAR szMsg[128];
	_sntprintf(szMsg, 127, _T("Groovy: %s"), _AtoT(szState));
	szMsg[127] = _T('\0');
	VidSNewShortMsg(szMsg, 0xFF3F3F, 5000, 0);
}

void GroovyFrameReady()
{
	// Watch for the edges that legitimately permit a NEW session, before any early return below
	// can hide them from us. Only these two clear bShutdownRequested:
	//
	//   bDrvOkay  0 -> 1   a driver was loaded. DrvInit() always runs DrvExit() first
	//                      (drv.cpp:149), so this is the natural game-to-game boundary.
	//   bGroovyEnabled 0 -> 1   the user re-enabled Groovy in the settings dialog.
	//
	// Anything else, such as a frame arriving moments after we deliberately closed, must not bring
	// the session back. See bShutdownRequested.
	static bool bPrevEnabled = false;
	static bool bPrevDrvOkay = false;
	const bool bNowEnabled = (bGroovyEnabled != 0);
	const bool bNowDrvOkay = (bDrvOkay != 0);
	if ((bNowEnabled && !bPrevEnabled) || (bNowDrvOkay && !bPrevDrvOkay)) {
		bShutdownRequested = false;
	}
	bPrevEnabled = bNowEnabled;
	bPrevDrvOkay = bNowDrvOkay;

	if (!bGroovyEnabled) {
		if (bSessionOpen) GroovySessionClose("disabled by user");
		return;
	}

	// Netplay is supported. GGPO stays the frame clock - see GroovyPacingActive() and
	// GroovyFrameSync() below for how the two coexist.
	if (!bDrvOkay || pVidImage == NULL) return;	// no game: splash and menu redraws are not ours

	// A close has been requested and no new-session edge has been seen since. Frames still arrive
	// here after OnClose/QuarkEnd - PostQuitMessage and bMediaExit both need the message loop to
	// come round - and re-opening on one of those would put the core straight back into a live
	// session that nothing then closes. That is the whole bug this guard exists for.
	if (bShutdownRequested) return;

	const UINT64 nWorkStart = GroovyTickNow();

	// Before the resolve below, not after: see EnsureSwitchres().
	if (!EnsureSwitchres()) return;

	if (SessionParamsStale()) {
		GroovyLog(GROOVY_LOG_ERROR, "session parameters changed - reconnecting");
		SessionClose("params changed");
	}

	// Do not hammer a failed connect.
	//
	// CmdInit makes THREE blocking getACK(60) calls (groovymister.cpp:748/784/806), so a
	// failed attempt costs up to 180ms - and this runs inside VidDoFrame -> VidFrame ->
	// RunFrame, i.e. inside the frame loop GGPO is timing. Retrying every frame would drop
	// the emulator to single-digit fps and starve the peer of inputs. Back off instead, and
	// in netplay give up for the session entirely rather than stutter a live match.
	if (!bSessionOpen && !SessionRetryDue()) return;

	const INT32 nWidth  = nVidImageWidth;
	const INT32 nHeight = nVidImageHeight;
	const INT32 nFps    = nAppVirtualFps;	// post bForce60Hz and post any netplay override

	nLastSrcW = nWidth; nLastSrcH = nHeight; nLastSrcFps = nFps;
	nLastSrcDepth = nVidImageDepth;

	const GroovyModeDecision dec = GroovySwitchresResolve(nWidth, nHeight, nFps);

	if (dec.result != GROOVY_MODE_OK) {
		if (dec.result != GROOVY_MODE_UNREADY) {
			ReportRefusal(dec.result, dec);
			bStreaming = false;
		}
		return;		// keep local play running; retry when the mode next changes
	}

	if (eLastRefusal != GROOVY_MODE_OK) {
		eLastRefusal = GROOVY_MODE_OK;		// recovered
		GroovyLog(GROOVY_LOG_ERROR, "mode accepted again");
	}

	if (!bSessionOpen && !SessionOpen()) return;

	// Send the modeline from this thread, immediately before the first frame that depends
	// on it. Re-sending an identical one costs a core-side mode reset, so only on change.
	if (!bHaveModeline || !curModeline.SameSignal(dec.modeline)) {
		const Modeline& m = dec.modeline;

		// CmdSwitchres ACKs and retries internally, so a non-zero return means the ACK never
		// landed and the core's modeline state is unconfirmed. Setting bHaveModeline anyway would
		// blit into a session that may discard every frame for the rest of its life: the core
		// leaves PoC_bytes_len at 0 until a switchres is processed, and only a reconnect can
		// restore it. Close and let the normal back-off retry, as CheckSessionAlive() does.
		if (gm.CmdSwitchres(m.pclock, m.hActive, m.hBegin, m.hEnd, m.hTotal,
		                     m.vActive, m.vBegin, m.vEnd, m.vTotal, m.interlace) != 0) {
			GroovyLogAlways("CmdSwitchres FAILED (no ACK after retry) for %dx%d %.3fMHz - "
			                "reconnecting", m.hActive, m.vActive, m.pclock);
			SessionClose("switchres ACK failed");
			NoteConnectFailed();
			return;
		}

		curModeline   = m;
		bHaveModeline = true;
		GroovyLog(GROOVY_LOG_ERROR, "CmdSwitchres %dx%d %.3fMHz hfreq %.2fkHz interlace %d",
		          m.hActive, m.vActive, m.pclock, m.hfreq / 1000.0, (int)m.interlace);
		SetState("streaming %dx%d @ %.2fkHz", m.hActive, m.vActive, m.hfreq / 1000.0);
	}

	// Never write pixels anywhere but getPBufferBlit(). Those buffers are allocated and, on
	// Windows, registered with RIO at CmdInit, so the send path knows their addresses. A memcpy
	// into them is fine; a pointer swap is not.
	char* pBlit = gm.getPBufferBlit(0);
	if (!pBlit) return;

	const bool bFlip = (bGroovyFlip180 != 0)
	                && ((BurnDrvGetFlags() & BDF_ORIENTATION_FLIPPED) != 0);

	const UINT32 nBytes = PackFrame(nVidImageDepth, nGroovyRgbMode,
	                                (const uint8_t*)pVidImage, nVidImagePitch,
	                                nWidth, nHeight, (uint8_t*)pBlit, bFlip);
	if (nBytes == 0) {
		SetState("cannot pack %dbpp source into rgbMode %d", (int)nVidImageDepth, (int)nGroovyRgbMode);
		// On-change only: this condition persists, so a plain log here would be per-frame.
		GroovyLogOnChange(GROOVY_LOG_ERROR, GROOVY_LOGKEY_PACKFAIL, "%s", szState);
		bStreaming = false;
		return;
	}
	nLastBlitBytes = nBytes;

	// The core displays frames in counter order and discards anything behind its current one, so
	// resync if it has moved past us.
	//
	// Bounded, because "the core moved past us" is a one- or two-frame condition. A lead of
	// thousands is not a resync but a counter from a previous session, and adopting it hands
	// WaitSync a spread it turns into minutes of busy-spin. The client zeroes its own fpga.* on
	// every CmdInit now, so this bound should never be the thing that catches it.
	if (gm.fpga.frame > nBlitFrame && gm.fpga.frame - nBlitFrame <= GROOVY_RASTER_MAX_SPREAD) {
		nBlitFrame = gm.fpga.frame;
	}
	nBlitFrame++;

	// field is always 0: with interlace byte 2 the core derives the field cadence from the
	// modeline itself. Feeding it an alternating field index would make it read consecutive
	// frames out of its two field buffers and comb on horizontal motion.
	gm.CmdBlit(nBlitFrame, 0, EffectiveVCountSync(), (uint32_t)nGroovyFdMarginNs, 0);
	nLastWireMs = (UINT32)timeGetTime();	// keepalive: a blit IS activity, so it suppresses one

	nFramesSent++;
	bStreaming = true;

	CheckSessionAlive();

	dPackBlitMs = GroovyTickMsSince(nWorkStart);
	AccumulateFrameCost();
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

void GroovyAudioFrame(INT16* pPcm, INT32 nSamples)
{
	if (!bSessionOpen || !bStreaming) return;
	if (nGroovyAudioMode == GROOVY_AUDIO_OFF) return;
	if (!pPcm || nSamples <= 0) return;
	if (nOpenSoundChan <= 0) return;			// audio was not negotiated for this session

	// Gate on what the CORE says: it drops audio when its OSD Audio option is off, and this
	// status bit is the only way to find out.
	if (!gm.fpga.audio) return;

	UINT32 nBytes = (UINT32)nSamples * 4u;		// s16 stereo == 4 bytes per sample frame

	// CMD_AUDIO's size field is a u16: a 65536-byte drain casts to 0 and puts an empty
	// CMD_AUDIO on the wire, which the core rejects. Cap well under, on a whole number of
	// stereo frames. Steady state is ~2,940 bytes at 44100/60, so this should never bite.
	const UINT32 nMaxBytes = 16384;
	if (nBytes > nMaxBytes) {
		nBytes = nMaxBytes & ~3u;
		GroovyLogOnChange(GROOVY_LOG_INFO, GROOVY_LOGKEY_AUDIOCLAMP,
		                  "audio drain clamped to %u bytes", nBytes);
	}

	char* pAudio = gm.getPBufferAudio();
	if (!pAudio) return;

	memcpy(pAudio, pPcm, nBytes);
	gm.CmdAudio((uint16_t)nBytes);
	nLastWireMs = (UINT32)timeGetTime();	// audio counts as activity too - any datagram does

	nAudioBytesSent += nBytes;

	// Only now that the audio has actually gone to the MiSTer is it safe to silence the PC.
	// Zeroing rather than skipping AudSoundFrame() keeps the DirectSound play cursor moving,
	// which AudSoundCheck()'s underrun logic reads every idle pass.
	if (nGroovyAudioMode == GROOVY_AUDIO_MISTER) {
		memset(pPcm, 0, (size_t)nSamples * 4u);
	}
}

// ---------------------------------------------------------------------------
// Pacing
// ---------------------------------------------------------------------------

bool GroovyPacingActive()
{
	// Netplay excluded: GGPO must remain the clock there.
	return bGroovyEnabled && bSessionOpen && bStreaming && !kNetGame;
}

void GroovyWaitSync()
{
	if (!bSessionOpen) return;

	// Decide the regime BEFORE the call: WaitSync itself can change bStreaming's inputs, and
	// the attribution has to describe the call we are about to make.
	const bool bPacing = GroovyPacingActive();

	// Nothing below this line may spin for seconds.
	//
	// WaitSync's sleep comes from DiffTimeRaster(), which derives a raster distance from the two
	// frame counters and multiplies by m_widthTime. Since m_frameTime = m_widthTime * vTotal, the
	// implied sleep is half a frame period per frame of divergence, and WaitSync accumulates it
	// across iterations - a spread in the thousands never returns. Only the positive direction
	// spins, since a negative dif clamps sleepTime to 0, so one comparison is enough.
	//
	// The client carries the same clamp internally now, so this is no longer the only thing
	// between a desync and a hang. It stays for the diagnostic: the library reports this at
	// verbosity 2 with a per-frame LOG(), and SessionOpen() caps client verbosity at 1 whenever
	// file logging is on, so its report is invisible in exactly the logs a user would send. This
	// one is on-change at error level and survives. Skipping pacing for one frame is a cheap
	// trade against a lock-up.
	const UINT32 nEcho = gm.fpga.frameEcho, nCoreFrame = gm.fpga.frame;
	if (nEcho > nCoreFrame && nEcho - nCoreFrame > GROOVY_RASTER_MAX_SPREAD) {
		GroovyLogOnChange(GROOVY_LOG_ERROR, GROOVY_LOGKEY_RASTERSPREAD,
		                  "raster desync: frameEcho %u vs core frame %u (spread %u > %d) - skipping "
		                  "pacing and resetting counters, WaitSync would have slept ~%.1fs",
		                  nEcho, nCoreFrame, nEcho - nCoreFrame, GROOVY_RASTER_MAX_SPREAD,
		                  (double)(nEcho - nCoreFrame) * 0.5 *
		                      ((nAppVirtualFps > 0) ? (100.0 / (double)nAppVirtualFps) : 0.0167));
		ResetClientFrameState();

		// This call cost nothing. Leave dPendingOverheadMs alone - it may hold overhead from
		// earlier in the frame that AccumulateFrameCost() still has to bill.
		dSyncMs         = 0.0;
		dPaceSleepMs    = 0.0;
		dSyncOverheadMs = 0.0;
		return;
	}

	// Called even on frames where nothing was blitted (paused, or a refused mode): on
	// Windows this is the only drain for the RIO send-completion queue.
	const UINT64 nStart = GroovyTickNow();
	gm.WaitSync();
	dSyncMs = GroovyTickMsSince(nStart);

	// Attribute it. When we own the clock this is deliberate sleep and must not be charged as
	// a cost; when something else does, sleepTime should be 0 and all of it is overhead.
	// See AccumulateFrameCost().
	if (bPacing) {
		dPaceSleepMs    = dSyncMs;
		dSyncOverheadMs = 0.0;
	} else {
		dPaceSleepMs    = 0.0;
		dSyncOverheadMs = dSyncMs;
		dPendingOverheadMs += dSyncMs;
	}
}

void GroovyFrameSync()
{
	// The app-master path: something else (GGPO plus FBNeo's timeGetTime accumulator) owns
	// the frame clock, and we are only here to keep the client healthy.
	//
	// WaitSync must still be called every frame. drainSendCompletions() is private and WaitSync is
	// its only caller; the send completion queue holds 846 entries and a 384x224 RGB888 frame
	// posts around 175 sends, so skipping it fills the queue in about five frames, after which
	// RIOSend fails silently, ACKs stop arriving, and the reconnect watchdog thrashes a good link.
	//
	// It is cheap here because we arrive late. WaitSync sleeps for m_frameTime - m_emulationTime,
	// and m_emulationTime is measured from the end of the previous WaitSync, so calling it once
	// per frame after the host clock has already spent the frame period leaves nothing to sleep;
	// the loop runs one iteration and falls through to the drain.
	//
	// Called even on frames where no blit went out, since a GGPO input stall returns early from
	// RunFrame and we still want fpga.frameEcho fresh for the status readout. Gated on
	// bStreaming rather than bSessionOpen because before the first CmdSwitchres the client has no
	// modeline, leaving m_frameTime and m_vTotal at 0 with nothing for the raster servo to use.
	if (!bSessionOpen || !bStreaming) return;
	if (GroovyPacingActive()) return;	// we own the clock; RunIdle's Groovy branch syncs instead

	GroovyWaitSync();
}

// ---------------------------------------------------------------------------
// Status / self-test
// ---------------------------------------------------------------------------

void GroovyGetStatus(GroovyStatus* pStatus)
{
	if (!pStatus) return;
	memset(pStatus, 0, sizeof(GroovyStatus));

	pStatus->bEnabled   = (bGroovyEnabled != 0);
	pStatus->bConnected = bSessionOpen;
	pStatus->bStreaming = bStreaming;
	strncpy(pStatus->szState, szState, sizeof(pStatus->szState) - 1);

	pStatus->nSrcWidth   = nLastSrcW;
	pStatus->nSrcHeight  = nLastSrcH;
	pStatus->nSrcFpsX100 = nLastSrcFps;
	pStatus->nSrcDepth   = nLastSrcDepth;
	pStatus->nRgbMode    = nGroovyRgbMode;
	pStatus->nBlitBytes  = nLastBlitBytes;

	if (bHaveModeline) {
		pStatus->nModeWidth     = curModeline.hActive;
		pStatus->nModeHeight    = curModeline.vActive;
		pStatus->dHfreqKHz      = curModeline.hfreq / 1000.0;
		pStatus->nInterlaceByte = curModeline.interlace;
	}

	pStatus->nFramesSent     = nFramesSent;
	pStatus->nGateRefusals   = nGateRefusals;
	pStatus->nAudioBytesSent = nAudioBytesSent;

	pStatus->dPackBlitMs      = dPackBlitMs;
	pStatus->dSyncMs         = dSyncMs;
	pStatus->dPaceSleepMs    = dPaceSleepMs;
	pStatus->dSyncOverheadMs = dSyncOverheadMs;
	pStatus->dWorstOverheadMs= dWorstOverheadMs;
	pStatus->dWorstFrameMs   = dWorstFrameMs;
	pStatus->nBudgetMissed = nBudgetMissed;
	pStatus->nCostSamples  = nCostSamples;

	pStatus->bNetGame      = (kNetGame != 0);
	pStatus->bNetSpectator = (kNetSpectator != 0);
	pStatus->nVCountSync   = (INT32)EffectiveVCountSync();

	if (bSessionOpen) {
		pStatus->bCoreAudio   = (gm.fpga.audio != 0);
		pStatus->nFrameskip   = gm.fpga.vgaFrameskip;
		pStatus->bVramSynced  = (gm.fpga.vramSynced != 0);
		pStatus->nInputCaps   = gm.getInputCaps();

		// Raster lag: how far the core's scanout is behind what it last acknowledged.
		const INT32 nLines = (INT32)gm.fpga.vCount - (INT32)gm.fpga.vCountEcho;
		pStatus->nRasterLagLines = nLines;
		if (curModeline.hfreq > 0.0) {
			pStatus->dRasterLagMs = (double)nLines * 1000.0 / curModeline.hfreq;
		}
	}
}

int GroovySelfTest(const char** ppszFirstFail)
{
	return SelfTestPixels(ppszFirstFail);
}
