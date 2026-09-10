// Groovy MiSTer - logging. See groovy_log.h.

#ifdef _WIN32
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#endif

#include "groovy_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static int  nLogLevel = GROOVY_LOG_ERROR;

static char szLogRing[GROOVY_LOG_LINES][GROOVY_LOG_LINE_LEN];
static int  nLogHead  = 0;	// next slot to write
static int  nLogCount = 0;	// lines retained, <= GROOVY_LOG_LINES

// File sink
#define GROOVY_LOG_PATH     "config/groovy.log"
#define GROOVY_LOG_MAX_SIZE (1024 * 1024)	// truncate on open past this, so it cannot creep

static int   bLogToFile   = 0;
static FILE* hLogFile     = NULL;
static int   bLogFileOpenFailed = 0;

// Duplicate suppression. The safety net for the "never log per frame" rule: if a caller does
// slip through, the file records one line plus a count rather than 60 lines a second.
static char szLastLine[GROOVY_LOG_LINE_LEN] = "";
static int  nRepeatCount = 0;

// GroovyLogOnChange state
static char szLastByKey[GROOVY_LOGKEY_COUNT][GROOVY_LOG_LINE_LEN];

// Build identity, recorded unconditionally at startup and on every session close.
//
// Two hardware sessions could not be interpreted because the MiSTer-side logs proved no CMD_CLOSE
// had arrived, but nothing on the PC side said WHICH binary produced them - so "the fix does not
// work" and "the fix is not in this build" were indistinguishable.
//
// Defined here rather than in groovy_output.cpp on purpose: every groovy module already links
// this one, whereas making groovy_config depend on groovy_output would invert the layering.
//
// __DATE__/__TIME__ stamp THIS translation unit, so on an incremental build that recompiled some
// other file they would read stale - which already happened once. GROOVY_MODULE_REV is the fix:
// it must be bumped by hand for any change to the module, and bumping it edits this file, which
// forces the timestamp to move with it. If a log shows an unexpected rev, the binary is old.
#define GROOVY_MODULE_REV "r14"		// r14: reconnect frame-state reset, bounded resync, WaitSync spread guard

const char* GroovyBuildStamp()
{
	return "groovy " GROOVY_MODULE_REV " built " __DATE__ " " __TIME__;
}

void GroovyLogSetLevel(int nLevel)
{
	if (nLevel < GROOVY_LOG_ERROR) nLevel = GROOVY_LOG_ERROR;
	if (nLevel > GROOVY_LOG_TRACE) nLevel = GROOVY_LOG_TRACE;
	nLogLevel = nLevel;
}

int GroovyLogGetLevel()
{
	return nLogLevel;
}

static void LogTimestamp(char* szOut, int nOutLen)
{
#ifdef _WIN32
	SYSTEMTIME st;
	GetLocalTime(&st);
	_snprintf(szOut, nOutLen - 1, "%02d:%02d:%02d.%03d",
	          (int)st.wHour, (int)st.wMinute, (int)st.wSecond, (int)st.wMilliseconds);
#else
	strncpy(szOut, "--:--:--.---", nOutLen - 1);
#endif
	szOut[nOutLen - 1] = '\0';
}

// bForce opens (and writes) even when the user has file logging switched off. Reserved for
// GroovyLogAlways() - see the comment there for why that exception exists.
static FILE* LogFile(int bForce)
{
	if ((!bLogToFile && !bForce) || bLogFileOpenFailed) return NULL;
	if (hLogFile) return hLogFile;

	// Start fresh once the file gets large, rather than appending forever across sessions.
	const char* pszMode = "at";
	FILE* hSize = fopen(GROOVY_LOG_PATH, "rb");
	if (hSize) {
		fseek(hSize, 0, SEEK_END);
		if (ftell(hSize) > GROOVY_LOG_MAX_SIZE) pszMode = "wt";
		fclose(hSize);
	}

	hLogFile = fopen(GROOVY_LOG_PATH, pszMode);
	if (!hLogFile) {
		bLogFileOpenFailed = 1;			// do not retry the open on every line
		return NULL;
	}

#ifdef _WIN32
	SYSTEMTIME st;
	GetLocalTime(&st);
	fprintf(hLogFile, "\n=== Groovy MiSTer log - %04d-%02d-%02d %02d:%02d:%02d ===\n",
	        (int)st.wYear, (int)st.wMonth, (int)st.wDay,
	        (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
#else
	fprintf(hLogFile, "\n=== Groovy MiSTer log ===\n");
#endif
	fflush(hLogFile);
	return hLogFile;
}

void GroovyLogSetFileOutput(int bEnable)
{
	if ((bEnable ? 1 : 0) == bLogToFile) return;

	if (!bEnable && hLogFile) {
		fprintf(hLogFile, "--- file logging disabled ---\n");
		fclose(hLogFile);
		hLogFile = NULL;
	}
	bLogToFile = bEnable ? 1 : 0;
	bLogFileOpenFailed = 0;				// a fresh enable gets a fresh attempt
}

int GroovyLogGetFileOutput()
{
	return bLogToFile;
}

// Write one already-trimmed line to the file, honouring duplicate suppression.
static void LogFileWrite(const char* pszLine, int bForce)
{
	FILE* h = LogFile(bForce);
	if (!h) return;

	if (!strcmp(pszLine, szLastLine)) {
		nRepeatCount++;					// identical to the previous line: hold it back
		return;
	}

	if (nRepeatCount > 0) {
		fprintf(h, "                 (previous line repeated %d times)\n", nRepeatCount);
		nRepeatCount = 0;
	}

	char szTime[24];
	LogTimestamp(szTime, sizeof(szTime));
	fprintf(h, "%s  %s\n", szTime, pszLine);

	// Flush every line: the failures worth capturing end in a hang or a kill, and anything
	// left in a stdio buffer would be exactly the part that was needed.
	fflush(h);

	strncpy(szLastLine, pszLine, GROOVY_LOG_LINE_LEN - 1);
	szLastLine[GROOVY_LOG_LINE_LEN - 1] = '\0';
}

void GroovyLogBanner(const char* pszSummary)
{
	if (!pszSummary) return;
	GroovyLogRaw(GROOVY_LOG_ERROR, pszSummary);
}

void GroovyTrace(const char* pszFormat, ...)
{
	// Deliberately not gated on nLogLevel. These are the breadcrumbs that explain a launch
	// which never reaches Groovy at all, and they only fire at lifecycle transitions, so
	// there is nothing to save by suppressing them. They still go through GroovyLogRaw, so
	// the ring, the file and the debugger all see them.
	char szLine[GROOVY_LOG_LINE_LEN];
	va_list args;
	va_start(args, pszFormat);
	vsnprintf(szLine, sizeof(szLine) - 1, pszFormat, args);
	va_end(args);
	szLine[sizeof(szLine) - 1] = '\0';

	GroovyLogRaw(GROOVY_LOG_ERROR, szLine);
}

// The shared body of GroovyLogRaw() and GroovyLogAlways(). No level gate: callers apply it.
static void LogEmit(const char* pszLine, int bForceFile)
{
	if (!pszLine) return;

	// Strip a trailing newline: switchres and the Groovy client both append one, and the
	// ring buffer is line-oriented already.
	char szTrimmed[GROOVY_LOG_LINE_LEN];
	strncpy(szTrimmed, pszLine, GROOVY_LOG_LINE_LEN - 1);
	szTrimmed[GROOVY_LOG_LINE_LEN - 1] = '\0';
	for (int i = (int)strlen(szTrimmed) - 1; i >= 0 && (szTrimmed[i] == '\n' || szTrimmed[i] == '\r'); i--) {
		szTrimmed[i] = '\0';
	}
	if (!szTrimmed[0]) return;

	strcpy(szLogRing[nLogHead], szTrimmed);
	nLogHead = (nLogHead + 1) % GROOVY_LOG_LINES;
	if (nLogCount < GROOVY_LOG_LINES) nLogCount++;

	LogFileWrite(szTrimmed, bForceFile);

#ifdef _WIN32
	// Also to the debugger, so a developer attached to a Release build sees it live.
	OutputDebugStringA("[groovy] ");
	OutputDebugStringA(szTrimmed);
	OutputDebugStringA("\n");
#else
	fprintf(stderr, "[groovy] %s\n", szTrimmed);	// standalone test builds
#endif
}

void GroovyLogRaw(int nLevel, const char* pszLine)
{
	if (nLevel > nLogLevel) return;
	LogEmit(pszLine, 0);
}

// The one exception to file logging being opt-in.
//
// A fault that only shows up on hardware is undiagnosable if the evidence lives in a file the user
// never switched on, and the setting is fragile in its own right: ConfigAppSave() rewrites the
// whole ini from memory on every normal exit, so a second FBNeo instance can revert a hand-edit of
// bGroovyLogToFile between runs.
//
// A strictly bounded set of lines is therefore written unconditionally: the build identity and the
// session open and close records. That is a handful per process run, at lifecycle transitions only.
// This is not a general-purpose logger - anything that can fire per frame must use GroovyLog() and
// stay behind the user's switch.
void GroovyLogAlways(const char* pszFormat, ...)
{
	char szLine[GROOVY_LOG_LINE_LEN];
	va_list args;
	va_start(args, pszFormat);
	vsnprintf(szLine, sizeof(szLine) - 1, pszFormat, args);
	va_end(args);
	szLine[sizeof(szLine) - 1] = '\0';

	LogEmit(szLine, 1);
}

void GroovyLogOnChange(int nLevel, int nKey, const char* pszFormat, ...)
{
	if (nLevel > nLogLevel) return;
	if (nKey < 0 || nKey >= GROOVY_LOGKEY_COUNT) return;

	char szLine[GROOVY_LOG_LINE_LEN];
	va_list args;
	va_start(args, pszFormat);
	vsnprintf(szLine, sizeof(szLine) - 1, pszFormat, args);
	va_end(args);
	szLine[sizeof(szLine) - 1] = '\0';

	// Only a transition is interesting. This is what lets breadcrumbs sit at points that are
	// hit many times per second - the video-init predicates, for instance - without the
	// caller having to track state itself.
	if (!strcmp(szLine, szLastByKey[nKey])) return;

	strncpy(szLastByKey[nKey], szLine, GROOVY_LOG_LINE_LEN - 1);
	szLastByKey[nKey][GROOVY_LOG_LINE_LEN - 1] = '\0';

	GroovyLogRaw(nLevel, szLine);
}

void GroovyLog(int nLevel, const char* pszFormat, ...)
{
	if (nLevel > nLogLevel) return;		// cheap out before formatting

	char szLine[GROOVY_LOG_LINE_LEN];
	va_list args;
	va_start(args, pszFormat);
	vsnprintf(szLine, sizeof(szLine) - 1, pszFormat, args);
	va_end(args);
	szLine[sizeof(szLine) - 1] = '\0';

	GroovyLogRaw(nLevel, szLine);
}

int GroovyLogLineCount()
{
	return nLogCount;
}

const char* GroovyLogLine(int nIndex)
{
	if (nIndex < 0 || nIndex >= nLogCount) return "";

	// Oldest retained line first.
	const int nOldest = (nLogCount < GROOVY_LOG_LINES) ? 0 : nLogHead;
	return szLogRing[(nOldest + nIndex) % GROOVY_LOG_LINES];
}

void GroovyLogClear()
{
	nLogHead = 0;
	nLogCount = 0;
}
