//	pieFX — the macOS transport. See pieFX_fifo.h.
//
//	The Windows original is a named-pipe SERVER with two one-way pipes: TX
//	(plug-in -> overlay, events) written from AE's UI thread, RX (overlay ->
//	plug-in, actions) parked on a background thread. Never one duplex pipe —
//	a synchronous duplex handle serialises its I/O, so a thread parked in read
//	blocks a write issued from the UI thread, which froze AE on the first
//	summon.
//
//	FIFOs keep that shape exactly, and the overlay's own end needs no change:
//	it opens both with a plain File::open on a path. What differs is the
//	semantics of open() and the existence of SIGPIPE, and those are where the
//	comments below are spent.

#include "pieFX_fifo.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

//	Mirrors of the pieFX.h constants, kept here so this file builds standalone.
//	They must not drift; the test asserts against the same numbers.
#define PIEFX_PIPE_WRITE_MS	1000
#define PIEFX_LINE_MAX		8192

//	How often the server thread re-examines a connection it is not hearing
//	from. It is a liveness cadence, not a latency one: reads wake on their own
//	when there is something to read. See ServeOne.
#define PIEFX_FIFO_POLL_MS	250

//	The base names, in $TMPDIR. The Windows equivalents are
//	\\.\pipe\pieFX and \\.\pipe\pieFX-cmd; the overlay carries matching
//	defaults so an overlay started by hand — the dev flow — still connects.
#define PIEFX_FIFO_EVENTS	"pieFX.events"
#define PIEFX_FIFO_ACTIONS	"pieFX.actions"
#define PIEFX_FIFO_LOCK		"pieFX.lock"

// ---------------------------------------------------------------------------
//	state
// ---------------------------------------------------------------------------
static pthread_mutex_t	S_mx		= PTHREAD_MUTEX_INITIALIZER;
static pthread_t		S_thread;
static int				S_thread_live	= 0;

static int	S_tx		= -1;		// write end of events, owned by the UI thread
static int	S_rx		= -1;		// read end of actions, owned by the server thread
static int	S_connected	= 0;
static int	S_lock_fd	= -1;

//	The stop signal. Windows has an event object and waits on it alongside the
//	I/O; poll() waits on descriptors, so the equivalent is a self-pipe: writing
//	a byte into it makes every poll() this file performs return at once.
static int	S_stop[2]	= { -1, -1 };

static char	S_events[512];
static char	S_actions[512];

static PieFXLineFn	S_on_line	= NULL;
static void		   *S_line_user	= NULL;
static PieFXLogFn	S_log		= NULL;
static void		   *S_log_user	= NULL;

static void
Log(const char *fmt, ...)
{
	if (!S_log) {
		return;
	}
	char	buf[1024];
	va_list	ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	S_log(buf, S_log_user);
}

static int
StopRequested(void)
{
	struct pollfd p;

	if (S_stop[0] < 0) {
		return 0;
	}
	p.fd		= S_stop[0];
	p.events	= POLLIN;
	p.revents	= 0;
	return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

// ---------------------------------------------------------------------------
//	names
// ---------------------------------------------------------------------------
static const char *
TempDir(void)
{
	//	$TMPDIR is macOS' GetTempPath. It is per-user and already ends in '/',
	//	but nothing guarantees the variable is set at all under a launchd job.
	const char *t = getenv("TMPDIR");

	return (t && *t) ? t : "/tmp/";
}

static void
JoinTemp(char *out, size_t cap, const char *leaf, long pid)
{
	const char *t	= TempDir();
	size_t		n	= strlen(t);
	const char *sep	= (n && t[n - 1] == '/') ? "" : "/";

	if (pid > 0) {
		snprintf(out, cap, "%s%s%s-%ld", t, sep, leaf, pid);
	} else {
		snprintf(out, cap, "%s%s%s", t, sep, leaf);
	}
}

//	Pick names that are actually free, the same intent as ResolvePipeNames on
//	Windows: try the base names first so a hand-started overlay connects, and
//	let a second AE instance fall back to a pid-suffixed pair rather than
//	silently running with no overlay.
//
//	The test for "taken" cannot be the one Windows uses. A named pipe exists
//	only while its server holds it, so CreateNamedPipe simply fails when
//	another AE has it. A FIFO is a FILE: it outlives the process that made it,
//	so its mere existence proves nothing — a crashed AE leaves one behind, and
//	treating that as "taken" would push every later session onto pid names
//	forever.
//
//	So the liveness question is asked of a LOCK, not of the FIFOs. flock is
//	released by the kernel when the holder dies, crash included, which is
//	exactly the property the FIFO lacks.
static void
ResolveNames(void)
{
	char lock_path[512];

	JoinTemp(lock_path, sizeof(lock_path), PIEFX_FIFO_LOCK, 0);
	S_lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
	if (S_lock_fd >= 0 && flock(S_lock_fd, LOCK_EX | LOCK_NB) == 0) {
		JoinTemp(S_events,  sizeof(S_events),  PIEFX_FIFO_EVENTS,  0);
		JoinTemp(S_actions, sizeof(S_actions), PIEFX_FIFO_ACTIONS, 0);
		return;
	}
	if (S_lock_fd >= 0) {
		close(S_lock_fd);
		S_lock_fd = -1;
	}

	long pid = (long)getpid();

	JoinTemp(S_events,  sizeof(S_events),  PIEFX_FIFO_EVENTS,  pid);
	JoinTemp(S_actions, sizeof(S_actions), PIEFX_FIFO_ACTIONS, pid);
	Log("  pipe: base names held (another AE?), using %s / %s\n", S_events, S_actions);
}

static int
MakeFifo(const char *path)
{
	//	A leftover from a crashed session is not an error to report — it is the
	//	normal state after a hard exit, and mkfifo would fail with EEXIST on a
	//	path that is ours to own.
	unlink(path);
	if (mkfifo(path, 0600) != 0) {
		Log("  pipe: mkfifo %s failed, errno=%d\n", path, errno);
		return 0;
	}
	return 1;
}

// ---------------------------------------------------------------------------
//	the write side (UI thread)
// ---------------------------------------------------------------------------
int
PieFX_PipeWrite(const char *jsonZ)
{
	int ok = 0;

	pthread_mutex_lock(&S_mx);
	if (S_connected && S_tx >= 0) {
		//	Bounded, because this runs on AE's UI thread. The Windows version
		//	reaches that with OVERLAPPED plus a deadline; here the fd is
		//	already O_NONBLOCK, so a full pipe returns EAGAIN instead of
		//	parking, and poll() supplies the deadline. Same guarantee, less
		//	machinery: an overlay that has stopped reading costs
		//	PIEFX_PIPE_WRITE_MS and is then declared gone, rather than freezing
		//	AE until someone kills it by hand.
		size_t		len		= strlen(jsonZ);
		size_t		sent	= 0;
		int			timed_out = 0;

		//	One deadline for the whole line, not per write() call: a slow
		//	reader draining a byte at a time would otherwise renew it forever.
		struct timespec t0;
		clock_gettime(CLOCK_MONOTONIC, &t0);

		while (sent < len && !timed_out) {
			ssize_t n = write(S_tx, jsonZ + sent, len - sent);

			if (n > 0) {
				sent += (size_t)n;
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				struct timespec now;
				long			elapsed_ms;
				struct pollfd	p;

				clock_gettime(CLOCK_MONOTONIC, &now);
				elapsed_ms = (long)((now.tv_sec - t0.tv_sec) * 1000 +
									(now.tv_nsec - t0.tv_nsec) / 1000000);
				if (elapsed_ms >= PIEFX_PIPE_WRITE_MS) {
					timed_out = 1;
					break;
				}
				p.fd		= S_tx;
				p.events	= POLLOUT;
				p.revents	= 0;
				if (poll(&p, 1, (int)(PIEFX_PIPE_WRITE_MS - elapsed_ms)) <= 0) {
					timed_out = 1;
					break;
				}
				continue;
			}
			//	EPIPE: the overlay closed its read end. This is the condition
			//	that would have KILLED the process by default — see the
			//	SIGPIPE note in PieFX_StartPipeServer. Here it is just an
			//	error return, which is what it already was on Windows.
			break;
		}

		ok = (sent == len);
		if (timed_out) {
			Log("  pipe: write timed out after %dms, treating the overlay as gone\n",
				PIEFX_PIPE_WRITE_MS);
		}
		if (!ok) {
			//	Client went away. The server thread recycles the instance.
			S_connected = 0;
		}
	}
	pthread_mutex_unlock(&S_mx);
	return ok;
}

int
PieFX_PipeConnected(void)
{
	int c;

	pthread_mutex_lock(&S_mx);
	c = S_connected;
	pthread_mutex_unlock(&S_mx);
	return c;
}

// ---------------------------------------------------------------------------
//	the server thread: accept + read + re-accept
// ---------------------------------------------------------------------------
//	Wait for the overlay to open the events FIFO for reading.
//
//	This is the accept, and it is where FIFO semantics differ most from a named
//	pipe. O_WRONLY fails with ENXIO until a READER exists, so a non-blocking
//	write-open that SUCCEEDS is a genuine connection event — the direct
//	equivalent of ConnectNamedPipe. The blocking form would be simpler and is
//	not usable: it parks in open() with nothing to poll, so a disarm could not
//	get this thread moving. Polling with a stop check keeps that possible.
static int
AcceptTx(void)
{
	for (;;) {
		int fd;

		if (StopRequested()) {
			return -1;
		}
		fd = open(S_events, O_WRONLY | O_NONBLOCK);
		if (fd >= 0) {
			return fd;
		}
		if (errno != ENXIO) {
			Log("  pipe: open(events) failed, errno=%d\n", errno);
			return -1;
		}
		usleep(20 * 1000);
	}
}

//	And the read end of actions. O_RDONLY on a FIFO succeeds immediately
//	whether or not a writer exists, so unlike the TX side the open proves
//	nothing; what proves the overlay is there is the first byte. O_NONBLOCK so
//	the poll below owns the waiting.
static int
OpenRx(void)
{
	int fd = open(S_actions, O_RDONLY | O_NONBLOCK);

	if (fd < 0) {
		Log("  pipe: open(actions) failed, errno=%d\n", errno);
	}
	return fd;
}

//	Is the overlay still holding the events FIFO open for reading?
//
//	This exists because the disconnect signal Windows relies on does not arrive
//	here. On a named pipe the client going away makes the parked ReadFile
//	return 0. Measured on macOS, a poll() parked on the read end of a FIFO
//	whose last writer has closed simply NEVER RETURNS — the server thread sat
//	in it through a killed overlay, a relaunched one, and every assertion in
//	between, and only came out when the stop byte arrived.
//
//	So liveness is asked directly instead. O_WRONLY on a FIFO fails with ENXIO
//	when there is no reader, which is exactly the question: no reader on the
//	events FIFO means no overlay. It is the same primitive the accept uses,
//	asked in the other direction.
static int
OverlayHasReader(void)
{
	int fd = open(S_events, O_WRONLY | O_NONBLOCK);

	if (fd >= 0) {
		close(fd);
		return 1;
	}
	return (errno != ENXIO);	// a different error is not proof of absence
}

static void
ServeOne(void)
{
	char	acc[PIEFX_LINE_MAX];
	size_t	acc_len = 0;

	for (;;) {
		struct pollfd	p[2];
		char			buf[1024];
		ssize_t			got;

		p[0].fd = S_rx;	 p[0].events = POLLIN; p[0].revents = 0;
		p[1].fd = S_stop[0]; p[1].events = POLLIN; p[1].revents = 0;

		if (poll(p, 2, PIEFX_FIFO_POLL_MS) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (p[1].revents & POLLIN) {
			break;						// disarm
		}
		if (!(p[0].revents & (POLLIN | POLLHUP))) {
			//	Nothing to read. The poll timed out rather than woke, so this
			//	is where a departed overlay is noticed — it will not announce
			//	itself.
			if (!OverlayHasReader()) {
				break;
			}
			continue;
		}

		got = read(S_rx, buf, sizeof(buf));
		if (got == 0) {
			//	No writer on the actions FIFO. That is ambiguous, and the two
			//	cases are opposites: the overlay has GONE, or it has opened
			//	events and not yet opened actions — the gap between its two
			//	opens, which happens on every single connect. The events
			//	reader tells them apart.
			if (!OverlayHasReader()) {
				break;
			}
			usleep(20 * 1000);	// its write end is coming; do not spin on the HUP
			continue;
		}
		if (got < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			}
			break;
		}
		for (ssize_t i = 0; i < got; i++) {
			char ch = buf[i];

			if (ch == '\n') {
				acc[acc_len] = 0;
				if (acc_len && S_on_line) {
					S_on_line(acc, S_line_user);
				}
				acc_len = 0;
			} else if (ch != '\r' && acc_len + 1 < sizeof(acc)) {
				acc[acc_len++] = ch;
			}
		}
	}
}

static void *
ServerThread(void *)
{
	for (;;) {
		int tx, rx;

		if (StopRequested()) {
			break;
		}

		//	TX first, and RX only after it: the overlay opens events then
		//	actions, in that order, and the harness that exercises this
		//	depends on the same sequence.
		tx = AcceptTx();
		if (tx < 0) {
			break;
		}
		//	The poke in StopPipeServer opens the events FIFO for reading, which
		//	is indistinguishable from an overlay arriving — that is the point of
		//	it, and it is how a parked accept gets moving. Re-check here so the
		//	poke does not get reported as a connection that never existed.
		if (StopRequested()) {
			close(tx);
			break;
		}
		rx = OpenRx();
		if (rx < 0) {
			close(tx);
			break;
		}

		pthread_mutex_lock(&S_mx);
		S_tx		= tx;
		S_rx		= rx;
		S_connected	= 1;
		pthread_mutex_unlock(&S_mx);
		Log("  pipe: overlay connected (tx+rx)\n");

		ServeOne();

		pthread_mutex_lock(&S_mx);
		S_connected	= 0;
		S_tx		= -1;
		S_rx		= -1;
		pthread_mutex_unlock(&S_mx);
		close(tx);
		close(rx);
		Log("  pipe: overlay disconnected\n");

		if (StopRequested()) {
			break;
		}
		//	else: loop and re-accept (overlay restarted)
	}
	return NULL;
}

// ---------------------------------------------------------------------------
//	lifetime
// ---------------------------------------------------------------------------
int
PieFX_StartPipeServer(char *events_out, size_t events_cap,
                      char *actions_out, size_t actions_cap,
                      PieFXLineFn on_line, void *line_user,
                      PieFXLogFn log, void *log_user)
{
	S_on_line	= on_line;
	S_line_user	= line_user;
	S_log		= log;
	S_log_user	= log_user;

	//	SIGPIPE, and it is not optional.
	//
	//	Writing to a pipe with no reader raises SIGPIPE, whose default action
	//	is to TERMINATE the process — and the process here is After Effects.
	//	The identical condition on Windows is an error return from WriteFile,
	//	so nothing in the original anticipates it.
	//
	//	Ignoring it process-wide is a decision taken inside somebody else's
	//	application, which deserves saying out loud. It is the conventional
	//	one: writes return EPIPE instead, which is what the Windows path
	//	already handles, and a host that WANTED to die on a broken pipe is not
	//	a thing that exists. The per-thread alternative (block it, then drain
	//	it with sigtimedwait) would have to run on AE's UI thread, since that
	//	is where PipeWrite is called from, and leaving a signal pending on AE's
	//	UI thread is the more invasive of the two.
	//
	//	MSG_NOSIGNAL, which would avoid the question entirely, is a socket
	//	facility and does not apply to FIFOs. It is the argument for the Unix
	//	domain socket fallback MAC_PORT.md keeps in reserve.
	signal(SIGPIPE, SIG_IGN);

	if (pipe(S_stop) != 0) {
		Log("  pipe: self-pipe failed, errno=%d\n", errno);
		return 0;
	}
	fcntl(S_stop[0], F_SETFL, O_NONBLOCK);
	fcntl(S_stop[1], F_SETFL, O_NONBLOCK);

	ResolveNames();
	if (!MakeFifo(S_events) || !MakeFifo(S_actions)) {
		return 0;
	}
	if (events_out)	 { snprintf(events_out,  events_cap,  "%s", S_events);  }
	if (actions_out) { snprintf(actions_out, actions_cap, "%s", S_actions); }

	if (pthread_create(&S_thread, NULL, ServerThread, NULL) != 0) {
		Log("  pipe: pthread_create failed, errno=%d\n", errno);
		return 0;
	}
	S_thread_live = 1;
	Log("  pipe: server up, events=%s actions=%s\n", S_events, S_actions);
	return 1;
}

void
PieFX_StopPipeServer(void)
{
	//	The overlay must ALREADY have been told to quit. That order is not a
	//	preference: closing the FIFOs while it is still alive against them
	//	leaves it un-dead, holding handles on paths that are about to vanish,
	//	which is the state AE's death hook was written to avoid and the fault
	//	b9a73eb fixed on the Windows side.
	if (S_stop[1] >= 0) {
		ssize_t ignored = write(S_stop[1], "x", 1);

		(void)ignored;
	}

	//	Unblock an AcceptTx parked on a FIFO with no reader. It polls, so the
	//	stop byte is enough — but a poke costs nothing and covers the moment
	//	between its stop check and its open().
	{
		int poke = open(S_events, O_RDONLY | O_NONBLOCK);

		if (poke >= 0) {
			close(poke);
		}
	}

	if (S_thread_live) {
		pthread_join(S_thread, NULL);
		S_thread_live = 0;
	}

	pthread_mutex_lock(&S_mx);
	if (S_tx >= 0) { close(S_tx); S_tx = -1; }
	if (S_rx >= 0) { close(S_rx); S_rx = -1; }
	S_connected = 0;
	pthread_mutex_unlock(&S_mx);

	//	The FIFOs are files, so leaving them behind leaves litter that the next
	//	run would have to reason about. Windows gets this for free: a named
	//	pipe disappears with its server.
	unlink(S_events);
	unlink(S_actions);

	if (S_stop[0] >= 0) { close(S_stop[0]); S_stop[0] = -1; }
	if (S_stop[1] >= 0) { close(S_stop[1]); S_stop[1] = -1; }
	if (S_lock_fd >= 0) { flock(S_lock_fd, LOCK_UN); close(S_lock_fd); S_lock_fd = -1; }
	Log("  pipe: server down\n");
}
