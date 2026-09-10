// Groovy MiSTer - the sender: session lifecycle, per-frame blit, audio, and raster pacing.
//
// This header is what FBNeo sees. It exposes no socket types and pulls in no system headers:
// groovymister.h, and its <winsock2.h>, is confined to groovy_output.cpp, because main.cpp
// includes the Winsock 1.1 <winsock.h> and MSVC will not tolerate both in one translation unit.
//
// Include AFTER burner.h (needs INT32 / INT16 / TCHAR).

#ifndef GROOVY_OUTPUT_H
#define GROOVY_OUTPUT_H

// ---------------------------------------------------------------------------
// Per-frame hooks
// ---------------------------------------------------------------------------

// Called at the end of VidDoFrame(), once the finished frame is sitting in pVidImage.
// Opens the session lazily on first use, applies a CmdSwitchres when the geometry changes,
// packs the frame and blits it. Safe (and cheap) to call when Groovy is disabled.
//
// Placed in the video path deliberately: GGPO re-simulates rolled-back frames through
// RunFrame(0,0,0), which never calls VidFrame(), so a hook here cannot see a rolled-back
// frame. Same for auto-frameskip mid-frames and netplay skip frames.
void GroovyFrameReady();

// Called from RunIdle(). True when the MiSTer raster should be the frame clock, i.e. when
// FBNeo's own timeGetTime() limiter must stand down. Exactly one pacer, never both.
bool GroovyPacingActive();

// Blocks until the right moment to start the next frame, using the ACK stream to close the
// loop against the real CRT raster.
//
// On Windows this is also the only thing that drains the RIO send-completion queue, so it must be
// called every iteration while connected, including frames where nothing was blitted. Skipping it
// fills the 846-entry queue within a few frames, after which sends fail silently and it looks like
// a dead core.
void GroovyWaitSync();

// The app-master counterpart of GroovyWaitSync(), for when something ELSE owns the frame
// clock - i.e. netplay, where GGPO must stay the clock and starving ggpo_idle is not an
// option (rollbacks and packet transmission both happen inside it).
//
// Call once per displayed frame from the stock RunIdle path, immediately after VidPaint(3).
// No-op when Groovy is pacing, so it and GroovyWaitSync() are mutually exclusive by
// construction. See the implementation for why it costs almost nothing.
void GroovyFrameSync();

// Mirror one frame's audio to the MiSTer. pPcm is signed 16-bit LE interleaved stereo -
// exactly what FBNeo already produces - and nSamples is nBurnSoundLen (sample frames, not
// bytes). No-op unless audio is enabled and the core reports it accepted audio.
//
// Takes a non-const pointer because in "MiSTer only" mode it also silences the host buffer in
// place, keeping that policy here rather than in run.cpp. Silencing rather than skipping
// AudSoundFrame() keeps the DirectSound clock running, which AudSoundCheck() depends on, and the
// mute only happens when audio is genuinely reaching the MiSTer: a dead or refused stream must
// never leave the user with no sound at all.
//
// Call it before AudSoundFrame(). DxSoundFrame() applies the optional low-pass DSP to this same
// buffer in place, and the MiSTer should get the emulator's output, not a filtered copy.
void GroovyAudioFrame(INT16* pPcm, INT32 nSamples);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Hold an idle session open against the core's idle timeout (OSD: Server -> Idle timeout, 5s by
// default). We advertise GM_CAP_KEEPALIVE at CmdInit, which is what permits the core to close us
// for going silent, so honouring it is an obligation rather than an optimisation.
//
// Sends a 1-byte CMD_GET_STATUS, and only once nothing has gone on the wire for a couple of
// seconds, so during emulation it sends nothing at all.
//
// Call it often and cheaply: it is gated internally on elapsed time, and the caller's polling rate
// must be faster than that gate or the worst-case silence roughly doubles. The WM_TIMER in
// scrn.cpp drives it at 250ms, which covers Win32 menus and modal dialogs - the cases where
// FBNeo's frame loop stops entirely.
//
// No-op when no session is open, or after a deliberate close.
void GroovyKeepAlive();

// Tear the session down. Idempotent, safe even if no session was ever opened, and safe to
// call from any exit path including ones that run more than once.
//
// Sends CMD_CLOSE by plain sendto before tearing the RIO queues down, because the client's own
// CmdClose() loses that race on Windows and leaves the core displaying the final frame instead of
// returning to connection-search.
//
// pszReason is recorded in the log so a shutdown says which path it came from - window close,
// match end, driver exit or process exit. Keep this fast: on the MENU_QUIT path it runs
// inside DrvExit(), ahead of the ggpo_close_session() that reports the match result.
void GroovySessionClose(const char* pszReason = 0);

// True once CmdInit + CmdSwitchres have succeeded and frames are going out.
bool GroovyIsStreaming();

// Does a Groovy session want the emulation rendered at 32bpp?
//
// Consulted by dx9AltTextureInit(), which otherwise hardcodes 16bpp. Deliberately independent of
// whether a session is open, because the depth is decided during VidInit(), long before the first
// frame opens one. This answers "do we intend to stream RGB888", not "are we streaming".
bool GroovyWants32Bit();

// Should the host's own vsync be suppressed?
//
// True whenever we intend to stream. With host vsync on, Present() blocks on the PC
// monitor's vblank while GroovyWaitSync() blocks on the CRT's raster - two clocks beating
// against each other, which is the one thing 7.1 says never to do. Like GroovyWants32Bit(),
// this is consulted when the D3D device is created (long before a session exists), so it
// answers "do we intend to pace" rather than "are we pacing".
bool GroovySuppressHostVSync();

// ---------------------------------------------------------------------------
// Status, for the settings dialog and the OSD
// ---------------------------------------------------------------------------

struct GroovyStatus {
	bool  bEnabled;
	bool  bConnected;
	bool  bStreaming;
	char  szState[96];		// human-readable, including the reason when refused

	INT32 nSrcWidth, nSrcHeight;
	INT32 nSrcFpsX100;

	INT32 nModeWidth, nModeHeight;
	double dHfreqKHz;
	INT32 nInterlaceByte;

	INT32 nSrcDepth;		// what the blitter actually gave us
	INT32 nRgbMode;			// what we are putting on the wire
	UINT32 nBlitBytes;

	UINT32 nFramesSent;
	UINT32 nGateRefusals;
	UINT32 nAudioBytesSent;
	bool   bCoreAudio;		// core-side audio actually enabled (fpga.audio)

	// Frame cost. Under netplay we report rather than auto-reconfigure, so these ARE the
	// deliverable - they are what tells the user whether to drop to LZ4 or RGB565.
	double dPackBlitMs;		// pack + encode + post
	double dSyncMs;			// total time in the per-frame WaitSync, whichever regime

	// dSyncMs split by who owns the frame clock. Exactly one is non-zero per frame.
	//
	// When Groovy paces, WaitSync sleeping out the frame period is the job and is most of a
	// frame by construction - charging it made a healthy session read as 296/299 frames over
	// budget. Under an external clock (netplay) sleepTime should be 0, so anything here is
	// genuine overhead and is the figure worth watching.
	double dPaceSleepMs;	// intentional: we own the clock
	double dSyncOverheadMs;	// chargeable: something else does
	double dWorstOverheadMs;// worst dSyncOverheadMs over a ~5s window

	double dWorstFrameMs;	// worst CHARGEABLE cost (packblit + overhead), over that window
	UINT32 nBudgetMissed;	// frames in that window whose chargeable cost exceeded half the frame period
	UINT32 nCostSamples;	// frames measured in that window

	bool   bNetGame;		// GGPO owns the frame clock
	bool   bNetSpectator;
	INT32  nVCountSync;		// the raster line actually being used, after substitution
	UINT32 nFrameskip;		// core reused a framebuffer: we missed a frame
	bool   bVramSynced;		// false = the core lost sync (red screen)
	INT32  nRasterLagLines;	// vCount - vCountEcho
	double dRasterLagMs;
	UINT8  nInputCaps;		// NEGOTIATED caps, not what we asked for
};

void GroovyGetStatus(GroovyStatus* pStatus);

// Run the built-in self-tests (pixel packers + modeline gate). Returns the failure count.
int GroovySelfTest(const char** ppszFirstFail);

// Open the settings dialog (IDD_GROOVYMISTER). Implemented in groovy_dialog.cpp.
int GroovyDialogCreate();

#endif // GROOVY_OUTPUT_H
