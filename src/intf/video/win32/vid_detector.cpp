#include "burner.h"
#include "vid_detector.h"
#include "detector_buffers.h"

// *** Detector data is COMPILED IN, and its absence is otherwise completely silent. ***
//
// detector_buffers.h / detector_loaders.h are generated (src/dep/scripts/detectors.pl). Both ship
// as empty placeholder files, so a build that skips the generator still compiles and runs - it just
// produces a table containing nothing but its terminator. Every lookup below then misses,
// GameDetector::Load() falls through to ST_NONE, and the emulator plays perfectly while silently
// never detecting a round start or a winner. No error, no log, no crash: round counters and RANKED
// MATCH REPORTING simply stop working. That cost a full debugging session to find once.
//
// The generator defines the sentinel, so requiring it converts that into a build failure.
#if !defined(FBNEO_DETECTORS_GENERATED) && !defined(FBNEO_ALLOW_NO_DETECTORS)
 #error "Game detector data has not been generated. Run detectors.pl to generate it, or define FBNEO_ALLOW_NO_DETECTORS to build without detectors (which disables round counters and ranked match reporting)."
#endif

struct DetectorBuffer {
	char name[128];
	const unsigned char *buffer;
	int size;
};

#define ITEM(name,buffer,size) { name,buffer,size },
static DetectorBuffer detector_buffers[] = {
	#include "detector_loaders.h"
	{ "", 0, 0 },
};
#undef ITEM

bool LoadMemoryBufferDetector(MemoryBuffer &buffer, const char *file, bool force_disk)
{
	if (force_disk) {
#ifdef PRINT_DEBUG_INFO
		dprintf(_T("Load DetectorMemoryBuffer from file: %s\n"), ANSIToTCHAR(file,0,0));
#endif
		return LoadMemoryBufferFromFile(buffer, file);
	}
	else {
#ifdef PRINT_DEBUG_INFO
		dprintf(_T("Load DetectorMemoryBuffer from internal buffer: %s\n"), ANSIToTCHAR(file,0,0));
#endif
		for (int i = 0; detector_buffers[i].size > 0; i++) {
			if (!_stricmp(detector_buffers[i].name, file)) {
				buffer.Init(detector_buffers[i].buffer, detector_buffers[i].size);
				break;
			}
		}
#ifdef PRINT_DEBUG_INFO
		if (buffer.len == 0) {
			dprintf(_T("Can't load DetectorMemoryBuffer: %s\n"), ANSIToTCHAR(file,0,0));
		}
#endif
		return buffer.len > 0;
	}
	return false;
}
