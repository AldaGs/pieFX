//	pieFX — the right-hold gesture on macOS.
//
//	The platform half of what MouseProc does on Windows. The state machine is
//	the same one, deliberately: DOWN starts a clock and is swallowed, a press
//	that outlives the threshold is ours, a press that does not is handed back.
//	What differs is every mechanism underneath it, and each difference is
//	argued where it is made in pieFX_gesture.mm.
//
//	The mechanism itself is not new work — it was proven in Phase 0 (S4: 23
//	holds out of 23, swallow and replay included, no Accessibility permission).
//	This is that code changing address, with the product's semantics.
#ifndef PIEFX_GESTURE_H
#define PIEFX_GESTURE_H

#include "pieFX_fifo.h"		// PieFXLogFn

#ifdef __cplusplus
extern "C" {
#endif

//	All coordinates are POINTS, TOP-LEFT origin — the protocol settled in
//	MAC_PORT.md step 1. NSEvent hands out bottom-left points; the conversion
//	happens once, here, so nothing downstream has to know.
typedef struct {
	//	The press outlived the threshold: summon the wheel at the DOWN point.
	void (*hold)(int x, int y, void *user);
	//	The cursor moved while the wheel is up. Raw position only — the overlay
	//	owns the wheel geometry and does its own hit-testing.
	void (*move)(int x, int y, void *user);
	//	The button came up on a press that was ours. The overlay decides what
	//	fires; this side must not, or it would fire twice.
	void (*release)(void *user);
	//	The press ended somewhere we could not see it — see PieFX_GesturePoll.
	void (*cancel)(void *user);
	void *user;
} PieFXGestureCallbacks;

//	Install the monitor. Swallow is ALWAYS on while armed, which is not a
//	preference: Phase 0 measured that AE's context menu, once open, eats the
//	mouse-UP before any local monitor sees it — 23 DOWNs and exactly 1 UP.
//	Swallowing the DOWN prevents the menu, which prevents the tracking loop,
//	which is what makes the UP visible at all.
int  PieFX_ArmGesture(const PieFXGestureCallbacks *cb, PieFXLogFn log, void *log_user);
void PieFX_DisarmGesture(void);

//	The backstop, called from the idle hook.
//
//	A LOCAL monitor sees only events destined for After Effects, so a
//	right-release over ANOTHER application is invisible to it: the press would
//	stay "down" forever and the wheel would be left on screen. Windows has the
//	identical blind spot with a thread-local WH_MOUSE hook and the identical
//	backstop; only the question "is the button actually up?" is asked
//	differently.
void PieFX_GesturePoll(void);

//	Is a right-press in progress? The idle hook skips its selection refresh
//	during one, exactly as the Windows side skips it while S_rdownB is set.
int  PieFX_GestureBusy(void);
int  PieFX_GestureArmed(void);

#ifdef __cplusplus
}
#endif

#endif	// PIEFX_GESTURE_H
