//	pieFX — the right-hold gesture on macOS. See pieFX_gesture.h.

#import <Cocoa/Cocoa.h>

#include "pieFX_gesture.h"

#include <stdarg.h>
#include <stdio.h>

//	The DEFAULT only. The live value arrives through PieFX_ArmGesture and lives
//	in S_hold_ms below; this is what is used when a caller passes 0.
//
//	It is deliberately not called PIEFX_HOLD_MS any more. The old name matched
//	the one in pieFX.h and was documented as mirroring it, which is exactly how
//	the settings value ended up being parsed and then ignored.
#define PIEFX_HOLD_DEFAULT_MS	200

//	How long after a replay our own click is still expected back.
//
//	This was a COUNT of 2 on the first Mac spike — the DOWN and the UP we post.
//	Phase 0 proved that wrong: once AE opens a context menu, NSMenu's tracking
//	loop consumes the mouse-UP before any local monitor sees it, so the
//	replayed UP usually never comes back, the count sticks at 1, and the NEXT
//	real press is silently eaten as if it were ours. A deadline cannot desync
//	that way.
#define PIEFX_REPLAY_WINDOW_MS	300

static id		S_monitor			= nil;
static BOOL		S_armed				= NO;

static BOOL		S_rdown				= NO;
static BOOL		S_hold_fired		= NO;
static double	S_rdown_time		= 0.0;
static NSPoint	S_rdown_pt			= { 0, 0 };
static NSPoint	S_last_sent			= { -1e9, -1e9 };
static double	S_replay_until		= 0.0;

//	The ORIGINAL down-click, retained the moment it is swallowed. The replay
//	posts this exact object rather than a synthetic stand-in, which is the
//	whole point: it still carries its true window, view and location.
static NSEvent *S_swallowed_down	= nil;

//	dispatch_after blocks cannot be cancelled the way KillTimer cancels a
//	Windows timer, so a stale one from an earlier press is invalidated by
//	generation instead of stopped.
static long		S_press_gen			= 0;

//	The live hold threshold, set by PieFX_ArmGesture. Both places that measure
//	the press read THIS, not a constant — the timer that normally fires it and
//	the drag fallback that catches the case where the timer has not landed yet.
//	They disagreeing is how a setting half-works.
static unsigned	S_hold_ms			= PIEFX_HOLD_DEFAULT_MS;

static PieFXGestureCallbacks	S_cb	= { NULL, NULL, NULL, NULL, NULL };
static PieFXLogFn				S_log	= NULL;
static void					   *S_log_user = NULL;

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

static double
NowMs(void)
{
	return [[NSProcessInfo processInfo] systemUptime] * 1000.0;
}

//	AppKit hands out bottom-left points in a space whose origin is the primary
//	screen's bottom-left. The overlay, the plug-in and Windows all use TOP-LEFT
//	with y down, so the flip happens once, here.
//
//	The reference is the PRIMARY screen's height — screens[0], the one with the
//	menu bar — because the top-left space is anchored to its top-left corner.
//	Not the screen the cursor is on: that would make the origin move with the
//	mouse.
static void
ToTopLeft(NSPoint p, int *x, int *y)
{
	NSArray<NSScreen *>	*screens	= [NSScreen screens];
	CGFloat				 primary_h	= 0.0;

	if ([screens count] > 0) {
		primary_h = [[screens objectAtIndex:0] frame].size.height;
	}
	*x = (int)llround((double)p.x);
	*y = (int)llround((double)(primary_h - p.y));
}

static void
ForgetSwallowedDown(void)
{
	if (S_swallowed_down) {
		[S_swallowed_down release];
		S_swallowed_down = nil;
	}
}

//	Put back a right-click we swallowed but did not want.
//
//	The original NSEvent goes straight back into AE's own queue. Nothing is
//	synthesised, so there is nothing for macOS's Accessibility gate to catch —
//	the CGEventPost replay this replaced worked, but demanded a permission
//	granted to After Effects rather than to the plug-in, which would have put a
//	system prompt in front of every user and been unavailable outright on a
//	managed Mac.
//
//	Order is the one thing to get right. postEvent APPENDS to the queue, while
//	a value returned from the monitor is dispatched in the CURRENT cycle — so
//	the UP must be POSTED after the DOWN rather than returned, or AE sees the
//	release before the press.
static void
ReplaySwallowedClick(NSEvent *upE)
{
	if (!S_swallowed_down) {
		Log("  !! replay skipped: no swallowed DOWN was retained\n");
		return;
	}
	S_replay_until = NowMs() + PIEFX_REPLAY_WINDOW_MS;

	[NSApp postEvent:S_swallowed_down atStart:NO];
	if (upE) {
		[NSApp postEvent:upE atStart:NO];
	}
	Log("  << re-posted the ORIGINAL DOWN+UP; AE gets its context menu\n");
}

static void
OnHoldDetected(const char *sourceZ)
{
	int x, y;

	S_hold_fired	= YES;
	S_last_sent		= NSMakePoint(-1e9, -1e9);
	ToTopLeft(S_rdown_pt, &x, &y);
	Log("  HOLD after %.0fms (via %s) -> summon at (%d,%d)\n",
		NowMs() - S_rdown_time, sourceZ, x, y);
	if (S_cb.hold) {
		S_cb.hold(x, y, S_cb.user);
	}
}

int
PieFX_ArmGesture(const PieFXGestureCallbacks *cb, unsigned hold_ms,
                 PieFXLogFn log, void *log_user)
{
	if (S_armed) {
		return 1;
	}
	if (cb) {
		S_cb = *cb;
	}
	S_log		= log;
	S_log_user	= log_user;
	S_hold_ms	= hold_ms ? hold_ms : PIEFX_HOLD_DEFAULT_MS;

	//	Logged, because "the setting did nothing" is the failure this argument
	//	exists to prevent, and the log is where that is now visible.
	Log("  gesture: hold threshold %ums\n", S_hold_ms);

	NSEventMask mask = NSEventMaskRightMouseDown
					 | NSEventMaskRightMouseUp
					 | NSEventMaskRightMouseDragged;

	//	LOCAL, not global. It sees only events destined for After Effects,
	//	which is exactly why no Accessibility permission is needed — and a
	//	global monitor could not consume events anyway, so it could not
	//	suppress the context menu.
	S_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
													 handler:^NSEvent *(NSEvent *e) {
		if (!S_armed) {
			return e;
		}

		//	Our own replayed click coming back around: pass it through
		//	untouched, and do not treat it as the start of a new gesture.
		if (NowMs() < S_replay_until) {
			return e;
		}

		switch ([e type]) {

			case NSEventTypeRightMouseDown: {
				S_rdown			= YES;
				S_hold_fired	= NO;
				S_rdown_time	= NowMs();
				S_rdown_pt		= [NSEvent mouseLocation];
				S_press_gen++;

				//	The hold clock. On Windows this had to be a thread timer,
				//	because AE's modal drag loop starves everything else.
				//	Phase 0 measured that Cocoa has no such problem: 23 holds
				//	out of 23 arrived via this timer, none via the drag
				//	fallback below.
				long gen = S_press_gen;

				dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
											 (int64_t)(S_hold_ms * NSEC_PER_MSEC)),
							   dispatch_get_main_queue(), ^{
					if (S_rdown && !S_hold_fired && S_press_gen == gen) {
						OnHoldDetected("timer");
					}
				});

				//	AE opens its context menu on DOWN, so the DOWN must go.
				ForgetSwallowedDown();
				S_swallowed_down = [e retain];
				return nil;
			}

			case NSEventTypeRightMouseDragged: {
				//	The fallback for a press whose timer has not landed yet.
				if (S_rdown && !S_hold_fired && (NowMs() - S_rdown_time) >= (double)S_hold_ms) {
					OnHoldDetected("drag");
				}
				if (S_rdown && S_hold_fired) {
					NSPoint	p = [NSEvent mouseLocation];

					//	Raw cursor only, de-duplicated. The overlay owns the
					//	wheel geometry and does its own hit-testing; this side
					//	deliberately has no opinion about which slot is under
					//	the cursor.
					if (p.x != S_last_sent.x || p.y != S_last_sent.y) {
						int x, y;

						S_last_sent = p;
						ToTopLeft(p, &x, &y);
						if (S_cb.move) {
							S_cb.move(x, y, S_cb.user);
						}
					}
				}
				//	Swallowed either way: while a press is ours, AE must not
				//	see the drag.
				return nil;
			}

			case NSEventTypeRightMouseUp: {
				if (S_rdown) {
					double held = NowMs() - S_rdown_time;

					S_rdown = NO;
					if (S_hold_fired) {
						//	Ours. AE saw neither the DOWN nor the UP, so there
						//	is nothing to undo and no menu to suppress.
						//
						//	We do NOT decide what fires: the overlay knows
						//	which slot the cursor is on and sends back a
						//	finished action. Deciding here as well would fire
						//	twice.
						ForgetSwallowedDown();
						Log("  UP after %.0fms — release sent; awaiting the overlay's action\n",
							held);
						if (S_cb.release) {
							S_cb.release(S_cb.user);
						}
					} else {
						//	Not ours after all. Give AE back the click we took.
						//	The real UP is consumed as well, because its DOWN
						//	is already gone and an unmatched release would
						//	confuse AE.
						Log("  UP after %.0fms — short click, handing it back\n", held);
						ReplaySwallowedClick(e);
						ForgetSwallowedDown();
					}
					return nil;
				}
			} break;

			default:
				break;
		}
		return e;
	}];

	S_armed = (S_monitor != nil);
	Log(S_armed ? "  gesture: armed (local monitor, swallow on)\n"
				: "  gesture: addLocalMonitor returned nil\n");
	return S_armed ? 1 : 0;
}

void
PieFX_DisarmGesture(void)
{
	if (S_monitor) {
		[NSEvent removeMonitor:S_monitor];
		S_monitor = nil;
	}
	S_armed			= NO;
	S_rdown			= NO;
	S_hold_fired	= NO;
	ForgetSwallowedDown();
	Log("  gesture: disarmed\n");
}

//	See the header. Asked of the HID layer rather than of our own event stream,
//	because the whole point is that our event stream missed the release.
void
PieFX_GesturePoll(void)
{
	if (!S_armed || !S_rdown) {
		return;
	}
	if (CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,
								 kCGMouseButtonRight)) {
		return;						// still genuinely held
	}

	//	We think it is down; the hardware says otherwise. The release happened
	//	somewhere we could not see, so treat it as a cancel: the wheel must not
	//	be left on screen.
	BOOL had_wheel = S_hold_fired;

	S_rdown			= NO;
	S_hold_fired	= NO;
	ForgetSwallowedDown();
	Log("  backstop: right-up unseen (released off-AE) -> cancel\n");
	if (had_wheel && S_cb.cancel) {
		S_cb.cancel(S_cb.user);
	}
}

int
PieFX_GestureBusy(void)
{
	return S_rdown ? 1 : 0;
}

int
PieFX_GestureArmed(void)
{
	return S_armed ? 1 : 0;
}
