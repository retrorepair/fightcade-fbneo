// Groovy MiSTer - settings dialog (IDD_GROOVYMISTER).
//
// Also carries the live status readout. Almost every support question about this feature is
// answered by the numbers here - what the source resolution is, what modeline it became,
// whether that is 15kHz or 31kHz, which monitor preset is actually in effect, and how far
// behind the raster we are - so it is worth more than the settings themselves.

#include "burner.h"

#include "groovy_config.h"
#include "groovy_output.h"
#include "groovy_switchres.h"
#include "groovy_log.h"

#include <stdio.h>

static HWND hGroovyDlg = NULL;
static bool bInSetup   = false;		// suppress change handling while we populate

#define GM_TIMER_STATUS   1
#define GM_TIMER_PERIOD 250

// ---------------------------------------------------------------------------
// Combobox contents. The item index maps to the stored value via these tables, so a
// dropdown never has to know the wire encoding.
// ---------------------------------------------------------------------------

static const INT32 nCodecValues[] = { GROOVY_CODEC_NLC, GROOVY_CODEC_LZ4, GROOVY_CODEC_RAW };
static const TCHAR* const szCodecNames[] = {
	_T("NLC (best quality)"), _T("LZ4 (any core)"), _T("RAW (debug)")
};

static const INT32 nPackValues[] = { GROOVY_NLC_PACK_RICE, GROOVY_NLC_PACK_TILED };
static const TCHAR* const szPackNames[] = { _T("Rice (rbf_rice_r3+)"), _T("TILED (any NLC core)") };

static const INT32 nNearValues[] = { 0, 1, 2, 3 };
static const TCHAR* const szNearNames[] = {
	_T("Lossless"), _T("Near +/-1"), _T("Near +/-2"), _T("Near +/-3")
};

static const INT32 nRgbValues[] = { GroovyMiSTer::RGB_888, GroovyMiSTer::RGB_565 };
static const TCHAR* const szRgbNames[] = { _T("RGB888 (32bpp)"), _T("RGB565 (16bpp)") };

static const INT32 nMtuValues[] = { 1500, 3800 };
static const TCHAR* const szMtuNames[] = { _T("1500"), _T("3800 (jumbo)") };

static const INT32 nAudioValues[] = { GROOVY_AUDIO_OFF, GROOVY_AUDIO_BOTH, GROOVY_AUDIO_MISTER };
static const TCHAR* const szAudioNames[] = { _T("PC only"), _T("MiSTer + PC"), _T("MiSTer only") };

static const INT32 nLogValues[] = { 0, 1, 2 };
static const TCHAR* const szLogNames[] = { _T("0"), _T("1"), _T("2") };

static void FillCombo(int nCtrl, const TCHAR* const* pszNames, const INT32* pnValues,
                      int nCount, INT32 nCurrent)
{
	HWND h = GetDlgItem(hGroovyDlg, nCtrl);
	SendMessage(h, CB_RESETCONTENT, 0, 0);
	int nSel = 0;
	for (int i = 0; i < nCount; i++) {
		SendMessage(h, CB_ADDSTRING, 0, (LPARAM)pszNames[i]);
		if (pnValues[i] == nCurrent) nSel = i;
	}
	SendMessage(h, CB_SETCURSEL, nSel, 0);
}

static INT32 ReadCombo(int nCtrl, const INT32* pnValues, int nCount, INT32 nFallback)
{
	const int nSel = (int)SendMessage(GetDlgItem(hGroovyDlg, nCtrl), CB_GETCURSEL, 0, 0);
	if (nSel == CB_ERR || nSel < 0 || nSel >= nCount) return nFallback;
	return pnValues[nSel];
}

// How many pixel formats the given codec can carry. NLC is RGB888 only, so under it the combo
// offers one entry rather than a second that CmdInit would refuse. A Win32 combo box cannot grey
// out individual items, so the list itself is the constraint.
static int RgbCountFor(INT32 nCodec)
{
	return (nCodec == GROOVY_CODEC_NLC) ? 1 : 2;
}

// nRgbValues[0] is RGB888, so the truncated list keeps its ordering and the caller's current
// selection still resolves. A 565 selection carried over from LZ4 lands on RGB888.
static void FillRgbCombo(INT32 nCodec, INT32 nCurrent)
{
	FillCombo(IDC_GM_RGBMODE, szRgbNames, nRgbValues, RgbCountFor(nCodec), nCurrent);
}

// ---------------------------------------------------------------------------
// Populate / read back
// ---------------------------------------------------------------------------

static void DialogToControls()
{
	bInSetup = true;

	CheckDlgButton(hGroovyDlg, IDC_GM_ENABLE,         bGroovyEnabled       ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hGroovyDlg, IDC_GM_CRTCAP,         bGroovyCrtSafetyCap  ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hGroovyDlg, IDC_GM_ROTATEVERT,     bGroovyRotateVertical? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hGroovyDlg, IDC_GM_INPUTS,         bGroovyUseInputs     ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hGroovyDlg, IDC_GM_AUTORECONNECT,  bGroovyAutoReconnect ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hGroovyDlg, IDC_GM_SUPPRESSLOCAL,  bGroovySuppressLocal ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hGroovyDlg, IDC_GM_LOGFILE,        bGroovyLogToFile     ? BST_CHECKED : BST_UNCHECKED);

	SetDlgItemText(hGroovyDlg, IDC_GM_HOST,  szGroovyHost);
	SetDlgItemText(hGroovyDlg, IDC_GM_SRINI, szGroovySwitchresIni);
	SetDlgItemInt (hGroovyDlg, IDC_GM_VCOUNTSYNC, nGroovyVCountSync, FALSE);

	// The preset list is the allowlist itself, so an invalid name simply cannot be chosen
	// here - which is the whole point, given switchres silently falls back to generic_15
	// (15kHz only, no 480p) on any name it does not recognise.
	int nPresetCount = 0;
	const char* const* pszPresets = GroovyPresetList(&nPresetCount);
	HWND hPreset = GetDlgItem(hGroovyDlg, IDC_GM_PRESET);
	SendMessage(hPreset, CB_RESETCONTENT, 0, 0);
	char szCurrent[64];
	GroovyResolvePreset(szCurrent, sizeof(szCurrent));
	int nPresetSel = 0;
	for (int i = 0; i < nPresetCount; i++) {
		SendMessage(hPreset, CB_ADDSTRING, 0, (LPARAM)_AtoT(pszPresets[i]));
		if (!strcmp(pszPresets[i], szCurrent)) nPresetSel = i;
	}
	SendMessage(hPreset, CB_SETCURSEL, nPresetSel, 0);

	FillCombo(IDC_GM_CODEC,   szCodecNames, nCodecValues, 3, nGroovyCodec);
	FillCombo(IDC_GM_NLCPACK, szPackNames,  nPackValues,  2, nGroovyNlcPack);
	FillCombo(IDC_GM_NEAR,    szNearNames,  nNearValues,  4, nGroovyNearLevel);
	FillRgbCombo(nGroovyCodec, nGroovyRgbMode);
	FillCombo(IDC_GM_MTU,     szMtuNames,   nMtuValues,   2, nGroovyMtu);
	FillCombo(IDC_GM_AUDIO,   szAudioNames, nAudioValues, 3, nGroovyAudioMode);
	FillCombo(IDC_GM_LOGLEVEL,szLogNames,   nLogValues,   3, nGroovyLogLevel);

	bInSetup = false;
}

// Returns true if a setting changed that requires the video layer to be rebuilt.
static bool ControlsToConfig()
{
	const INT32 nOldEnabled = bGroovyEnabled;
	const INT32 nOldRgbMode = nGroovyRgbMode;

	bGroovyEnabled        = (IsDlgButtonChecked(hGroovyDlg, IDC_GM_ENABLE)        == BST_CHECKED);
	bGroovyCrtSafetyCap   = (IsDlgButtonChecked(hGroovyDlg, IDC_GM_CRTCAP)        == BST_CHECKED);
	bGroovyRotateVertical = (IsDlgButtonChecked(hGroovyDlg, IDC_GM_ROTATEVERT)    == BST_CHECKED);
	bGroovyUseInputs      = (IsDlgButtonChecked(hGroovyDlg, IDC_GM_INPUTS)        == BST_CHECKED);
	bGroovyAutoReconnect  = (IsDlgButtonChecked(hGroovyDlg, IDC_GM_AUTORECONNECT) == BST_CHECKED);
	bGroovySuppressLocal  = (IsDlgButtonChecked(hGroovyDlg, IDC_GM_SUPPRESSLOCAL) == BST_CHECKED);
	bGroovyLogToFile      = (IsDlgButtonChecked(hGroovyDlg, IDC_GM_LOGFILE)       == BST_CHECKED);

	GetDlgItemText(hGroovyDlg, IDC_GM_HOST,  szGroovyHost,         sizeof(szGroovyHost)  / sizeof(TCHAR));
	GetDlgItemText(hGroovyDlg, IDC_GM_SRINI, szGroovySwitchresIni, sizeof(szGroovySwitchresIni) / sizeof(TCHAR));

	BOOL bOkay = FALSE;
	const UINT nSync = GetDlgItemInt(hGroovyDlg, IDC_GM_VCOUNTSYNC, &bOkay, FALSE);
	if (bOkay) nGroovyVCountSync = (INT32)nSync;

	const int nPresetSel = (int)SendMessage(GetDlgItem(hGroovyDlg, IDC_GM_PRESET), CB_GETCURSEL, 0, 0);
	if (nPresetSel != CB_ERR) {
		int nPresetCount = 0;
		const char* const* pszPresets = GroovyPresetList(&nPresetCount);
		if (nPresetSel >= 0 && nPresetSel < nPresetCount) {
			_tcscpy(szGroovyPreset, _AtoT(pszPresets[nPresetSel]));
		}
	}

	nGroovyCodec     = ReadCombo(IDC_GM_CODEC,    nCodecValues, 3, nGroovyCodec);
	nGroovyNlcPack   = ReadCombo(IDC_GM_NLCPACK,  nPackValues,  2, nGroovyNlcPack);
	nGroovyNearLevel = ReadCombo(IDC_GM_NEAR,     nNearValues,  4, nGroovyNearLevel);
	// Always read the full range: CB_GETCURSEL is bounded by what the list actually holds, and
	// under NLC that is RGB888 alone. GroovyConfigApply() below is the backstop for any other
	// route into these variables.
	nGroovyRgbMode   = ReadCombo(IDC_GM_RGBMODE,  nRgbValues,   2, nGroovyRgbMode);
	nGroovyMtu       = ReadCombo(IDC_GM_MTU,      nMtuValues,   2, nGroovyMtu);
	nGroovyAudioMode = ReadCombo(IDC_GM_AUDIO,    nAudioValues, 3, nGroovyAudioMode);
	nGroovyLogLevel  = ReadCombo(IDC_GM_LOGLEVEL, nLogValues,   3, nGroovyLogLevel);

	// Pushes the log level and the file sink, corrects any invalid codec/pixel-format pair, and
	// re-banners the config in effect.
	GroovyConfigApply();

	// The monitor preset and the switchres INI are baked in at sr_init(), so a change means
	// a rebuild of the calculator.
	GroovySwitchresReconfigure();

	// Both the render depth and the host-vsync suppression are fixed when the D3D device is
	// created, so these two need the video layer rebuilt to take effect at all.
	return (nOldEnabled != bGroovyEnabled) || (nOldRgbMode != nGroovyRgbMode);
}

// ---------------------------------------------------------------------------
// Status readout
// ---------------------------------------------------------------------------

static void UpdateStatus()
{
	GroovyStatus st;
	GroovyGetStatus(&st);

	TCHAR szText[768];
	TCHAR szLine1[192], szLine2[192], szLine3[192], szLine4[192];

	// Netplay changes who owns the frame clock, so say which mode is in effect - it is the
	// first thing that explains the numbers below.
	TCHAR szClock[64];
	if (st.bNetGame) {
		_sntprintf(szClock, 63, _T(" [%s: GGPO clock, sync line %d]"),
		           st.bNetSpectator ? _T("spectator") : _T("netplay"), (int)st.nVCountSync);
	} else if (st.bStreaming) {
		_sntprintf(szClock, 63, _T(" [MiSTer clock, sync line %s]"),
		           st.nVCountSync == 0 ? _T("auto") : _T("explicit"));
	} else {
		szClock[0] = _T('\0');
	}
	szClock[63] = _T('\0');

	_sntprintf(szLine1, 191, _T("%s%s%s"),
	           st.bStreaming ? _T("STREAMING - ") : (st.bConnected ? _T("connected - ") : _T("")),
	           _AtoT(st.szState), szClock);

	if (st.nSrcWidth > 0 && st.nModeWidth > 0) {
		_sntprintf(szLine2, 191,
		           _T("source %dx%d @ %.2fHz (%dbpp)  ->  %dx%d  %.2f kHz  interlace %d  %u bytes/blit"),
		           (int)st.nSrcWidth, (int)st.nSrcHeight, st.nSrcFpsX100 / 100.0, (int)st.nSrcDepth,
		           (int)st.nModeWidth, (int)st.nModeHeight, st.dHfreqKHz,
		           (int)st.nInterlaceByte, st.nBlitBytes);
	} else if (st.nSrcWidth > 0) {
		_sntprintf(szLine2, 191, _T("source %dx%d @ %.2fHz - no modeline in effect"),
		           (int)st.nSrcWidth, (int)st.nSrcHeight, st.nSrcFpsX100 / 100.0);
	} else {
		_tcscpy(szLine2, _T("no frames seen yet"));
	}

	_sntprintf(szLine3, 191,
	           _T("preset '%s'%s   frames %u  refused %u  frameskip %u  vramSynced %s  lag %d lines (%.2f ms)  caps 0x%02X"),
	           _AtoT(GroovySwitchresActivePreset()),
	           GroovySwitchresPresetAsRequested() ? _T("") : _T(" (MISMATCH)"),
	           st.nFramesSent, st.nGateRefusals, st.nFrameskip,
	           st.bConnected ? (st.bVramSynced ? _T("yes") : _T("NO - core lost sync")) : _T("-"),
	           (int)st.nRasterLagLines, st.dRasterLagMs, (unsigned)st.nInputCaps);

	// Frame cost. Under netplay we report rather than reconfigure, so this line is the whole
	// basis for the user's decision about codec and pixel format.
	if (st.nCostSamples > 0 || st.dPackBlitMs > 0.0) {
		const double dFrameMs = 100000.0 / (double)(nAppVirtualFps > 0 ? nAppVirtualFps : 6000);
		// Pacing sleep is deliberate and is not a cost, so it is labelled separately from the
		// sync overhead that actually counts against the budget. See GroovyStatus.
		_sntprintf(szLine4, 191,
		           _T("frame cost: pack+blit %.2fms  sync %s %.2fms  worst %.2fms of %.2fms budget%s"),
		           st.dPackBlitMs,
		           st.bNetGame ? _T("overhead") : _T("(pacing)"),
		           st.bNetGame ? st.dSyncOverheadMs : st.dPaceSleepMs,
		           st.dWorstFrameMs, dFrameMs,
		           (st.nCostSamples && st.nBudgetMissed * 5 >= st.nCostSamples)
		               ? _T("   <-- HIGH") : _T(""));
	} else {
		_tcscpy(szLine4, _T("frame cost: not measured yet"));
	}

	_sntprintf(szText, 767, _T("%s\r\n%s\r\n%s\r\n%s"), szLine1, szLine2, szLine3, szLine4);
	szText[767] = _T('\0');
	SetDlgItemText(hGroovyDlg, IDC_GM_STATUS, szText);
}

static void ShowLog()
{
	const int nCount = GroovyLogLineCount();
	if (nCount <= 0) {
		FBAPopupAddText(PUF_TEXT_DEFAULT, _T("The Groovy log is empty."));
		FBAPopupDisplay(PUF_TYPE_INFO);
		return;
	}

	TCHAR szBuf[GROOVY_LOG_LINES * 96];
	szBuf[0] = _T('\0');
	int nLen = 0;
	for (int i = 0; i < nCount; i++) {
		TCHAR szLine[GROOVY_LOG_LINE_LEN + 4];
		_sntprintf(szLine, GROOVY_LOG_LINE_LEN + 3, _T("%s\n"), _AtoT(GroovyLogLine(i)));
		const int nThis = (int)_tcslen(szLine);
		if (nLen + nThis >= (int)(sizeof(szBuf) / sizeof(TCHAR)) - 1) break;
		_tcscat(szBuf, szLine);
		nLen += nThis;
	}

	FBAPopupAddText(PUF_TEXT_DEFAULT, szBuf);
	FBAPopupDisplay(PUF_TYPE_INFO);
}

static void RunSelfTest()
{
	const char* pszFail = NULL;
	const int nFails = GroovySelfTest(&pszFail);

	TCHAR szMsg[512];
	if (nFails == 0) {
		_tcscpy(szMsg, _T("Groovy self-test passed.\n\n")
		               _T("Pixel packers verified for all four source-depth / wire-format\n")
		               _T("combinations, including the red/blue channel order and the 180 flip."));
	} else {
		_sntprintf(szMsg, 511,
		           _T("Groovy self-test FAILED: %d check(s).\n\nFirst failure:\n%s"),
		           nFails, pszFail ? _AtoT(pszFail) : _T("(unknown)"));
	}
	szMsg[511] = _T('\0');

	FBAPopupAddText(PUF_TEXT_DEFAULT, szMsg);
	FBAPopupDisplay(nFails ? PUF_TYPE_ERROR : PUF_TYPE_INFO);
}

// ---------------------------------------------------------------------------
// Dialog procedure
// ---------------------------------------------------------------------------

static INT_PTR CALLBACK GroovyDialogProc(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	if (Msg == WM_INITDIALOG) {
		hGroovyDlg = hDlg;
		DialogToControls();
		UpdateStatus();
		SetTimer(hDlg, GM_TIMER_STATUS, GM_TIMER_PERIOD, NULL);
		WndInMid(hDlg, hScrnWnd);
		return TRUE;
	}

	if (Msg == WM_TIMER && wParam == GM_TIMER_STATUS) {
		UpdateStatus();
		return TRUE;
	}

	if (Msg == WM_COMMAND) {
		const int nId     = LOWORD(wParam);
		const int nNotify = HIWORD(wParam);

		if (bInSetup) return FALSE;

		// Rebuild the pixel-format list whenever the codec changes, so NLC cannot be paired
		// with RGB565. Selecting NLC while 565 is showing moves the selection to RGB888.
		if (nId == IDC_GM_CODEC && nNotify == CBN_SELCHANGE) {
			const INT32 nCodec = ReadCombo(IDC_GM_CODEC,   nCodecValues, 3, nGroovyCodec);
			const INT32 nRgb   = ReadCombo(IDC_GM_RGBMODE, nRgbValues,   2, nGroovyRgbMode);
			FillRgbCombo(nCodec, nRgb);
			return TRUE;
		}

		if (nId == IDOK && nNotify == BN_CLICKED) {
			const bool bNeedsVideoRebuild = ControlsToConfig();
			KillTimer(hDlg, GM_TIMER_STATUS);
			EndDialog(hDlg, bNeedsVideoRebuild ? 1 : 0);
			return TRUE;
		}

		if (nId == IDCANCEL && nNotify == BN_CLICKED) {
			KillTimer(hDlg, GM_TIMER_STATUS);
			EndDialog(hDlg, 0);
			return TRUE;
		}

		if (nId == IDC_GM_APPLY && nNotify == BN_CLICKED) {
			if (ControlsToConfig()) {
				// Depth and vsync are device-creation properties: without this the change
				// silently does nothing until the next video re-init.
				POST_INITIALISE_MESSAGE;
			}
			UpdateStatus();
			return TRUE;
		}

		if (nId == IDC_GM_SELFTEST && nNotify == BN_CLICKED) { RunSelfTest(); return TRUE; }
		if (nId == IDC_GM_LOG      && nNotify == BN_CLICKED) { ShowLog();     return TRUE; }
	}

	if (Msg == WM_CLOSE) {
		KillTimer(hDlg, GM_TIMER_STATUS);
		EndDialog(hDlg, 0);
		return TRUE;
	}

	if (Msg == WM_DESTROY) {
		hGroovyDlg = NULL;
		return TRUE;
	}

	return FALSE;
}

int GroovyDialogCreate()
{
	const int nOldPause = bRunPause;
	bRunPause = 1;
	AudBlankSound();

	const INT_PTR nRet = FBADialogBox(hAppInst, MAKEINTRESOURCE(IDD_GROOVYMISTER),
	                                  hScrnWnd, (DLGPROC)GroovyDialogProc);

	bRunPause = nOldPause;

	if (nRet == 1) {
		POST_INITIALISE_MESSAGE;	// render depth / vsync changed - rebuild the video layer
	}

	return 0;
}
