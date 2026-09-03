//	pieFX — launching the overlay on macOS. See pieFX_launch.h.

#include "pieFX_launch.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

//	No ".exe". Matches [[bin]] name in the overlay's Cargo.toml, which is
//	pinned to pieFX-overlay precisely so this lookup can be a fixed name.
#define PIEFX_OVERLAY_EXE	"pieFX-overlay"

static pid_t		S_overlay	= -1;
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

//	The directory this code is loaded from — the macOS answer to
//	GetModuleHandleEx + GetModuleFileName. dladdr resolves an address inside
//	this image back to the file it came from, which for the plug-in is
//	somewhere inside the .plugin bundle and for the test is the test binary.
static int
SelfDir(char *out, size_t cap)
{
	Dl_info	info;

	if (!dladdr((const void *)&SelfDir, &info) || !info.dli_fname) {
		return 0;
	}
	snprintf(out, cap, "%s", info.dli_fname);

	char *slash = strrchr(out, '/');

	if (!slash) {
		return 0;
	}
	slash[1] = 0;
	return 1;
}

int
PieFX_OverlayAlive(void)
{
	int st;

	if (S_overlay <= 0) {
		return 0;
	}
	//	Reap on the way past. A child that has exited but not been waited for
	//	is still a pid to kill(0), so without this "is it up?" answers yes
	//	forever and arm/disarm/arm never launches again.
	if (waitpid(S_overlay, &st, WNOHANG) == S_overlay) {
		S_overlay = -1;
		return 0;
	}
	return 1;
}

long
PieFX_OverlayPid(void)
{
	return (long)S_overlay;
}

int
PieFX_LaunchOverlay(const char *events, const char *actions,
                    long owner_pid, PieFXLogFn log, void *log_user)
{
	char	dir[1024];
	char	exe[1200];
	char	owner[32];

	S_log		= log;
	S_log_user	= log_user;

	//	Arm -> disarm -> arm used to start a second overlay every time on
	//	Windows, leaving the earlier ones alive and spinning on a pipe they
	//	will never be given. Same rule here.
	if (PieFX_OverlayAlive()) {
		Log("  overlay: already running, not launching another\n");
		return 1;
	}

	if (!SelfDir(dir, sizeof(dir))) {
		Log("  overlay: cannot locate own directory\n");
		return 0;
	}
	snprintf(exe, sizeof(exe), "%s%s", dir, PIEFX_OVERLAY_EXE);
	if (access(exe, X_OK) != 0) {
		Log("  overlay: %s not found beside the plug-in (run it by hand for dev)\n", exe);
		return 0;
	}

	snprintf(owner, sizeof(owner), "%ld", owner_pid > 0 ? owner_pid : (long)getpid());

	//	How the parent learns that the EXEC failed, as opposed to the fork.
	//
	//	fork() succeeding says nothing about the program starting: the child
	//	only discovers execl has failed after the parent has already moved on,
	//	and _exit(127) into the void looks identical to a healthy launch. This
	//	cost a full debugging pass — the launch reported success, the overlay
	//	never existed, and the connection assertion downstream passed on a
	//	stale connection from the previous test.
	//
	//	The pipe is CLOEXEC, so a successful exec closes it and the parent
	//	reads EOF. A failure writes errno into it first. Read-a-byte or EOF is
	//	then the whole answer.
	int	status_pipe[2];

	if (pipe(status_pipe) != 0) {
		Log("  overlay: status pipe failed, errno=%d\n", errno);
		return 0;
	}
	fcntl(status_pipe[1], F_SETFD, FD_CLOEXEC);

	pid_t pid = fork();

	if (pid < 0) {
		Log("  overlay: fork failed, errno=%d\n", errno);
		close(status_pipe[0]);
		close(status_pipe[1]);
		return 0;
	}
	if (pid == 0) {
		close(status_pipe[0]);
		//	setsid BEFORE exec, so the overlay is a group leader before it can
		//	spawn a single WebKit child. This is the same reasoning as
		//	CreateProcess(CREATE_SUSPENDED) + AssignProcessToJobObject on
		//	Windows: a child born before the grouping is established is outside
		//	it, and outside it is exactly the leak this exists to stop.
		setsid();
		//	DIRECTION-EXPLICIT flags, not tx/rx: "tx" from this side is "rx"
		//	from the overlay's, so the two agreed on the words and disagreed on
		//	the meaning, and the launched overlay opened the wrong pipe for
		//	reading. Naming a channel after what flows through it removes the
		//	perspective entirely.
		execl(exe, exe,
			  "--events", events,
			  "--actions", actions,
			  "--owner-pid", owner,
			  (char *)NULL);
		{
			int	err = errno;
			ssize_t ignored = write(status_pipe[1], &err, sizeof(err));

			(void)ignored;
		}
		_exit(127);
	}

	close(status_pipe[1]);
	{
		int		err = 0;
		ssize_t	got = read(status_pipe[0], &err, sizeof(err));

		close(status_pipe[0]);
		if (got == (ssize_t)sizeof(err)) {
			//	The child reached execl and it failed. Reap it, so a launch
			//	that never happened does not leave a zombie either.
			waitpid(pid, NULL, 0);
			Log("  overlay: exec of %s failed, errno=%d\n", exe, err);
			return 0;
		}
	}

	S_overlay = pid;
	Log("  overlay: launched %s (pid %ld, owner %s, events %s)\n",
		exe, (long)pid, owner, events);
	return 1;
}

void
PieFX_EndOverlay(int grace_ms)
{
	if (!PieFX_OverlayAlive()) {
		S_overlay = -1;
		return;
	}

	//	The GROUP, not the process. Killing the overlay alone leaves its WebKit
	//	children behind, and on Windows that was the process tree that stopped
	//	AE finishing its quit — the fault a job object was introduced to fix.
	//	Negative pid is the group, and the group exists because of the setsid
	//	above.
	kill(-S_overlay, SIGTERM);

	for (int waited = 0; waited < grace_ms; waited += 25) {
		if (!PieFX_OverlayAlive()) {
			Log("  overlay: ended on SIGTERM\n");
			S_overlay = -1;
			return;
		}
		usleep(25 * 1000);
	}

	kill(-S_overlay, SIGKILL);
	//	Reap, so this leaves no zombie behind for AE to carry.
	waitpid(S_overlay, NULL, 0);
	Log("  overlay: did not go on SIGTERM, killed the group\n");
	S_overlay = -1;
}
