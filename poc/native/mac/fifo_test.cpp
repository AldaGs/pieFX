//	pieFX — the plug-in side of the transport, tested without After Effects.
//
//	poc/pipe_test.py drives the OVERLAY: it plays the plug-in's part and proves
//	the UI. This is the mirror, and the half that had no harness on either
//	platform: it runs the REAL plug-in transport against the REAL overlay
//	binary, with no AE anywhere.
//
//	It exists for the four things that can only go wrong here, every one of
//	which either has no Windows equivalent or behaves differently:
//
//	  1. the handshake, over open() semantics rather than ConnectNamedPipe
//	  2. the BOUNDED write — an overlay that stops reading must cost the
//	     deadline, not the session. This is the fault b9a73eb fixed on
//	     Windows, and a FIFO reaches it by a different route.
//	  3. SIGPIPE, which on this platform KILLS the writer by default and on
//	     Windows is merely an error return
//	  4. re-accept, after the overlay goes and comes back
//
//	    ./poc/native/mac/build_fifo_test.sh && $TMPDIR/pieFX_fifo_test
//
#include "pieFX_fifo.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int	S_fails		= 0;
static int	S_lines		= 0;
static char	S_last_line[4096];

static void
Ok(int cond, const char *what, const char *detail)
{
	if (cond) {
		printf("  PASS  %s%s%s\n", what, detail && *detail ? " — " : "", detail ? detail : "");
	} else {
		printf("  FAIL  %s%s%s\n", what, detail && *detail ? " — " : "", detail ? detail : "");
		S_fails++;
	}
}

static void
OnLine(const char *line, void *)
{
	S_lines++;
	snprintf(S_last_line, sizeof(S_last_line), "%s", line);
	printf("    <- %s\n", line);
}

static void
OnLog(const char *msg, void *)
{
	//	The transport's own log, verbatim: when a handshake goes wrong this is
	//	the only account of which half did.
	fputs(msg, stdout);
}

static long
NowMs(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void
SleepMs(long ms)
{
	usleep((useconds_t)ms * 1000);
}

//	When `bare` is set the overlay is started with NO --events/--actions, so it
//	must find the FIFOs through its own built-in defaults. That is the dev flow
//	— an overlay launched by hand knows only those names — and it is the one
//	place the two sides' base names have to agree, so it is worth an assertion
//	rather than a convention.
static int S_launch_bare = 0;

static pid_t
LaunchOverlay(const char *exe, const char *events, const char *actions)
{
	pid_t pid = fork();

	if (pid == 0) {
		if (S_launch_bare) {
			setsid();
			execl(exe, exe, "--settings", "none", (char *)NULL);
			_exit(127);
		}
		//	Its own process group, so the cleanup here can take the WebKit
		//	children with it — the same reason Windows puts the overlay in a
		//	job object.
		setsid();
		execl(exe, exe, "--events", events, "--actions", actions,
			  "--settings", "none", (char *)NULL);
		_exit(127);
	}
	return pid;
}

static void
KillTree(pid_t pid)
{
	if (pid <= 0) {
		return;
	}
	kill(-pid, SIGKILL);
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

//	Wait for the transport to report both channels up.
static int
WaitConnected(int want, long timeout_ms)
{
	long deadline = NowMs() + timeout_ms;

	while (NowMs() < deadline) {
		if (PieFX_PipeConnected() == want) {
			return 1;
		}
		SleepMs(25);
	}
	return 0;
}

static void
Summon(int x, int y)
{
	char buf[256];

	//	POINTS, top-left origin — settled by MAC_PORT.md step 1. The plug-in
	//	will take these straight from NSEvent and convert nothing.
	snprintf(buf, sizeof(buf),
			 "{\"type\":\"summon\",\"x\":%d,\"y\":%d,"
			 "\"hasSelection\":true,\"hasComp\":true,\"layerCount\":1}\n", x, y);
	PieFX_PipeWrite(buf);
}

int
main(int argc, char **argv)
{
	const char *exe = (argc > 1)
		? argv[1]
		: "poc/overlay/src-tauri/target/release/pieFX-overlay";
	char	events[512]		= { 0 };
	char	actions[512]	= { 0 };

	if (access(exe, X_OK) != 0) {
		printf("no overlay binary at %s\n", exe);
		printf("  build it: cd poc/overlay/src-tauri && cargo build --release\n");
		return 1;
	}

	printf("== start ==\n");
	if (!PieFX_StartPipeServer(events, sizeof(events), actions, sizeof(actions),
							   OnLine, NULL, OnLog, NULL)) {
		printf("FAIL: server would not start\n");
		return 1;
	}

	// --- 1. the handshake ---------------------------------------------------
	printf("== handshake ==\n");
	pid_t ov = LaunchOverlay(exe, events, actions);
	printf("  overlay pid %d\n", (int)ov);
	Ok(WaitConnected(1, 15000), "both channels connected", NULL);

	// --- 2. a round trip ----------------------------------------------------
	//	Not a transport property on its own, but if this fails the rest of the
	//	run says nothing: every later assertion is about a channel that works.
	printf("== round trip ==\n");
	S_lines = 0;
	Summon(760, 500);
	SleepMs(250);
	PieFX_PipeWrite("{\"type\":\"cursor\",\"x\":760,\"y\":620}\n");
	SleepMs(250);
	PieFX_PipeWrite("{\"type\":\"release\"}\n");
	SleepMs(800);
	Ok(S_lines > 0, "a release came back on the actions FIFO", S_last_line);

	// --- 3. the bounded write ----------------------------------------------
	//	The one that froze After Effects. SIGSTOP makes the overlay stop
	//	reading while leaving it alive and the FIFO whole — precisely the state
	//	a synchronous write never returns from. The pipe buffer absorbs the
	//	first writes; the call that finds it full must give up on the deadline
	//	rather than park.
	printf("== bounded write (overlay stopped, not killed) ==\n");
	kill(ov, SIGSTOP);
	SleepMs(200);
	{
		long	t0		= NowMs();
		long	elapsed	= 0;
		int		blocked	= 0;

		//	64KB of pipe buffer has to be filled before a write can block at
		//	all, so this pushes until one fails.
		for (int i = 0; i < 4000; i++) {
			if (!PieFX_PipeWrite("{\"type\":\"cursor\",\"x\":700,\"y\":700}\n")) {
				blocked = 1;
				break;
			}
		}
		elapsed = NowMs() - t0;

		char detail[128];
		snprintf(detail, sizeof(detail), "gave up after %ldms", elapsed);
		Ok(blocked, "a stalled overlay makes the write FAIL rather than hang", detail);
		//	The deadline is 1000ms. Anything near it is right; what this is
		//	really excluding is "never returned", which on Windows meant AE was
		//	frozen until the overlay was killed by hand.
		Ok(elapsed < 5000, "and it gave up promptly", detail);
		Ok(!PieFX_PipeConnected(), "the overlay is now treated as gone", NULL);
	}
	kill(ov, SIGCONT);
	SleepMs(200);

	// --- 4. SIGPIPE ---------------------------------------------------------
	//	Kill the reader outright and write into the FIFO. The default action
	//	for SIGPIPE is to terminate the process — which in production is After
	//	Effects — so the assertion is simply that we are still here afterwards.
	printf("== SIGPIPE ==\n");
	KillTree(ov);
	ov = -1;
	SleepMs(300);
	{
		int wrote = PieFX_PipeWrite("{\"type\":\"cursor\",\"x\":1,\"y\":1}\n");

		Ok(!wrote, "a write with no reader fails", NULL);
		Ok(1, "and did NOT kill this process (SIGPIPE ignored)", NULL);
	}
	Ok(WaitConnected(0, 5000), "the server noticed the overlay had gone", NULL);

	// --- 5. re-accept -------------------------------------------------------
	//	The overlay restarted. The server loops and accepts again, which is
	//	what makes arm/disarm/arm survivable.
	printf("== re-accept ==\n");
	ov = LaunchOverlay(exe, events, actions);
	printf("  overlay pid %d\n", (int)ov);
	Ok(WaitConnected(1, 15000), "a fresh overlay reconnected on the same FIFOs", NULL);

	S_lines = 0;
	Summon(760, 500);
	SleepMs(250);
	PieFX_PipeWrite("{\"type\":\"cursor\",\"x\":760,\"y\":620}\n");
	SleepMs(250);
	PieFX_PipeWrite("{\"type\":\"release\"}\n");
	SleepMs(800);
	Ok(S_lines > 0, "and the round trip still works after reconnecting", S_last_line);

	// --- 6. the quit path ---------------------------------------------------
	//	How AE's death hook ends it: the overlay goes on its own, while the
	//	FIFOs are still whole. Tearing the transport down under a live overlay
	//	is what left it un-dead.
	printf("== quit ==\n");
	PieFX_PipeWrite("{\"type\":\"quit\"}\n");
	{
		int		gone	= 0;
		long	deadline = NowMs() + 5000;

		while (NowMs() < deadline) {
			int st;

			if (waitpid(ov, &st, WNOHANG) == ov) {
				gone = 1;
				break;
			}
			SleepMs(50);
		}
		Ok(gone, "the overlay quit on request, with the FIFOs still up", NULL);
		if (!gone) {
			KillTree(ov);
		}
		ov = -1;
	}

	// --- 7. the dev flow: built-in default names ---------------------------
	//	The overlay's macOS defaults must match PIEFX_FIFO_EVENTS /
	//	PIEFX_FIFO_ACTIONS. Nothing else checks that they have not drifted, and
	//	the symptom of drift is an overlay that starts and silently never
	//	connects.
	printf("== default names (dev flow) ==\n");
	S_launch_bare = 1;
	ov = LaunchOverlay(exe, events, actions);
	printf("  overlay pid %d, started with no --events/--actions\n", (int)ov);
	{
		int on_base = (strstr(events, "pieFX.events") != NULL);

		Ok(on_base, "the server holds the BASE names (nothing else is running)", events);
		if (on_base) {
			Ok(WaitConnected(1, 15000),
			   "an overlay started bare found them through its own defaults", NULL);
		}
	}
	PieFX_PipeWrite("{\"type\":\"quit\"}\n");
	SleepMs(1500);
	KillTree(ov);
	ov = -1;

	printf("== stop ==\n");
	PieFX_StopPipeServer();
	Ok(access(events, F_OK) != 0 && access(actions, F_OK) != 0,
	   "the FIFOs were removed (a named pipe gets this for free)", NULL);

	printf("\n%s (%d failure(s))\n", S_fails ? "FAILED" : "done", S_fails);
	return S_fails ? 1 : 0;
}
