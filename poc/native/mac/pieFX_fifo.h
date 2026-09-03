//	pieFX — the macOS transport.
//
//	The FIFO half of what pieFX.cpp does with named pipes on Windows. Same
//	shape, same names, so the two sides read alike:
//
//	    StartPipeServer / StopPipeServer / PipeWrite
//
//	Deliberately free of AEGP and of AE headers, so it builds and runs on its
//	own against the real overlay binary — see fifo_test.cpp. Every transport
//	bug this project has had was caught outside After Effects, and a new
//	transport is exactly the category that harness catches.
#ifndef PIEFX_FIFO_H
#define PIEFX_FIFO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//	One line of JSON from the overlay, without its newline. Called on the
//	server thread, never on the caller's.
typedef void (*PieFXLineFn)(const char *line, void *user);

//	Diagnostics. The plug-in passes its own Log; the test passes printf.
typedef void (*PieFXLogFn)(const char *msg, void *user);

//	Resolve names, create the FIFOs, start the accept/read thread.
//	`events_out` / `actions_out` receive the paths actually used, which the
//	overlay must be launched with (--events / --actions): a second AE finds the
//	base names held and gets its own pair.
int  PieFX_StartPipeServer(char *events_out, size_t events_cap,
                           char *actions_out, size_t actions_cap,
                           PieFXLineFn on_line, void *line_user,
                           PieFXLogFn log, void *log_user);

//	Stop the thread, unblock whatever it is parked in, remove the FIFOs.
//	The overlay should already have been told to quit — see the note in
//	pieFX_fifo.cpp about the order.
void PieFX_StopPipeServer(void);

//	Write one line to the overlay. Safe to call from AE's UI thread: the write
//	is bounded, and an overlay that has stopped reading costs the deadline
//	rather than the session. Returns 1 on success.
int  PieFX_PipeWrite(const char *jsonZ);

//	Whether the overlay is currently connected on both channels.
int  PieFX_PipeConnected(void);

#ifdef __cplusplus
}
#endif

#endif	// PIEFX_FIFO_H
