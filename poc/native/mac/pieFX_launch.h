//	pieFX — launching the overlay, and ending it, on macOS.
//
//	The Windows original leans on ONE mechanism: a job object with
//	JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE. That covers both ways AE can go —
//	quitting properly, and crashing — because the kernel kills the job when the
//	last handle closes, and process exit closes handles either way.
//
//	macOS has no such thing, so the same guarantee is assembled from two halves
//	that cover one case each:
//
//	  the process GROUP    a deliberate teardown. The overlay is spawned into
//	                       its own group, so one kill takes its WebKit children
//	                       with it. This is what stops the leak that
//	                       TerminateProcess-without-a-job produced on Windows.
//	  --owner-pid          the crash. The overlay watches AE's pid with kqueue
//	                       and exits on its own, with nobody left to tell it to.
//
//	Neither is sufficient alone, which is worth saying because either one looks
//	sufficient in isolation.
#ifndef PIEFX_LAUNCH_H
#define PIEFX_LAUNCH_H

#include <stddef.h>
#include "pieFX_fifo.h"		// PieFXLogFn

#ifdef __cplusplus
extern "C" {
#endif

//	Launch the overlay beside this binary, told which FIFOs to use and whose
//	lifetime to follow. `owner_pid` of 0 means this process, which is what the
//	plug-in wants; the test passes another so it can kill the owner without
//	killing itself. Returns 1 if an overlay is running when it returns —
//	INCLUDING the case where one already was, which is not an error and not a
//	reason to start a second.
int PieFX_LaunchOverlay(const char *events, const char *actions,
                        long owner_pid, PieFXLogFn log, void *log_user);

//	Is the overlay still up? Reaps it if it has exited, so no zombie is left.
int PieFX_OverlayAlive(void);

long PieFX_OverlayPid(void);

//	End it. The caller should ALREADY have sent {"type":"quit"} and given it a
//	moment: this is the fallback, not the first resort, and it escalates —
//	SIGTERM to the group, then SIGKILL — rather than going straight to force.
void PieFX_EndOverlay(int grace_ms);

#ifdef __cplusplus
}
#endif

#endif	// PIEFX_LAUNCH_H
