// Groovy MiSTer - switchres wrapper. See groovy_switchres.h.

#include "burner.h"

#include "groovy_switchres.h"
#include "groovy_config.h"
#include "groovy_log.h"

#include "switchres_wrapper.h"

#include <stdio.h>
#include <string.h>

using namespace GroovyMiSTer;

static bool bSrReady = false;
static char szSrActivePreset[64] = "";
static bool bSrPresetAsRequested = true;

// Cached decision. The key includes the gate's inputs (RGB mode, CRT cap) as well as the
// geometry, so changing either in the settings dialog re-evaluates instead of leaving the
// user stuck behind a stale refusal - e.g. switching to RGB565 to get under the byte budget
// must actually take effect.
static GroovyModeDecision sDecision;
static bool  bDecisionValid  = false;
static INT32 nKeyWidth = 0, nKeyHeight = 0, nKeyFpsX100 = 0;
static INT32 nKeyRgbMode = -1, nKeyCrtCap = -1;

// Sanity window for the source refresh rate, in fps*100. Deliberately wide: this only
// screens out garbage from a driver that has not settled yet. switchres applies its own,
// much tighter, per-monitor refresh tolerance on top.
#define GROOVY_FPS_MIN  4000
#define GROOVY_FPS_MAX 13000

// ---------------------------------------------------------------------------
// switchres log sinks. These are printf-style and get handed over as void*.
// ---------------------------------------------------------------------------

static void SrLogError(const char* pszFormat, ...)
{
	char szLine[GROOVY_LOG_LINE_LEN];
	va_list args;
	va_start(args, pszFormat);
	vsnprintf(szLine, sizeof(szLine) - 1, pszFormat, args);
	va_end(args);
	szLine[sizeof(szLine) - 1] = '\0';
	GroovyLog(GROOVY_LOG_ERROR, "switchres: %s", szLine);
}

static void SrLogInfo(const char* pszFormat, ...)
{
	if (GroovyLogGetLevel() < GROOVY_LOG_INFO) return;
	char szLine[GROOVY_LOG_LINE_LEN];
	va_list args;
	va_start(args, pszFormat);
	vsnprintf(szLine, sizeof(szLine) - 1, pszFormat, args);
	va_end(args);
	szLine[sizeof(szLine) - 1] = '\0';
	GroovyLog(GROOVY_LOG_INFO, "switchres: %s", szLine);
}

static void SrLogDebug(const char* pszFormat, ...)
{
	if (GroovyLogGetLevel() < GROOVY_LOG_TRACE) return;
	char szLine[GROOVY_LOG_LINE_LEN];
	va_list args;
	va_start(args, pszFormat);
	vsnprintf(szLine, sizeof(szLine) - 1, pszFormat, args);
	va_end(args);
	szLine[sizeof(szLine) - 1] = '\0';
	GroovyLog(GROOVY_LOG_TRACE, "switchres: %s", szLine);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static bool SrBringUp(bool bQuiet);

bool GroovySwitchresInit()
{
	return SrBringUp(false);
}

static bool SrBringUp(bool bQuiet)
{
	if (bSrReady) return true;

	// sr_init() reads any switchres.ini it finds in the working directory. That is a real
	// trap: it silently changes the monitor ranges out from under us, and the symptom is
	// "modes that used to work are refused". We set the monitor AFTER sr_init() so our
	// choice wins regardless, but say something if such a file exists.
	FILE* h = bQuiet ? NULL : fopen("switchres.ini", "r");
	if (h) {
		fclose(h);
		GroovyLog(GROOVY_LOG_ERROR,
		          "note: a switchres.ini exists in the working directory; it was parsed by "
		          "sr_init(). The monitor preset below is still ours, but its ranges may not be.");
	}

	sr_init();
	sr_set_log_callback_error((void*)SrLogError);
	sr_set_log_callback_info((void*)SrLogInfo);
	sr_set_log_callback_debug((void*)SrLogDebug);

	// The monitor preset is the single most consequential setting we expose: it is what
	// stands between the stream and a CRT that cannot sync it. An unrecognised name falls
	// back to generic_15 (15kHz only, no 480p) silently, so it is allowlisted first.
	char szPreset[64];
	const bool bPresetOkay = GroovyResolvePreset(szPreset, sizeof(szPreset));
	if (!bPresetOkay && !bQuiet) {
		GroovyLog(GROOVY_LOG_ERROR,
		          "monitor preset '%s' is not recognised (or is custom/lcd with no INI set) - "
		          "using '%s' instead", _TtoA(szGroovyPreset), szPreset);
	}
	sr_set_monitor(szPreset);

	// "dummy" is the calculate-only display. Never pass anything else - with SR_CALC_ONLY
	// it is the only backend linked anyway, but the intent should be explicit.
	// Returns the display INDEX (0 for the first display), -1 on failure. Not a bool:
	// testing it as one would treat a perfectly good init as an error.
	const int nDisp = sr_init_disp("dummy", NULL);
	if (nDisp < 0) {
		GroovyLog(GROOVY_LOG_ERROR, "sr_init_disp(\"dummy\") failed - no modelines can be generated");
		sr_deinit();
		return false;
	}

	if (szGroovySwitchresIni[0] != _T('\0')) {
		char szIni[MAX_PATH];
		strncpy(szIni, _TtoA(szGroovySwitchresIni), sizeof(szIni) - 1);
		szIni[sizeof(szIni) - 1] = '\0';
		sr_load_ini(szIni);
		GroovyLog(GROOVY_LOG_ERROR, "loaded switchres INI '%s'", szIni);
	}

	// Report the preset actually in effect next to the one we asked for. sr_get_state()
	// stores the name verbatim, so this catches a stray-INI override - though note it
	// cannot reveal the unknown-name fallback, which is why we allowlist above.
	sr_state st;
	memset(&st, 0, sizeof(st));
	sr_get_state(&st);
	strncpy(szSrActivePreset, st.monitor, sizeof(szSrActivePreset) - 1);
	szSrActivePreset[sizeof(szSrActivePreset) - 1] = '\0';
	bSrPresetAsRequested = (strcmp(szSrActivePreset, szPreset) == 0);

	if (!bSrPresetAsRequested) {
		GroovyLog(GROOVY_LOG_ERROR, "monitor preset mismatch: asked for '%s', switchres reports '%s'",
		          szPreset, szSrActivePreset);
	} else if (!bQuiet) {
		GroovyLog(GROOVY_LOG_ERROR, "switchres ready, monitor preset '%s'", szSrActivePreset);
	}

	bSrReady = true;
	GroovySwitchresInvalidate();
	return true;
}

void GroovySwitchresExit()
{
	if (!bSrReady) return;
	sr_deinit();
	bSrReady = false;
	szSrActivePreset[0] = '\0';
	GroovySwitchresInvalidate();
}

bool GroovySwitchresIsReady()
{
	return bSrReady;
}

bool GroovySwitchresReconfigure()
{
	GroovySwitchresExit();
	return GroovySwitchresInit();
}

void GroovySwitchresInvalidate()
{
	bDecisionValid = false;
	nKeyWidth = nKeyHeight = nKeyFpsX100 = 0;
	nKeyRgbMode = nKeyCrtCap = -1;
}

const char* GroovySwitchresActivePreset()
{
	return szSrActivePreset[0] ? szSrActivePreset : "(none)";
}

bool GroovySwitchresPresetAsRequested()
{
	return bSrPresetAsRequested;
}

// ---------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------

static GroovyModeDecision Unready()
{
	// Deliberately not cached: the reported geometry is briefly unstable right after a
	// driver init or a mode change, and latching that as a failure would wedge the stream
	// until the game happened to change mode again.
	GroovyModeDecision d;		// constructor already sets UNREADY / GATE_OK
	return d;
}

GroovyModeDecision GroovySwitchresResolve(INT32 nWidth, INT32 nHeight, INT32 nFpsX100)
{
	if (!bSrReady) return Unready();

	if (nWidth <= 0 || nHeight <= 0 || nFpsX100 < GROOVY_FPS_MIN || nFpsX100 > GROOVY_FPS_MAX) {
		return Unready();
	}

	const INT32 nRgbMode = nGroovyRgbMode;
	const INT32 nCrtCap  = bGroovyCrtSafetyCap ? 1 : 0;

	if (bDecisionValid
	    && nKeyWidth == nWidth && nKeyHeight == nHeight && nKeyFpsX100 == nFpsX100
	    && nKeyRgbMode == nRgbMode && nKeyCrtCap == nCrtCap) {
		return sDecision;		// unchanged: a modeline lookup per frame is wasted work
	}

	// switchres ACCUMULATES modes: get_mode() searches every mode added so far and will
	// happily answer a new geometry with a previously added one, scaled (display.cpp:368).
	// Verified: asking for 320x240 and then 320x224 returns the 320x240 mode. We do not
	// scale, so that would either be refused by the size-mismatch gate or - without it -
	// stream the wrong number of pixels into the blit.
	//
	// FBNeo hits this routinely: SFIII toggles 384x224 <-> 496x224 mid-match, Backfire
	// 640x240 <-> 320x240, gaelco2 368x240 <-> 768x240. So whenever the geometry changes,
	// rebuild the calculator to get a mode derived fresh from the monitor preset. This is
	// off the hot path - it only runs when the tuple actually changes.
	if (bDecisionValid && (nKeyWidth != nWidth || nKeyHeight != nHeight)) {
		sr_deinit();
		bSrReady = false;
		if (!SrBringUp(true)) {
			GroovyLog(GROOVY_LOG_ERROR, "switchres re-init failed on geometry change");
			return Unready();
		}
	}

	const double dHz = (double)nFpsX100 / 100.0;

	// Never ask for SR_MODE_INTERLACED. FBNeo hands us whole progressive frames at the field
	// rate, so we want switchres to weigh the progressive band first; if it still lands on an
	// interlaced modeline (a 480-line source on a 15kHz-only monitor, say) we feed that as a
	// PROGRESSIVE FRAMEBUFFER over an interlaced signal - interlace byte 2 - and let the core
	// derive the field cadence. Sending byte 1 would require us to actually emit half-height
	// fields, which we do not.
	sr_mode srm;
	memset(&srm, 0, sizeof(srm));
	const int nRet = sr_add_mode((int)nWidth, (int)nHeight, dHz, 0, &srm);

	sDecision = GroovyModeDecision();

	if (nRet == 0 || srm.width == 0 || srm.height == 0) {
		sDecision.result = GROOVY_MODE_NO_MODELINE;
		GroovyLog(GROOVY_LOG_ERROR,
		          "no modeline for %dx%d @ %.2fHz on monitor '%s' - the source is outside this "
		          "monitor's ranges", (int)nWidth, (int)nHeight, dHz, GroovySwitchresActivePreset());
	} else {
		Modeline& m = sDecision.modeline;
		m.pclock  = (double)srm.pclock / 1000000.0;	// switchres reports Hz, Groovy wants MHz
		m.hActive = (uint16_t)srm.width;
		m.hBegin  = (uint16_t)srm.hbegin;
		m.hEnd    = (uint16_t)srm.hend;
		m.hTotal  = (uint16_t)srm.htotal;
		m.vActive = (uint16_t)srm.height;
		m.vBegin  = (uint16_t)srm.vbegin;
		m.vEnd    = (uint16_t)srm.vend;
		m.vTotal  = (uint16_t)srm.vtotal;
		m.interlace = srm.interlace ? (uint8_t)INTERLACE_PROGRESSIVE_FB : (uint8_t)INTERLACE_PROGRESSIVE;
		m.hfreq   = srm.hfreq;

		const GateResult gate = GateModeline(m, BytesPerPixel(nRgbMode), nCrtCap != 0);
		sDecision.gate = gate;

		// switchres may STRETCH: it can answer a source it cannot match natively with a
		// larger mode plus a scale factor (verified: 1600x1200 comes back as 1600x768i,
		// y_scale 0.640, is_stretched). GroovyMAME would scale into that; we have no scaler
		// in this path, so packing h*w source pixels into an hActive*vActive blit would run
		// past the end of a RIO-registered buffer. Refuse instead.
		// Every realistic FBNeo geometry comes back 1:1, so this should never fire in
		// practice - which is exactly why it must be checked rather than assumed.
		if (gate == GATE_OK
		    && ((INT32)m.hActive != nWidth || (INT32)m.vActive != nHeight)) {
			sDecision.result = GROOVY_MODE_SIZE_MISMATCH;
			GroovyLog(GROOVY_LOG_ERROR,
			          "REFUSED %dx%d @ %.2fHz: switchres returned a stretched %dx%d mode and we "
			          "do not scale. Try a monitor preset that covers this resolution natively.",
			          (int)nWidth, (int)nHeight, dHz, m.hActive, m.vActive);
		} else if (gate == GATE_OK) {
			sDecision.result = GROOVY_MODE_OK;
			GroovyLog(GROOVY_LOG_ERROR,
			          "mode %dx%d @ %.2fHz -> %dx%d%s %.3fMHz, hfreq %.2fkHz, interlace byte %d",
			          (int)nWidth, (int)nHeight, dHz,
			          m.hActive, m.vActive, srm.interlace ? "i" : "p",
			          m.pclock, m.hfreq / 1000.0, (int)m.interlace);
		} else {
			sDecision.result = GROOVY_MODE_GATED;
			GroovyLog(GROOVY_LOG_ERROR,
			          "REFUSED %dx%d @ %.2fHz (modeline %dx%d, %u bytes/blit): %s",
			          (int)nWidth, (int)nHeight, dHz, m.hActive, m.vActive,
			          BlitBytes(m, BytesPerPixel(nRgbMode)), GateResultText(gate));
		}
	}

	nKeyWidth   = nWidth;
	nKeyHeight  = nHeight;
	nKeyFpsX100 = nFpsX100;
	nKeyRgbMode = nRgbMode;
	nKeyCrtCap  = nCrtCap;
	bDecisionValid = true;
	return sDecision;
}
