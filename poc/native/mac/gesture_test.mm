//	pieFX — the gesture, in a host that is not After Effects.
//
//	The transport could be proven headlessly. This cannot: a LOCAL event
//	monitor only sees events destined for its own application, which is the
//	property that makes it work without an Accessibility grant and also the
//	property that makes it untestable from outside. So the test supplies an
//	application.
//
//	The window carries a CONTEXT MENU on purpose. It is AE's behaviour in
//	miniature, and it makes both halves visible rather than merely logged:
//
//	  a SHORT right-click   the menu opens. The DOWN was swallowed and handed
//	                        back, so the click reached the app anyway.
//	  a right-click HELD    the menu does NOT open, and a hold is reported.
//	                        The DOWN never reached the app at all.
//
//	If the menu opens on a hold, the swallow is broken. If it never opens on a
//	short click, the replay is broken. Neither shows up in a log that only
//	records what the monitor saw.
//
//	    ./poc/native/mac/build_gesture_test.sh && .../pieFX_gesture_test
//
#import <Cocoa/Cocoa.h>

#include "pieFX_gesture.h"

#include <stdio.h>

static int	S_holds		= 0;
static int	S_moves		= 0;
static int	S_releases	= 0;

static void
OnLog(const char *msg, void *)
{
	fputs(msg, stdout);
	fflush(stdout);
}

static void
OnHold(int x, int y, void *)
{
	S_holds++;
	printf("  >> HOLD #%d — summon at (%d, %d)  [points, top-left]\n", S_holds, x, y);
	fflush(stdout);
}

static void
OnMove(int x, int y, void *)
{
	S_moves++;
	//	One line per move would bury everything else; the count is the point.
	if (S_moves % 10 == 0) {
		printf("     cursor (%d, %d)  [%d moves]\n", x, y, S_moves);
		fflush(stdout);
	}
}

static void
OnRelease(void *)
{
	S_releases++;
	printf("  >> RELEASE #%d (after %d cursor updates)\n", S_releases, S_moves);
	S_moves = 0;
	fflush(stdout);
}

@interface Delegate : NSObject <NSApplicationDelegate>
@property (retain) NSWindow *win;
@end

@implementation Delegate

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
	(void)n;
	NSRect frame = NSMakeRect(0, 0, 620, 360);

	self.win = [[NSWindow alloc] initWithContentRect:frame
										   styleMask:(NSWindowStyleMaskTitled |
													  NSWindowStyleMaskClosable)
											 backing:NSBackingStoreBuffered
											   defer:NO];
	[self.win setTitle:@"pieFX gesture test"];
	[self.win center];

	NSTextField *label = [[NSTextField alloc] initWithFrame:NSInsetRect(frame, 24, 24)];

	[label setEditable:NO];
	[label setBezeled:NO];
	[label setDrawsBackground:NO];
	[label setFont:[NSFont systemFontOfSize:15]];
	[label setStringValue:
		@"Right-click in this window.\n\n"
		 "SHORT click  →  the context menu should OPEN.\n"
		 "                The click was swallowed and handed back.\n\n"
		 "HOLD (>200ms) →  the menu must NOT open, and the terminal\n"
		 "                should report a HOLD. Drag while holding to\n"
		 "                see cursor updates, then let go for a RELEASE.\n\n"
		 "If the menu opens on a hold, the swallow is broken.\n"
		 "If it never opens on a short click, the replay is broken."];
	[[self.win contentView] addSubview:label];

	//	The stand-in for AE's context menu — the thing the swallow has to
	//	suppress and the replay has to restore.
	NSMenu *menu = [[NSMenu alloc] initWithTitle:@"ctx"];

	[menu addItemWithTitle:@"AE would show a context menu here"
					action:NULL
			 keyEquivalent:@""];
	[[self.win contentView] setMenu:menu];

	[self.win makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];

	PieFXGestureCallbacks cb;

	cb.hold		= OnHold;
	cb.move		= OnMove;
	cb.release	= OnRelease;
	cb.user		= NULL;
	if (!PieFX_ArmGesture(&cb, OnLog, NULL)) {
		printf("FAIL: could not arm the gesture\n");
	}
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a
{
	(void)a;
	return YES;
}

- (void)applicationWillTerminate:(NSNotification *)n
{
	(void)n;
	PieFX_DisarmGesture();
	printf("\n%d hold(s), %d release(s). Close the window to finish.\n",
		   S_holds, S_releases);
}

@end

int
main(void)
{
	@autoreleasepool {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

		Delegate *d = [[Delegate alloc] init];

		[NSApp setDelegate:d];
		printf("pieFX gesture test — right-click in the window.\n");
		fflush(stdout);
		[NSApp run];
	}
	return 0;
}
