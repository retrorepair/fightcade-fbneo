// Groovy MiSTer - switchres wrapper: (width, height, refresh) -> a CRT modeline.
//
// switchres is used purely as a calculator. It is built calc-only (SR_CALC_ONLY), so no real
// host-display backend is linked; the modeline it returns goes to the FPGA over UDP and the PC's
// own monitor is never touched.
//
// This header exposes no switchres types, so FBNeo translation units never need its headers.
// Include AFTER burner.h (needs INT32).

#ifndef GROOVY_SWITCHRES_H
#define GROOVY_SWITCHRES_H

#include "groovy_modeline.h"

enum GroovyModeResult {
	GROOVY_MODE_OK = 0,		// modeline is valid and passed the gate - safe to send
	GROOVY_MODE_UNREADY,	// geometry is transient garbage; retry, and do NOT cache this
	GROOVY_MODE_NO_MODELINE,// switchres could not produce one for this monitor preset
	GROOVY_MODE_GATED,		// a modeline exists but our safety gate refused it
	GROOVY_MODE_SIZE_MISMATCH	// switchres stretched: its active area != our source. See below.
};

struct GroovyModeDecision {
	GroovyModeResult         result;
	GroovyMiSTer::Modeline   modeline;	// valid only when result == GROOVY_MODE_OK
	GroovyMiSTer::GateResult gate;		// why, when result == GROOVY_MODE_GATED

	// Modeline has a constructor, so this type is non-trivial: memset() on it is wrong
	// (and warns). Default to the safe answer - "not ready", so a caller that somehow
	// reads an uninitialised decision does not stream.
	GroovyModeDecision()
		: result(GROOVY_MODE_UNREADY), gate(GroovyMiSTer::GATE_OK) {}
};

// Bring switchres up with the configured monitor preset. Safe to call repeatedly; a second
// call is a no-op unless GroovySwitchresReconfigure() asked for a rebuild.
// Returns false if switchres could not be initialised at all.
bool GroovySwitchresInit();
void GroovySwitchresExit();
bool GroovySwitchresIsReady();

// Tear down and re-init. Call when the monitor preset or switchres INI path changes, since
// both are baked in at init time.
bool GroovySwitchresReconfigure();

// Resolve a source geometry to a modeline decision.
//
// nFpsX100 is FBNeo's fps*100 (i.e. nAppVirtualFps) rather than a double, so the cache key
// is exact integers - no float comparison. Cheap to call every frame: the decision is cached
// internally and only recomputed when the tuple, the RGB mode, or the CRT-cap setting changes.
//
// Returned BY VALUE on purpose. An earlier version handed back a pointer to the internal
// cache, which is a quiet aliasing trap - two calls return the same address, so holding a
// result across a call silently gives you the newer one. The struct is small and this runs
// once per frame; correctness is worth more than the copy.
GroovyModeDecision GroovySwitchresResolve(INT32 nWidth, INT32 nHeight, INT32 nFpsX100);

// Drop the cached decision. Call on session open so a fresh session re-evaluates.
void GroovySwitchresInvalidate();

// For the status readout: the preset actually in effect, per sr_get_state(). If this differs
// from what was requested, a stray switchres.ini in the working directory is the usual cause.
const char* GroovySwitchresActivePreset();
bool        GroovySwitchresPresetAsRequested();

#endif // GROOVY_SWITCHRES_H
