// Groovy MiSTer - logging.
//
// FBNeo is a GUI app: stdout goes nowhere, so a failed handshake or a silently-substituted
// monitor preset would otherwise be invisible. Everything here also lands in a ring buffer
// the settings dialog can display, which is what makes "it doesn't work" answerable.
//
// Standalone by design (no burner.h, no socket headers) so both the switchres wrapper and
// the sender can use it without dragging include-order problems around.

#ifndef GROOVY_LOG_H
#define GROOVY_LOG_H

#define GROOVY_LOG_ERROR 0	// always emitted: handshake, refusals, state changes
#define GROOVY_LOG_INFO  1	// telemetry
#define GROOVY_LOG_TRACE 2	// full trace

#define GROOVY_LOG_LINES     96
#define GROOVY_LOG_LINE_LEN 256

void GroovyLogSetLevel(int nLevel);
int  GroovyLogGetLevel();

void GroovyLog(int nLevel, const char* pszFormat, ...);
void GroovyLogRaw(int nLevel, const char* pszLine);	// for sinks handed an already-formatted line

// Write to config/groovy.log EVEN WHEN the user has file logging switched off, and regardless
// of the log level.
//
// Strictly for bounded, once-per-process lifecycle events - currently the build identity and the
// session-close record. Never call this from anything that can fire per frame; use GroovyLog()
// and respect the user's switch. See the implementation for why the exception exists.
void GroovyLogAlways(const char* pszFormat, ...);

// "groovy module built <date> <time>" - the compile stamp of groovy_log.cpp. Recorded at startup
// and on every close so a log always answers "which binary produced this".
const char* GroovyBuildStamp();

// ---------------------------------------------------------------------------
// Optional file sink - config/groovy.log
// ---------------------------------------------------------------------------
//
// Off by default. Exists because a failed netplay match otherwise leaves no evidence: the ring
// below is in memory, the settings dialog pauses the game so it cannot be read mid-match, and
// Fightcade launches a fresh FBNeo per match, so the ring dies with the process.
//
// Lines are flushed individually. That matters more than it looks - the failures worth
// capturing end in a hang or a kill, and anything still sitting in a stdio buffer would be
// exactly the part that was needed.
//
// Never log per frame. Callers are expected to log state changes; on top of that the sink
// collapses identical consecutive lines (see GroovyLogRaw) so a caller that slips through
// cannot flood the file.
void GroovyLogSetFileOutput(int bEnable);
int  GroovyLogGetFileOutput();

// One-off line recorded at the top of a session, describing the configuration in effect.
void GroovyLogBanner(const char* pszSummary);

// Emit a line only when its content has CHANGED since the last call with the same key.
// For breadcrumbs at points that are hit many times per second - video-init predicates, for
// instance - where only a transition is interesting.
#define GROOVY_LOGKEY_VIDEOINIT 0
#define GROOVY_LOGKEY_VSYNC     1
#define GROOVY_LOGKEY_PACKFAIL  2
#define GROOVY_LOGKEY_AUDIOCLAMP 3
#define GROOVY_LOGKEY_NETSTALL  4	// GGPO input stall starting/stopping
#define GROOVY_LOGKEY_FRAMEPATH 5	// which branch of RunFrame is being taken
#define GROOVY_LOGKEY_KEEPALIVE 6	// idle keepalive started/stopped
#define GROOVY_LOGKEY_SESSDEAD  7	// core stopped acknowledging / returned nonsense
#define GROOVY_LOGKEY_RASTERSPREAD 8	// frameEcho/frame desync that would spin WaitSync
#define GROOVY_LOGKEY_COUNT     9
void GroovyLogOnChange(int nLevel, int nKey, const char* pszFormat, ...);

// ---------------------------------------------------------------------------
// Lifecycle breadcrumbs
// ---------------------------------------------------------------------------
//
// Groovy's own log only starts once GroovyFrameReady() runs, which is far too late to explain
// a launch that never gets that far. These trace the FBNeo/GGPO startup itself so a failed
// netplay match says where it died rather than just going quiet.
//
// All of them are one-shot or on-change - none is per frame.
void GroovyTrace(const char* pszFormat, ...);

// Ring buffer, for the settings dialog. Index 0 is the oldest retained line.
int         GroovyLogLineCount();
const char* GroovyLogLine(int nIndex);
void        GroovyLogClear();

#endif // GROOVY_LOG_H
