// Groovy MiSTer - modeline representation and the safety gate applied before any
// modeline reaches the FPGA.
//
// Deliberately dependency-free (stdint only, no burner.h, no windows.h, no switchres):
// everything here is constexpr and can be unit-tested standalone. Adapted from the
// PCSX2 Groovy fork's GroovyMiSTerModeline.h, which is the same three-tier gate.

#ifndef GROOVY_MODELINE_H
#define GROOVY_MODELINE_H

#include <stdint.h>

namespace GroovyMiSTer {

// The `interlace` byte of CMD_SWITCHRES. This is OUR choice, not switchres's:
// srm.interlace only tells us the modeline is interlaced, we still pick how to feed it.
enum InterlaceMode {
	INTERLACE_PROGRESSIVE = 0,	// progressive modeline, full frame, field always 0
	INTERLACE_FIELD       = 1,	// interlaced modeline + interlaced framebuffer: half-height fields
	INTERLACE_PROGRESSIVE_FB = 2	// interlaced modeline + progressive framebuffer: full frame, field 0
};

// A CRT modeline, in exactly the shape CmdSwitchres() wants.
struct Modeline {
	double   pclock;			// pixel clock in MHz (switchres reports Hz - divide by 1e6)
	uint16_t hActive, hBegin, hEnd, hTotal;
	uint16_t vActive, vBegin, vEnd, vTotal;
	uint8_t  interlace;			// InterlaceMode
	double   hfreq;				// Hz, informational: surfaces 15kHz vs 31kHz to the user

	Modeline()
		: pclock(0.0), hActive(0), hBegin(0), hEnd(0), hTotal(0)
		, vActive(0), vBegin(0), vEnd(0), vTotal(0), interlace(0), hfreq(0.0) {}

	bool SameSignal(const Modeline& r) const	// hfreq is derived, not part of the signal
	{
		return pclock == r.pclock
			&& hActive == r.hActive && hBegin == r.hBegin && hEnd == r.hEnd && hTotal == r.hTotal
			&& vActive == r.vActive && vBegin == r.vBegin && vEnd == r.vEnd && vTotal == r.vTotal
			&& interlace == r.interlace;
	}
};

// BUFFER_SIZE in the vendored client: sized for 720x576x3. The client derives the stream
// length from whatever modeline we hand it and clamps NOTHING in between, so if this check
// is not here it is nowhere.
const uint32_t MAX_BLIT_BYTES = 1245312u;

// CRT envelope cap. An arcade CRT driven far outside its designed envelope can be damaged -
// the horizontal output stage and flyback are what let go. Same envelope RPCS3 and PCSX2 use.
const uint16_t MAX_SAFE_H_ACTIVE = 1024;
const uint16_t MAX_SAFE_V_ACTIVE = 576;

// Why a modeline was refused. We surface this on the OSD, so "it just doesn't work" is
// never the user's only information.
enum GateResult {
	GATE_OK = 0,
	GATE_MALFORMED,			// zero-sized, or blanking that does not enclose the active area
	GATE_OVER_BLIT_BUDGET,	// would overrun the client's fixed RIO-registered buffer
	GATE_OVER_CRT_ENVELOPE	// beyond what a fixed-frequency CRT should be asked to sync
};

// Structural sanity. A modeline that fails this is malformed, not merely aggressive, and
// would drive the core's PLL into an undefined state, so it is always rejected and must
// never be user-disableable. switchres will not produce one, but a user INI or a bad
// re-sync can.
inline bool IsWellFormed(const Modeline& m)
{
	return m.pclock > 0.0
		&& m.hActive > 0 && m.vActive > 0
		&& m.hBegin >= m.hActive && m.hEnd >= m.hBegin && m.hTotal > m.hEnd
		&& m.vBegin >= m.vActive && m.vEnd >= m.vBegin && m.vTotal > m.vEnd;
}

// Bytes streamed for one blit of this modeline. A true-field stream (interlace == 1) sends
// one half-height field per blit and so costs half; the other two modes send every line.
inline uint32_t BlitBytes(const Modeline& m, uint32_t bytesPerPixel)
{
	const uint32_t lines = (m.interlace == INTERLACE_FIELD)
		? (uint32_t)m.vActive / 2u
		: (uint32_t)m.vActive;
	return (uint32_t)m.hActive * lines * bytesPerPixel;
}

inline bool FitsBlitBuffer(const Modeline& m, uint32_t bytesPerPixel)
{
	return BlitBytes(m, bytesPerPixel) <= MAX_BLIT_BYTES;
}

inline bool IsWithinCrtSafeEnvelope(const Modeline& m)
{
	return m.hActive <= MAX_SAFE_H_ACTIVE && m.vActive <= MAX_SAFE_V_ACTIVE;
}

// The full gate. Well-formedness and the blit budget are structural - they are what stands
// between us and an undefined PLL state or a buffer overrun - so they are never optional.
// Only the CRT envelope follows the user's setting, because that one is a judgement call
// about their display.
inline GateResult GateModeline(const Modeline& m, uint32_t bytesPerPixel, bool bEnforceCrtCap)
{
	if (!IsWellFormed(m))                      return GATE_MALFORMED;
	if (!FitsBlitBuffer(m, bytesPerPixel))     return GATE_OVER_BLIT_BUDGET;
	if (bEnforceCrtCap && !IsWithinCrtSafeEnvelope(m)) return GATE_OVER_CRT_ENVELOPE;
	return GATE_OK;
}

inline const char* GateResultText(GateResult r)
{
	switch (r) {
		case GATE_OK:                 return "ok";
		case GATE_MALFORMED:          return "malformed modeline (blanking does not enclose active area)";
		case GATE_OVER_BLIT_BUDGET:   return "over the client's blit budget - try RGB565 or a smaller mode";
		case GATE_OVER_CRT_ENVELOPE:  return "outside the CRT safety envelope";
	}
	return "unknown";
}

// Bytes per pixel on the wire, by CMD_INIT rgbMode. Kept here so the budget check and the
// pixel packers agree on one definition.
enum RgbMode { RGB_888 = 0, RGB_A888 = 1, RGB_565 = 2 };

inline uint32_t BytesPerPixel(int rgbMode)
{
	switch (rgbMode) {
		case RGB_888:  return 3;
		case RGB_A888: return 4;
		case RGB_565:  return 2;
	}
	return 3;
}

} // namespace GroovyMiSTer

#endif // GROOVY_MODELINE_H
