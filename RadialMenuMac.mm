/*
	RadialMenuMac.mm - Phase 0 spikes S1, S3-Mac, S4 and S5 for After Effects on macOS.

	=====================================================================
	  AUTHORED ON WINDOWS. NEVER COMPILED, NEVER RUN. Assume it is wrong
	  in small ways until a Mac says otherwise. Every Cocoa call here was
	  written from the API contract, not from a working build.
	=====================================================================

	This is a SEPARATE, SELF-CONTAINED plug-in rather than a port of
	RadialMenu.cpp. That is deliberate: the Windows spike is verified and
	working, and refactoring it into a shared core plus two platform layers
	would put that at risk for no gain while the Mac side is still unknown.
	A spike is allowed to duplicate ~80 lines of AEGP glue. Merge them later,
	if ever.

	It registers its own menu commands under its own names, so both plug-ins
	can be installed at once without colliding.

	What it covers, and why in this order:

	  S1-Mac  the AEGP layer builds and runs on macOS at all. Do this FIRST -
	          it is the cheapest possible proof that the toolchain, the PiPL,
	          the entry point and the bundle are all correct. If it fails,
	          nothing below can be trusted.

	  S4      the real spike: NSEvent addLocalMonitorForEventsMatchingMask.
	          "Local" is the payoff - a local monitor only sees events for our
	          own application, and we ARE inside AE's application. No
	          Accessibility permission prompt, no CGEventTap, no notarisation
	          scrutiny over input capture.

	  S3-Mac  the overlay: a borderless transparent NSWindow above AE, and
	          whether taking focus costs the layer selection.

	  S5-Mac  the effects catalogue. Pure AEGP, so it should behave exactly as
	          on Windows. Included as a control: if S5 misbehaves here, the
	          problem is the Mac build, not the API.

	WHAT WINDOWS ALREADY TAUGHT US - carried over as hypotheses, not facts.
	Each is a claim about AE's behaviour, and AE on macOS is a different build.
	Re-check, do not assume:

	  1. Nothing can identify an AE panel. Every Windows panel shared one
	     window class. Almost certainly the same story here, and it no longer
	     matters: the design gates on AE's SELECTION STATE, not panel identity.

	  2. AE opens its context menu on mouse-DOWN, not mouse-UP. This is the one
	     most likely to differ, since macOS menus are NSMenu. VERIFY IT FIRST
	     (see S4_VERIFY_MENU_TIMING below) - the whole swallow strategy depends
	     on the answer.

	  3. Layer selection is document state, not focus state. It survived the
	     overlay taking focus, and survived AE being deactivated by another
	     process. Very likely holds here too, being AE's data model rather than
	     an OS behaviour.

	  4. On Windows a motionless press generated no move events, and AE's modal
	     drag loop starved the AEGP idle hook, so the hold had to be timed by a
	     thread timer. Cocoa's run loop is not Windows' modal loop and
	     dispatch_after does not depend on idle time, so this probably does not
	     arise. The log records which clock won, so we will know rather than
	     assume.

	THE STRATEGY PORTS EVEN WHERE THE CODE DOES NOT:
	  swallow the DOWN; if the press outlives the threshold it is our gesture;
	  if it is released early, replay it so AE's menu still appears.
	Windows needed SendInput for the replay. Here it is CGEventPost.
*/

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include "AEConfig.h"
#include "entry.h"
#include "AE_GeneralPlug.h"
#include "AEGP_SuiteHandler.h"
#include "AE_Macros.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

extern "C" DllExport AEGP_PluginInitFuncPrototype EntryPointFunc;

#define RM_HOLD_MS			200
#define RM_OVERLAY_MS		3000
#define RM_OVERLAY_ARM_MS	4000
#define RM_OVERLAY_SIZE		320

#define RM_MENU_S1		"Radial Menu (Mac) S1: Anchor to Center"
#define RM_MENU_S4		"Radial Menu (Mac) S4: Watch Right-Hold"
#define RM_MENU_S4SWAL	"Radial Menu (Mac) S4: Swallow Hold OFF/ON"
#define RM_MENU_S3		"Radial Menu (Mac) S3: Overlay Test"
#define RM_MENU_S5		"Radial Menu (Mac) S5: Dump Effects Catalogue"

static AEGP_PluginID	S_my_id			= 0L;
static SPBasicSuite		*sP				= NULL;

static AEGP_Command		S_cmd_s1		= 0L;
static AEGP_Command		S_cmd_watch		= 0L;
static AEGP_Command		S_cmd_swallow	= 0L;
static AEGP_Command		S_cmd_overlay	= 0L;
static AEGP_Command		S_cmd_fxdump	= 0L;

//	Byte-identical to the Windows spike's script - it is the one piece that is
//	genuinely the same on both platforms, ES3 and all.
static const A_char *S_anchor_script =
	"(function(){"
	"  var c = app.project.activeItem;"
	"  if (!(c instanceof CompItem)) { return 'no active comp'; }"
	"  var sel = c.selectedLayers;"
	"  if (sel.length === 0) { return 'no layer selected'; }"
	"  app.beginUndoGroup('Radial Menu S1: Anchor to Center');"
	"  var moved = 0;"
	"  for (var i = 0; i < sel.length; i++) {"
	"    var L = sel[i];"
	"    var ap = L.property('Anchor Point');"
	"    if (!ap) { continue; }"
	"    var r;"
	"    try { r = L.sourceRectAtTime(c.time, false); } catch (e) { continue; }"
	"    var oldA = ap.value;"
	"    var cx = r.left + r.width / 2;"
	"    var cy = r.top + r.height / 2;"
	"    var dx = cx - oldA[0];"
	"    var dy = cy - oldA[1];"
	"    var s = L.property('Scale').value;"
	"    var sx = dx * s[0] / 100;"
	"    var sy = dy * s[1] / 100;"
	"    var rot = L.threeDLayer ? L.property('Z Rotation').value : L.property('Rotation').value;"
	"    var th = rot * Math.PI / 180;"
	"    var ct = Math.cos(th), st = Math.sin(th);"
	"    var pdx = sx * ct - sy * st;"
	"    var pdy = sx * st + sy * ct;"
	"    var pos = L.property('Position');"
	"    var p = pos.value;"
	"    if (L.threeDLayer) {"
	"      ap.setValue([cx, cy, oldA[2]]);"
	"      pos.setValue([p[0] + pdx, p[1] + pdy, p[2]]);"
	"    } else {"
	"      ap.setValue([cx, cy]);"
	"      pos.setValue([p[0] + pdx, p[1] + pdy]);"
	"    }"
	"    moved++;"
	"  }"
	"  app.endUndoGroup();"
	"  return 'moved ' + moved + ' layer(s)';"
	"})()";


#pragma mark - logging

static char	S_log_path[1024] = { 0 };

static void
LogPathInit(const char *leafZ)
{
	//	NSTemporaryDirectory() is per-user and always writable, which /tmp is not
	//	guaranteed to be under a hardened runtime.
	NSString *dir = NSTemporaryDirectory();
	NSString *p   = [dir stringByAppendingPathComponent:[NSString stringWithUTF8String:leafZ]];
	strncpy(S_log_path, [p UTF8String], sizeof(S_log_path) - 1);
	S_log_path[sizeof(S_log_path) - 1] = 0;
}

static void
Log(const char *fmtZ, ...)
{
	if (!S_log_path[0]) {
		return;
	}

	FILE *fp = fopen(S_log_path, "a");
	if (!fp) {
		return;
	}

	va_list args;
	va_start(args, fmtZ);
	vfprintf(fp, fmtZ, args);
	va_end(args);
	fclose(fp);
}

static void
Report(const char *fmtZ, ...)
{
	char	msg[2048];
	va_list	args;

	va_start(args, fmtZ);
	vsnprintf(msg, sizeof(msg), fmtZ, args);
	va_end(args);

	AEGP_SuiteHandler suites(sP);
	suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
}

//	The same measurement the Windows spike used for the focus question.
static A_u_long
CountSelection(AEGP_SuiteHandler &suites)
{
	A_Err		err		= A_Err_NONE;
	AEGP_ItemH	itemH	= NULL;
	A_u_long	count	= 0;

	ERR(suites.ItemSuite6()->AEGP_GetFirstProjItem(NULL, &itemH));

	while (!err && itemH) {
		A_Boolean	selectedB	= FALSE;
		AEGP_ItemH	nextH		= NULL;

		ERR(suites.ItemSuite6()->AEGP_IsItemSelected(itemH, &selectedB));
		if (!err && selectedB) {
			count++;
		}

		ERR(suites.ItemSuite6()->AEGP_GetNextProjItem(NULL, itemH, &nextH));
		itemH = nextH;
	}
	return count;
}


#pragma mark - S4: the local event monitor

static id		S_monitor			= nil;
static BOOL		S_watch_on			= NO;
static BOOL		S_swallow_on		= NO;

static BOOL		S_rdown				= NO;
static BOOL		S_hold_fired		= NO;
static double	S_rdown_time		= 0.0;
static NSPoint	S_rdown_pt			= { 0, 0 };
static int		S_hold_count		= 0;
static int		S_click_count		= 0;
static int		S_replay_count		= 0;

//	Counts events WE injected, so the monitor does not mistake its own replay
//	for a new gesture. A counter rather than a flag, for the same reason as on
//	Windows: injected events arrive asynchronously, after any flag would already
//	have been cleared.
static int		S_replay_pending	= 0;

//	Generation counter, so a stale dispatch_after from an earlier press cannot
//	fire a hold for a press that has already ended. Windows killed its timer;
//	dispatch_after blocks cannot be cancelled, so they get invalidated instead.
static long		S_press_gen			= 0;

static double
NowMs(void)
{
	return [[NSProcessInfo processInfo] systemUptime] * 1000.0;
}

static void
OnHoldDetected(const char *sourceZ)
{
	S_hold_fired = YES;
	S_hold_count++;

	NSBeep();	// audible, and it cannot disturb the input being measured

	Log("  HOLD detected after %.0fms (hold #%d, via %s)\n",
		NowMs() - S_rdown_time, S_hold_count, sourceZ);
}

//	Put back a right-click we swallowed but did not want.
//
//	kCGHIDEventTap posts at the lowest level, so the event travels the same path
//	a real click would - which is what we want, since (as on Windows) we are in
//	no position to address the event to a particular view.
static void
ReplayRightClick(NSPoint screenPt)
{
	//	CGEvent uses a TOP-LEFT origin; NSEvent mouseLocation is BOTTOM-LEFT,
	//	and the flip is against the primary screen, not the one under the
	//	cursor. Worth checking on a multi-monitor setup with a screen above or
	//	below the primary - a replayed click landing in the wrong place would
	//	show up as "the menu opened somewhere else".
	CGFloat	screenH	= NSMaxY([[[NSScreen screens] objectAtIndex:0] frame]);
	CGPoint	cgPt	= CGPointMake(screenPt.x, screenH - screenPt.y);

	CGEventRef down = CGEventCreateMouseEvent(NULL, kCGEventRightMouseDown, cgPt, kCGMouseButtonRight);
	CGEventRef up   = CGEventCreateMouseEvent(NULL, kCGEventRightMouseUp,   cgPt, kCGMouseButtonRight);

	if (!down || !up) {
		if (down) { CFRelease(down); }
		if (up)   { CFRelease(up); }
		Log("  !! REPLAY FAILED: could not create CGEvents\n");
		return;
	}

	S_replay_pending = 2;

	CGEventPost(kCGHIDEventTap, down);
	CGEventPost(kCGHIDEventTap, up);

	CFRelease(down);
	CFRelease(up);

	S_replay_count++;
	Log("  << replayed the click (#%d) - AE's menu should appear now\n", S_replay_count);
}

static void
InstallMonitor(void)
{
	NSEventMask mask = NSEventMaskRightMouseDown
					 | NSEventMaskRightMouseUp
					 | NSEventMaskRightMouseDragged;

	//	LOCAL, not global. It sees only events destined for AE, which is exactly
	//	why no Accessibility permission is needed - and a global monitor could
	//	not consume events anyway.
	S_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
													 handler:^NSEvent *(NSEvent *e) {

		if (!S_watch_on) {
			return e;
		}

		//	Our own injected click coming back around.
		if (S_replay_pending > 0 &&
			(e.type == NSEventTypeRightMouseDown || e.type == NSEventTypeRightMouseUp)) {
			S_replay_pending--;
			return e;
		}

		switch (e.type) {

			case NSEventTypeRightMouseDown: {
				S_rdown			= YES;
				S_hold_fired	= NO;
				S_rdown_time	= NowMs();
				S_rdown_pt		= [NSEvent mouseLocation];
				S_press_gen++;

				Log("\n--- right button DOWN ---\n");
				Log("  press at (%.0f,%.0f)  windowNumber=%ld\n",
					S_rdown_pt.x, S_rdown_pt.y, (long)[e windowNumber]);

				//	The hold clock. On Windows this had to be a thread timer,
				//	because AE's modal drag loop starved everything else. Here
				//	the main queue should keep running during a press. If
				//	"via timer" never appears in the log, that assumption is
				//	wrong and this needs a CFRunLoopTimer added to the event
				//	tracking run loop mode instead.
				long gen = S_press_gen;
				dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
											 (int64_t)(RM_HOLD_MS * NSEC_PER_MSEC)),
							   dispatch_get_main_queue(), ^{
					if (S_rdown && !S_hold_fired && S_press_gen == gen) {
						OnHoldDetected("timer");
					}
				});

				if (S_swallow_on) {
					Log("  >> swallowed the DOWN\n");
					return nil;			// consume
				}
			} break;

			case NSEventTypeRightMouseDragged: {
				if (S_rdown && !S_hold_fired && (NowMs() - S_rdown_time) >= RM_HOLD_MS) {
					OnHoldDetected("drag");
				}
				if (S_swallow_on) {
					return nil;
				}
			} break;

			case NSEventTypeRightMouseUp: {
				if (S_rdown) {
					double held = NowMs() - S_rdown_time;

					if (S_hold_fired) {
						Log("  right button UP after %.0fms - was a HOLD\n", held);
						S_rdown = NO;

						//	Ours. AE saw neither the DOWN nor the UP, so there
						//	is nothing to undo and no menu to suppress.
						if (S_swallow_on) {
							Log("  >> swallowed the UP too. AE never saw this press.\n");
							return nil;
						}
					} else {
						S_click_count++;
						Log("  right button UP after %.0fms - short CLICK #%d\n", held, S_click_count);
						S_rdown = NO;

						//	Not ours after all. Give AE back the click we took.
						//	The real UP is consumed as well, because its DOWN is
						//	already gone and an unmatched release would confuse AE.
						if (S_swallow_on) {
							ReplayRightClick(S_rdown_pt);
							return nil;
						}
					}
				}
			} break;

			default:
				break;
		}

		return e;
	}];
}

static void
RemoveMonitor(void)
{
	if (S_monitor) {
		[NSEvent removeMonitor:S_monitor];
		S_monitor = nil;
	}
}

static void
WatchToggle(void)
{
	if (S_watch_on) {
		S_watch_on		= NO;
		S_swallow_on	= NO;
		RemoveMonitor();

		Log("\n=== watch OFF: %d hold(s), %d short click(s), %d replayed ===\n",
			S_hold_count, S_click_count, S_replay_count);

		Report("S4 watch OFF.\n\n%d hold(s), %d short click(s), %d replayed.\n\nLog: %s",
				S_hold_count, S_click_count, S_replay_count, S_log_path);
		return;
	}

	LogPathInit("RadialMenu_S4_gesture.txt");

	FILE *fp = fopen(S_log_path, "w");
	if (fp) {
		fprintf(fp, "Radial Menu (Mac) - S4 right-hold watch\n");
		fprintf(fp, "Hold threshold: %dms. Local NSEvent monitor.\n\n", RM_HOLD_MS);
		fclose(fp);
	}

	S_hold_count		= 0;
	S_click_count		= 0;
	S_replay_count		= 0;
	S_replay_pending	= 0;
	S_rdown				= NO;
	S_watch_on			= YES;

	InstallMonitor();

	if (!S_monitor) {
		S_watch_on = NO;
		Report("S4: addLocalMonitorForEventsMatchingMask returned nil.\n\n"
				"That is the spike FAILING, not a bug to work around - the fallback\n"
				"is hotkey-only summon on macOS, per the roadmap's gate table.");
		return;
	}

	/*	S4_VERIFY_MENU_TIMING
		Do this before trusting anything else.

		With the watch ON but swallow still OFF, right-click normally and watch
		WHEN AE's context menu appears relative to the log's DOWN and UP lines.
		On Windows the menu came up on DOWN, which is why the DOWN is what gets
		swallowed. If macOS raises it on UP instead, the strategy simplifies a
		lot: swallow only the UP of a press that already crossed the threshold,
		and the entire CGEventPost replay path can be deleted.

		Judge this BY EYE. On Windows a message trace said the opposite of what
		was actually on screen, and cost a full test cycle.
	*/

	Report("S4 watch ON (threshold %dms).\n\n"
			"FIRST, with swallow still OFF: right-click normally and note WHEN the menu\n"
			"appears - on press, or on release? That answers the question the whole\n"
			"strategy depends on, and it must be judged by eye, not from the log.\n\n"
			"Then press and hold still - you should hear a beep at %dms.\n\n"
			"Log: %s",
			RM_HOLD_MS, RM_HOLD_MS, S_log_path);
}

static void
SwallowToggle(void)
{
	if (!S_watch_on) {
		Report("S4: turn the watch on first.");
		return;
	}

	S_swallow_on = !S_swallow_on;
	Log("\n=== swallow-on-hold %s ===\n", S_swallow_on ? "ARMED" : "disarmed");

	if (S_swallow_on) {
		Report("S4 swallow ARMED.\n\n"
				"Every right-button DOWN is now consumed. A press past %dms is ours; a\n"
				"shorter one is replayed with CGEventPost so AE's menu still opens.\n\n"
				"By eye:\n"
				"(1) a hold shows NO menu.\n"
				"(2) a short right-click still shows the SAME menu, in the right place.\n"
				"(3) no double menus, no stuck menus, no lost clicks.\n"
				"(4) AE still feels normal afterwards - drags, selection, panels.",
				RM_HOLD_MS);
	} else {
		Report("S4 swallow disarmed.");
	}
}


#pragma mark - S3-Mac: the overlay window

@interface RMRingView : NSView
@end

@implementation RMRingView

- (void)drawRect:(NSRect)dirty
{
	NSRect	b		= self.bounds;
	CGFloat	outer	= MIN(NSWidth(b), NSHeight(b)) / 2.0 - 6.0;
	CGFloat	thick	= 26.0;
	NSPoint	c		= NSMakePoint(NSMidX(b), NSMidY(b));

	//	Stroke centre-line sits half a line width inside the outer edge.
	NSRect	ringRect = NSMakeRect(c.x - outer + thick / 2.0,
								  c.y - outer + thick / 2.0,
								  (outer - thick / 2.0) * 2.0,
								  (outer - thick / 2.0) * 2.0);

	NSBezierPath *path = [NSBezierPath bezierPathWithOvalInRect:ringRect];
	[path setLineWidth:thick];
	[[NSColor colorWithSRGBRed:1.0 green:0.45 blue:0.1 alpha:0.85] set];
	[path stroke];
}

@end

static NSWindow	*S_overlay		= nil;
static A_u_long	S_sel_before	= 0;

static void
CloseOverlay(void)
{
	if (!S_overlay) {
		return;
	}

	[S_overlay orderOut:nil];
	S_overlay = nil;

	AEGP_SuiteHandler	suites(sP);
	A_u_long			after = CountSelection(suites);

	Report("S3-Mac overlay closed.\n\n"
			"Selected items BEFORE: %lu\n"
			"Selected items AFTER:  %lu\n%s\n\n"
			"By eye: did the ring sit ABOVE everything, and were its edges soft?",
			(unsigned long)S_sel_before,
			(unsigned long)after,
			(S_sel_before == after)
				? "==> SELECTION SURVIVED the focus round-trip."
				: "==> SELECTION CHANGED. The overlay must not take focus.");
}

static void
CreateOverlayNow(void)
{
	AEGP_SuiteHandler suites(sP);

	S_sel_before = CountSelection(suites);

	NSPoint	pt		= [NSEvent mouseLocation];	// screen coords, bottom-left origin
	NSRect	frame	= NSMakeRect(pt.x - RM_OVERLAY_SIZE / 2.0,
								 pt.y - RM_OVERLAY_SIZE / 2.0,
								 RM_OVERLAY_SIZE, RM_OVERLAY_SIZE);

	S_overlay = [[NSWindow alloc] initWithContentRect:frame
											styleMask:NSWindowStyleMaskBorderless
											  backing:NSBackingStoreBuffered
												defer:NO];

	[S_overlay setOpaque:NO];
	[S_overlay setBackgroundColor:[NSColor clearColor]];
	[S_overlay setHasShadow:NO];

	//	Windows needed WS_EX_TOPMOST to beat AE's panels; this is the equivalent.
	//	NSStatusWindowLevel sits above normal and floating windows. If it still
	//	loses to something, try NSScreenSaverWindowLevel before concluding the
	//	spike has failed.
	[S_overlay setLevel:NSStatusWindowLevel];

	//	Appear on the active Space, and over a full-screen AE.
	[S_overlay setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
									 NSWindowCollectionBehaviorFullScreenAuxiliary];

	[S_overlay setContentView:[[RMRingView alloc] initWithFrame:
								NSMakeRect(0, 0, RM_OVERLAY_SIZE, RM_OVERLAY_SIZE)]];

	//	Take focus ON PURPOSE. On Windows this proved safe - selection is
	//	document state, not focus state - and the counter above is what proves
	//	or disproves it here.
	[S_overlay makeKeyAndOrderFront:nil];

	NSBeep();

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(RM_OVERLAY_MS * NSEC_PER_MSEC)),
				   dispatch_get_main_queue(), ^{
		CloseOverlay();
	});
}

static void
ShowOverlay(void)
{
	if (S_overlay) {
		CloseOverlay();
		return;
	}

	//	Arm, do not show. Otherwise the ring always appears over the menu item
	//	that launched it, which is the one screen position we do not care about.
	Report("S3-Mac armed.\n\n"
			"Dismiss this, then move the cursor over whatever you want to test.\n"
			"In %d seconds the ring appears there, stays %d seconds, and closes itself.\n\n"
			"Targets: comp viewer, timeline, a CEP panel, second monitor.",
			RM_OVERLAY_ARM_MS / 1000, RM_OVERLAY_MS / 1000);

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(RM_OVERLAY_ARM_MS * NSEC_PER_MSEC)),
				   dispatch_get_main_queue(), ^{
		CreateOverlayNow();
	});
}


#pragma mark - S5-Mac: effects catalogue (pure AEGP, included as a control)

static void
DumpEffectCatalogue(AEGP_SuiteHandler &suites)
{
	A_Err	err		= A_Err_NONE;
	A_long	claimed	= 0;

	LogPathInit("RadialMenu_S5_effects.txt");

	ERR(suites.EffectSuite4()->AEGP_GetNumInstalledEffects(&claimed));

	FILE *fp = fopen(S_log_path, "w");
	if (!fp) {
		Report("S5-Mac: could not open the log for writing.");
		return;
	}

	fprintf(fp, "Radial Menu (Mac) - S5 installed effects catalogue\n");
	fprintf(fp, "AEGP_GetNumInstalledEffects reports: %ld\n\n", claimed);
	fprintf(fp, "%-5s  %-28s  %-44s  %s\n", "#", "CATEGORY", "DISPLAY NAME", "MATCH NAME");

	AEGP_InstalledEffectKey	key		= AEGP_InstalledEffectKey_NONE;
	A_long					walked	= 0;

	while (!err) {
		ERR(suites.EffectSuite4()->AEGP_GetNextInstalledEffect(key, &key));

		if (err || AEGP_InstalledEffectKey_NONE == key) {
			break;
		}

		A_char	name[AEGP_MAX_EFFECT_NAME_SIZE]			= { 0 };
		A_char	match[AEGP_MAX_EFFECT_MATCH_NAME_SIZE]	= { 0 };
		A_char	cat[AEGP_MAX_EFFECT_CATEGORY_NAME_SIZE]	= { 0 };

		ERR(suites.EffectSuite4()->AEGP_GetEffectName(key, name));
		ERR(suites.EffectSuite4()->AEGP_GetEffectMatchName(key, match));
		ERR(suites.EffectSuite4()->AEGP_GetEffectCategory(key, cat));

		walked++;
		fprintf(fp, "%-5ld  %-28s  %-44s  %s\n", walked, cat, name, match);
	}

	fprintf(fp, "\nWalked %ld entries; AE claimed %ld. %s\n", walked, claimed,
			(walked == claimed) ? "MATCH." : "*** MISMATCH - enumeration incomplete. ***");
	fclose(fp);

	//	Windows saw 519/519. A different TOTAL here is expected - a different
	//	machine has different plug-ins installed - but walked != claimed is not.
	Report("S5-Mac: walked %ld effects; AE claims %ld. %s\n\n%s",
			walked, claimed,
			(walked == claimed) ? "Counts match." : "COUNTS DISAGREE.",
			S_log_path);
}


#pragma mark - AEGP plumbing

static A_Err
ReportAndDisposeHandle(AEGP_SuiteHandler &suites, AEGP_MemHandle handleH, const A_char *prefixZ)
{
	A_Err	err = A_Err_NONE;

	if (!handleH) {
		return err;
	}

	A_char *textP = NULL;
	ERR(suites.MemorySuite1()->AEGP_LockMemHandle(handleH, reinterpret_cast<void**>(&textP)));

	//	AE returns a non-NULL but EMPTY error handle on success. Test the
	//	string, not the handle, or every run fires a blank alert.
	if (!err && textP && textP[0] != 0) {
		Report("%s%s", prefixZ, textP);
	}

	ERR(suites.MemorySuite1()->AEGP_UnlockMemHandle(handleH));
	ERR(suites.MemorySuite1()->AEGP_FreeMemHandle(handleH));
	return err;
}

static A_Err
DeathHook(AEGP_GlobalRefcon plugin_refconP, AEGP_DeathRefcon refconP)
{
	//	Leaving a monitor installed as the bundle unloads is how you take the
	//	host down with you.
	RemoveMonitor();
	S_watch_on = NO;
	return A_Err_NONE;
}

static A_Err
UpdateMenuHook(AEGP_GlobalRefcon plugin_refconPV, AEGP_UpdateMenuRefcon refconPV,
			   AEGP_WindowType active_window)
{
	A_Err				err		= A_Err_NONE,
						err2	= A_Err_NONE;
	AEGP_ItemH			itemH	= NULL;
	AEGP_ItemType		type	= AEGP_ItemType_NONE;
	AEGP_SuiteHandler	suites(sP);

	ERR(suites.ItemSuite6()->AEGP_GetActiveItem(&itemH));

	if (!err && itemH) {
		ERR(suites.ItemSuite6()->AEGP_GetItemType(itemH, &type));
	}

	if (!err && itemH && AEGP_ItemType_COMP == type) {
		ERR(suites.CommandSuite1()->AEGP_EnableCommand(S_cmd_s1));
	} else {
		ERR2(suites.CommandSuite1()->AEGP_DisableCommand(S_cmd_s1));
	}

	ERR2(suites.CommandSuite1()->AEGP_EnableCommand(S_cmd_watch));
	ERR2(suites.CommandSuite1()->AEGP_EnableCommand(S_cmd_swallow));
	ERR2(suites.CommandSuite1()->AEGP_EnableCommand(S_cmd_overlay));
	ERR2(suites.CommandSuite1()->AEGP_EnableCommand(S_cmd_fxdump));
	return err;
}

static A_Err
CommandHook(AEGP_GlobalRefcon plugin_refconPV, AEGP_CommandRefcon refconPV,
			AEGP_Command command, AEGP_HookPriority hook_priority,
			A_Boolean already_handledB, A_Boolean *handledPB)
{
	A_Err				err = A_Err_NONE;
	AEGP_SuiteHandler	suites(sP);

	if (S_cmd_s1 == command) {
		AEGP_MemHandle resultH = NULL, errorH = NULL;

		ERR(suites.UtilitySuite6()->AEGP_ExecuteScript(S_my_id, S_anchor_script, FALSE,
														&resultH, &errorH));
		ERR(ReportAndDisposeHandle(suites, errorH,  "S1 script error: "));
		ERR(ReportAndDisposeHandle(suites, resultH, "S1: "));
		*handledPB = TRUE;

	} else if (S_cmd_watch == command) {
		WatchToggle();
		*handledPB = TRUE;

	} else if (S_cmd_swallow == command) {
		SwallowToggle();
		*handledPB = TRUE;

	} else if (S_cmd_overlay == command) {
		ShowOverlay();
		*handledPB = TRUE;

	} else if (S_cmd_fxdump == command) {
		DumpEffectCatalogue(suites);
		*handledPB = TRUE;
	}
	return err;
}

//	FIVE parameters. The Commando sample shows a stale seven-param form with
//	file_pathZ/res_pathZ; using it makes this an OVERLOAD of the extern "C"
//	declaration, which links clean and exports mangled, and AE then reports
//	"Couldn't find main entry point". That cost one launch on Windows. Verify
//	after building with:
//
//	    nm -gU RadialMenuMac.plugin/Contents/MacOS/RadialMenuMac | grep EntryPoint
//
//	It must show a bare _EntryPointFunc, not a mangled __Z... symbol.
DllExport A_Err
EntryPointFunc(
	struct SPBasicSuite		*pica_basicP,		/* >> */
	A_long					major_versionL,		/* >> */
	A_long					minor_versionL,		/* >> */
	AEGP_PluginID			aegp_plugin_id,		/* >> */
	AEGP_GlobalRefcon		*global_refconP)	/* << */
{
	A_Err	err		= A_Err_NONE,
			err2	= A_Err_NONE;

	sP		= pica_basicP;
	S_my_id	= aegp_plugin_id;	// the sample forgets this; everything below needs it

	AEGP_SuiteHandler suites(pica_basicP);

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_cmd_s1));
	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(S_cmd_s1, RM_MENU_S1,
										AEGP_Menu_WINDOW, AEGP_MENU_INSERT_SORTED));

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_cmd_watch));
	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(S_cmd_watch, RM_MENU_S4,
										AEGP_Menu_WINDOW, AEGP_MENU_INSERT_SORTED));

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_cmd_swallow));
	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(S_cmd_swallow, RM_MENU_S4SWAL,
										AEGP_Menu_WINDOW, AEGP_MENU_INSERT_SORTED));

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_cmd_overlay));
	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(S_cmd_overlay, RM_MENU_S3,
										AEGP_Menu_WINDOW, AEGP_MENU_INSERT_SORTED));

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_cmd_fxdump));
	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(S_cmd_fxdump, RM_MENU_S5,
										AEGP_Menu_WINDOW, AEGP_MENU_INSERT_SORTED));

	ERR(suites.RegisterSuite5()->AEGP_RegisterCommandHook(S_my_id, AEGP_HP_BeforeAE,
										AEGP_Command_ALL, CommandHook, 0));
	ERR(suites.RegisterSuite5()->AEGP_RegisterUpdateMenuHook(S_my_id, UpdateMenuHook, NULL));
	ERR(suites.RegisterSuite5()->AEGP_RegisterDeathHook(S_my_id, DeathHook, NULL));

	if (err) {
		ERR2(suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id,
				"Radial Menu (Mac) failed to register."));
	}
	return err;
}
