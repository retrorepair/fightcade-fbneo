// Groovy MiSTer - user-facing settings.
//
// Include AFTER burner.h (needs INT32 / TCHAR / MAX_PATH), like the other burner headers.
// Deliberately contains no socket headers: groovymister.h pulls in <winsock2.h> and main.cpp
// includes the Winsock 1.1 <winsock.h>, which MSVC will not tolerate in one translation unit.
//
// Every variable here is persisted verbatim by cona.cpp's VAR()/STR() macros, which
// stringify the identifier - so the INI key in config/fcadefbneo.ini is literally the C name.

#ifndef GROOVY_CONFIG_H
#define GROOVY_CONFIG_H

// nGroovyAudioMode
#define GROOVY_AUDIO_OFF        0	// audio stays on the PC only
#define GROOVY_AUDIO_BOTH       1	// stream to the MiSTer and keep the PC output
#define GROOVY_AUDIO_MISTER     2	// stream to the MiSTer, silence the PC output

// nGroovyCodec - CmdInit's lz4Frames argument
#define GROOVY_CODEC_RAW        0
#define GROOVY_CODEC_LZ4        1	// safe on every Groovy core
#define GROOVY_CODEC_NLC        7	// our default; needs an NLC-capable core, RGB888 only

// nGroovyNlcPack
#define GROOVY_NLC_PACK_TILED   1
#define GROOVY_NLC_PACK_RICE    2	// needs rbf_rice_r3 or newer - older cores show GARBAGE, silently

extern INT32 bGroovyEnabled;			// master switch, default off
extern TCHAR szGroovyHost[64];			// MiSTer IP; direct GbE link strongly recommended
extern INT32 nGroovyPort;				// 32100, video + control + ACK
extern TCHAR szGroovyPreset[32];		// switchres monitor preset - the most consequential setting
extern TCHAR szGroovySwitchresIni[MAX_PATH];	// optional; required by the custom/lcd presets
extern INT32 nGroovyCodec;
extern INT32 nGroovyRgbMode;			// 0 RGB888 (default), 2 RGB565; NLC forces RGB888
extern INT32 nGroovyMtu;				// 1500, or 3800 with OSD Jumbo frames on
extern INT32 bGroovyCrtSafetyCap;		// CRT envelope cap, on by default
extern INT32 nGroovyVCountSync;			// 0 = automatic frame delay (valid: we are synchronous)
extern INT32 nGroovyFdMarginNs;			// frame-delay safety headroom, nanoseconds
extern INT32 nGroovyAudioMode;
extern INT32 bGroovyUseInputs;			// consume MiSTer pads
extern INT32 nGroovyInputPort;			// 32101, inputs + rumble
extern INT32 bGroovySuppressLocal;		// headless cabinet: skip the local present
extern INT32 bGroovyAutoReconnect;		// client ACK watchdog; disarmed once we declare a session dead
extern INT32 nGroovyLogLevel;			// setVerbose 0-2
extern INT32 bGroovyLogToFile;			// also write config/groovy.log; default OFF
extern INT32 bGroovyRotateVertical;		// 0 = send native scanout (right for a TATE cabinet)
extern INT32 bGroovyFlip180;			// honour BDF_ORIENTATION_FLIPPED
extern INT32 nGroovyNlcPack;
extern INT32 nGroovyNearLevel;			// 0 = lossless (default), 1-3 = near-lossless

// Apply built-in defaults. Call BEFORE ConfigAppLoad() so the INI overrides them.
void GroovyConfigSetDefaults();

// Push the loaded settings into the subsystems that cache them, currently the log level and the
// file sink, and correct any combination the wire cannot carry. Call AFTER ConfigAppLoad(), and
// again whenever the dialog applies changes, so file logging is live from startup rather than only
// once a session opens.
void GroovyConfigApply();

// The compiled-in switchres monitor presets, for the dialog dropdown and validation.
const char* const* GroovyPresetList(int* pnCount);

// Is this an allowlisted preset name? Case-sensitive, lowercase only - matching switchres,
// whose lookup is a raw strcmp, so "Arcade_15" misses and silently becomes generic_15.
bool GroovyPresetIsValid(const char* szName);

// Resolve szGroovyPreset into a validated lowercase ASCII name.
// Returns true if the configured name was usable; false if it was rejected and szOut was
// filled with the default instead (the caller should say so in the log - a silent fallback
// to generic_15 is the "480p never works and nothing explains why" bug).
bool GroovyResolvePreset(char* szOut, int nOutLen);

#endif // GROOVY_CONFIG_H
