// Groovy MiSTer - settings storage, defaults, and monitor-preset validation.

#include "burner.h"
#include "groovy_config.h"
#include "groovy_modeline.h"
#include "groovy_log.h"

#include <ctype.h>

INT32 bGroovyEnabled        = 0;
TCHAR szGroovyHost[64]      = _T("192.168.100.2");
INT32 nGroovyPort           = 32100;
TCHAR szGroovyPreset[32]    = _T("arcade_15_25_31");
TCHAR szGroovySwitchresIni[MAX_PATH] = _T("");
INT32 nGroovyCodec          = GROOVY_CODEC_NLC;
INT32 nGroovyRgbMode        = GroovyMiSTer::RGB_888;
INT32 nGroovyMtu            = 1500;
INT32 bGroovyCrtSafetyCap   = 1;
INT32 nGroovyVCountSync     = 0;
INT32 nGroovyFdMarginNs     = 0;
INT32 nGroovyAudioMode      = GROOVY_AUDIO_MISTER;
INT32 bGroovyUseInputs      = 1;
INT32 nGroovyInputPort      = 32101;
INT32 bGroovySuppressLocal  = 0;
INT32 bGroovyAutoReconnect  = 1;
INT32 nGroovyLogLevel       = 0;
INT32 bGroovyLogToFile      = 0;
INT32 bGroovyRotateVertical = 0;
INT32 bGroovyFlip180        = 1;
INT32 nGroovyNlcPack        = GROOVY_NLC_PACK_RICE;
INT32 nGroovyNearLevel      = 0;

#define GROOVY_DEFAULT_PRESET "arcade_15_25_31"

// switchres's compiled-in presets (handoff doc 4.3). This list is an ALLOWLIST, not a
// convenience: switchres matches with a raw strcmp and falls back to generic_15 - a
// 15kHz-only band that refuses every 31kHz mode - on any name it does not recognise,
// without saying so. A typo would therefore read as "480p is broken".
static const char* const szGroovyPresets[] = {
	// Arcade multi-sync
	"arcade_15_25_31", "arcade_15_31", "arcade_15_25",
	// Arcade single-sync
	"arcade_15", "arcade_15ex", "arcade_25", "arcade_31",
	// Generic / broadcast
	"generic_15", "ntsc", "pal",
	// Specific CRTs
	"d9800", "d9400", "d9200", "k7000", "k7131", "m3129", "m2929",
	"h9110", "polo", "pstar", "ms2930", "ms929", "r666b",
	// PC CRT / VESA GTF
	"pc_31_120", "pc_70_120", "vesa_480", "vesa_600", "vesa_768", "vesa_1024",
	// Ranges come from the user's switchres INI
	"custom", "lcd"
};

void GroovyConfigSetDefaults()
{
	bGroovyEnabled        = 0;
	_tcscpy(szGroovyHost, _T("192.168.100.2"));
	nGroovyPort           = 32100;
	_tcscpy(szGroovyPreset, _T(GROOVY_DEFAULT_PRESET));
	szGroovySwitchresIni[0] = _T('\0');
	nGroovyCodec          = GROOVY_CODEC_NLC;
	nGroovyRgbMode        = GroovyMiSTer::RGB_888;
	nGroovyMtu            = 1500;
	bGroovyCrtSafetyCap   = 1;
	nGroovyVCountSync     = 0;
	nGroovyFdMarginNs     = 0;
	nGroovyAudioMode      = GROOVY_AUDIO_MISTER;
	bGroovyUseInputs      = 1;
	nGroovyInputPort      = 32101;
	bGroovySuppressLocal  = 0;
	bGroovyAutoReconnect  = 1;
	nGroovyLogLevel       = 0;
	bGroovyLogToFile      = 0;
	bGroovyRotateVertical = 0;
	bGroovyFlip180        = 1;
	nGroovyNlcPack        = GROOVY_NLC_PACK_RICE;
	nGroovyNearLevel      = 0;
}

void GroovyConfigApply()
{
	GroovyLogSetLevel(nGroovyLogLevel);
	GroovyLogSetFileOutput(bGroovyLogToFile);

	// NLC is RGB888 only. The FPGA decoder has three plane cores and a fixed three bytes per
	// pixel with no pixel-format input, so RGBA888 loses stream framing on the first line and
	// RGB565 is not encodable at all; CmdInit rejects both outright. Correct the pair here
	// rather than at session open, because GroovyWants32Bit() fixes the render depth during
	// VidInit() and never revisits it - a late correction would leave a 16bpp render packing
	// up to 888 for the whole session.
	//
	// This runs on every path that can change either setting: startup, after ConfigAppLoad(),
	// and the settings dialog's OK and Apply. The value is persisted, so the correction is
	// written back to the ini.
	if (nGroovyCodec == GROOVY_CODEC_NLC && nGroovyRgbMode != GroovyMiSTer::RGB_888) {
		GroovyLogAlways("config: NLC requires RGB888 - rgbMode %d corrected to %d",
		                (int)nGroovyRgbMode, (int)GroovyMiSTer::RGB_888);
		nGroovyRgbMode = GroovyMiSTer::RGB_888;
	}

	// Unconditional, once per process: which binary this is, and whether file logging was on.
	// bGroovyLogToFile is a separate ini key from nGroovyLogLevel - setting the level alone does
	// not produce a file - so record both.
	//
	// bVidVSync is reported here because there is nowhere else it reliably can be.
	// GroovySuppressHostVSync() carries its own breadcrumb, but every call site has the form
	// (bVidVSync && !GroovySuppressHostVSync()), and short-circuit evaluation means that with
	// vsync off the function is never entered and the breadcrumb never fires. The log could
	// report "vsync on" but never "vsync off", which is the case worth knowing: host vsync decides
	// whether Present() blocks for most of the frame, and therefore how much work is left for
	// WaitSync to absorb.
	GroovyLogAlways("startup: %s | logLevel=%d logToFile=%d hostVSync=%d",
	                GroovyBuildStamp(), (int)nGroovyLogLevel, (int)bGroovyLogToFile,
	                (int)bVidVSync);

	// One banner per session recording what was actually in effect, since interpreting a log
	// after the fact starts with knowing the settings.
	if (bGroovyLogToFile) {
		// _TtoA() returns a shared static buffer and memsets it on every call, so two of them in
		// one expression would both yield the second string. Convert into separate buffers.
		char szHost[64], szPreset[64];
		TCHARToANSI(szGroovyHost,   szHost,   sizeof(szHost));
		TCHARToANSI(szGroovyPreset, szPreset, sizeof(szPreset));

		char szBanner[GROOVY_LOG_LINE_LEN];
		_snprintf(szBanner, sizeof(szBanner) - 1,
		          "config: enabled=%d host=%s preset=%s codec=%d rgb=%d mtu=%d nlcPack=%d near=%d "
		          "audio=%d inputs=%d syncline=%d cap=%d",
		          (int)bGroovyEnabled, szHost, szPreset,
		          (int)nGroovyCodec, (int)nGroovyRgbMode, (int)nGroovyMtu,
		          (int)nGroovyNlcPack, (int)nGroovyNearLevel, (int)nGroovyAudioMode,
		          (int)bGroovyUseInputs, (int)nGroovyVCountSync, (int)bGroovyCrtSafetyCap);
		szBanner[sizeof(szBanner) - 1] = '\0';
		GroovyLogBanner(szBanner);
	}
}

const char* const* GroovyPresetList(int* pnCount)
{
	if (pnCount) *pnCount = (int)(sizeof(szGroovyPresets) / sizeof(szGroovyPresets[0]));
	return szGroovyPresets;
}

bool GroovyPresetIsValid(const char* szName)
{
	if (!szName || !szName[0]) return false;

	const int nCount = (int)(sizeof(szGroovyPresets) / sizeof(szGroovyPresets[0]));
	for (int i = 0; i < nCount; i++) {
		if (!strcmp(szName, szGroovyPresets[i])) return true;
	}
	return false;
}

bool GroovyResolvePreset(char* szOut, int nOutLen)
{
	if (!szOut || nOutLen < 2) return false;

	// TCHAR -> ASCII, lowercased. switchres is case-sensitive and we must not hand it
	// whatever casing the user typed.
	char szName[64];
	const char* pszSrc = _TtoA(szGroovyPreset);
	int i = 0;
	for (; pszSrc[i] && i < (int)sizeof(szName) - 1; i++) {
		szName[i] = (char)tolower((unsigned char)pszSrc[i]);
	}
	szName[i] = '\0';

	bool bOkay = GroovyPresetIsValid(szName);

	// custom/lcd take their ranges from the user's INI. With no INI loaded they are an
	// empty monitor, which is worse than a wrong one: it refuses everything.
	if (bOkay && (!strcmp(szName, "custom") || !strcmp(szName, "lcd"))
	    && szGroovySwitchresIni[0] == _T('\0')) {
		bOkay = false;
	}

	strncpy(szOut, bOkay ? szName : GROOVY_DEFAULT_PRESET, nOutLen - 1);
	szOut[nOutLen - 1] = '\0';
	return bOkay;
}
