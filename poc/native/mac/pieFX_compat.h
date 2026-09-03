//	pieFX — the small Win32 vocabulary, on macOS.
//
//	pieFX.cpp is ~2,950 lines and only five REGIONS of it are genuinely
//	platform-specific: the pipe server, the overlay launch, the gesture, the
//	clipboard, and the paths. Everything else that fails to compile here fails
//	over vocabulary rather than over substance — `BOOL`, `MAX_PATH`,
//	`sprintf_s`, `ZeroMemory` — spread thinly across code that is otherwise
//	pure AEGP and perfectly portable.
//
//	Translating that vocabulary is much less invasive than rewriting the 300-odd
//	lines it appears on, and it leaves every one of those lines BYTE-IDENTICAL
//	between the two platforms, which is the property that matters: the Windows
//	product is shipping, and a port is not a licence to churn it.
//
//	This header deliberately does NOT try to emulate Windows. It covers exactly
//	what pieFX.cpp uses and nothing more; anything genuinely platform-specific
//	belongs in a module with an argued implementation, not in a shim.
#ifndef PIEFX_COMPAT_H
#define PIEFX_COMPAT_H

#ifndef AE_OS_WIN

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdint.h>

// --- types ------------------------------------------------------------------
typedef int				BOOL;
typedef unsigned char	BYTE;
typedef unsigned int	UINT;
typedef unsigned long	DWORD;
typedef long			LONG;
typedef unsigned long	UINT_PTR;
typedef const char	   *LPCSTR;
typedef void			VOID;

#ifndef TRUE
	#define TRUE	1
#endif
#ifndef FALSE
	#define FALSE	0
#endif

typedef struct { LONG x, y; } POINT;

//	PATH_MAX is 1024 here against Windows' 260. Using the platform's own value
//	rather than redefining it to 260 means a long macOS path is not silently
//	truncated into a file that does not exist.
#ifndef MAX_PATH
	#define MAX_PATH	PATH_MAX
#endif

// --- the string functions ---------------------------------------------------
//	The _s forms take the destination SIZE, which is the whole reason they
//	exist, so these map onto the bounded POSIX equivalents rather than onto the
//	unbounded ones. Return values follow the Windows convention: 0 for success,
//	non-zero for failure, because pieFX.cpp tests them that way.
#define _TRUNCATE	((size_t)-1)

static inline int
pieFX_strcpy_s(char *dst, size_t cap, const char *src)
{
	if (!dst || !cap) {
		return 1;
	}
	if (!src) {
		dst[0] = 0;
		return 1;
	}
	if (strlen(src) >= cap) {
		snprintf(dst, cap, "%s", src);
		return 1;			// truncated, and the caller is told
	}
	snprintf(dst, cap, "%s", src);
	return 0;
}

static inline int
pieFX_strcat_s(char *dst, size_t cap, const char *src)
{
	size_t used;

	if (!dst || !cap || !src) {
		return 1;
	}
	used = strnlen(dst, cap);
	if (used >= cap) {
		return 1;
	}
	if (strlen(src) >= cap - used) {
		snprintf(dst + used, cap - used, "%s", src);
		return 1;
	}
	snprintf(dst + used, cap - used, "%s", src);
	return 0;
}

#define strcpy_s(d, c, s)	pieFX_strcpy_s((d), (c), (s))
#define strcat_s(d, c, s)	pieFX_strcat_s((d), (c), (s))
//	sprintf_s takes (buf, size, fmt, ...) — snprintf's signature exactly.
#define sprintf_s			snprintf
//	_snprintf_s(buf, size, _TRUNCATE, fmt, ...) — the count argument is dropped,
//	because snprintf always truncates rather than overrunning.
#define _snprintf_s(b, c, t, ...)	snprintf((b), (c), __VA_ARGS__)

static inline int
pieFX_strncat_s(char *dst, size_t cap, const char *src, size_t count)
{
	//	pieFX only ever calls this with _TRUNCATE, which means "append what
	//	fits". That is exactly strncat_s's bounded behaviour minus the error.
	(void)count;
	return pieFX_strcat_s(dst, cap, src);
}

#define strncat_s(d, c, s, n)	pieFX_strncat_s((d), (c), (s), (n))

static inline int
pieFX_strncpy_s(char *dst, size_t cap, const char *src, size_t count)
{
	(void)count;			// always _TRUNCATE here
	return pieFX_strcpy_s(dst, cap, src);
}

#define strncpy_s(d, c, s, n)	pieFX_strncpy_s((d), (c), (s), (n))

//	Case-insensitive compare. Only ever used to ask "is this the same path?",
//	and note that macOS filesystems are usually case-INSENSITIVE too, so the
//	comparison means the same thing on both.
#define _stricmp	strcasecmp

//	fopen_s returns an error code and takes the FILE** first.
static inline int
pieFX_fopen_s(FILE **f, const char *path, const char *mode)
{
	if (!f) {
		return 1;
	}
	*f = fopen(path, mode);
	return *f ? 0 : 1;
}

#define fopen_s	pieFX_fopen_s

// --- the two path calls that are pure vocabulary ----------------------------
//	These are NOT the "paths" job in MAC_PORT.md step 5 — that one is about
//	%APPDATA% and Documents, which need real decisions about where a macOS
//	pieFX keeps its settings. These two have exact equivalents and no decision
//	in them.

//	GetTempPathA writes a directory ending in a separator and returns its
//	length, or 0 on failure.
static inline DWORD
GetTempPathA(DWORD cap, char *out)
{
	const char *t = getenv("TMPDIR");
	size_t		n;

	if (!t || !*t) {
		t = "/tmp/";
	}
	n = strlen(t);
	if (!out || n + 2 > (size_t)cap) {
		return 0;
	}
	snprintf(out, cap, "%s", t);
	if (out[n - 1] != '/') {		// $TMPDIR usually ends in '/', but not always
		out[n]		= '/';
		out[n + 1]	= 0;
		n++;
	}
	return (DWORD)n;
}

//	DeleteFileA: "make sure this is not there". Windows returns FALSE when the
//	file was already absent and pieFX does not look, because absent is the
//	outcome it wanted either way — unlink has exactly that shape.
static inline BOOL
DeleteFileA(const char *path)
{
	return path && unlink(path) == 0;
}

#define SW_SHOWNORMAL	1

//	"Open this in whatever handles it" — /usr/bin/open, which is what the Finder
//	would do. Spawned rather than run through system(), so a path containing a
//	space or a quote is an argument and never shell syntax.
static inline void
ShellExecuteA(void *hwnd, const char *verb, const char *path,
              const char *params, const char *dir, int show)
{
	(void)hwnd; (void)verb; (void)params; (void)dir; (void)show;
	if (!path) {
		return;
	}
	pid_t pid = fork();

	if (pid == 0) {
		execl("/usr/bin/open", "open", path, (char *)NULL);
		_exit(127);
	}
	//	Not waited for: it is a viewer, and its lifetime is not ours. Reaped
	//	by the double-fork-free route of ignoring SIGCHLD would be neater, but
	//	pieFX opens a report at most once per run.
}

// --- the critical section ----------------------------------------------------
//	A direct equivalent, so it belongs in the vocabulary rather than in a
//	platform branch: the action queue is written on the transport thread and
//	drained on AE's UI thread on BOTH platforms, and that code should stay
//	identical. A Windows CRITICAL_SECTION is recursive, so the mutex is made
//	recursive too rather than quietly changing the locking semantics.
#include <pthread.h>

typedef pthread_mutex_t	CRITICAL_SECTION;

static inline void
InitializeCriticalSection(CRITICAL_SECTION *cs)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(cs, &attr);
	pthread_mutexattr_destroy(&attr);
}

static inline void DeleteCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_destroy(cs); }
static inline void EnterCriticalSection(CRITICAL_SECTION *cs)  { pthread_mutex_lock(cs); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *cs)  { pthread_mutex_unlock(cs); }

// --- odds and ends ----------------------------------------------------------
#define ZeroMemory(p, n)	memset((p), 0, (n))

//	Only ever logged, to tell "which thread was this?" apart in a log file.
//	pthread_self is an opaque pointer, so it is narrowed deliberately rather
//	than pretending to be a Windows thread id.
static inline DWORD
GetCurrentThreadId(void)
{
	return (DWORD)(unsigned long)(uintptr_t)pthread_self();
}

static inline DWORD
GetCurrentProcessId(void)
{
	return (DWORD)getpid();
}

//	Milliseconds since boot, monotonic. The Windows original wraps at 49 days
//	and pieFX only ever subtracts two readings, so the wrap never mattered;
//	CLOCK_MONOTONIC has no wrap to reason about at all.
static inline DWORD
GetTickCount(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (DWORD)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

//	Windows' Sleep takes milliseconds; usleep takes microseconds, and the
//	silent factor of 1000 between them is a classic way to hang for an hour.
static inline void
Sleep(DWORD ms)
{
	usleep((useconds_t)ms * 1000);
}

#endif	// !AE_OS_WIN
#endif	// PIEFX_COMPAT_H
