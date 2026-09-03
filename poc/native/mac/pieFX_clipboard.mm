#include "pieFX_clipboard.h"

#import <AppKit/AppKit.h>
#include <string.h>
#include <stdio.h>

static void
SetErr(char *errZ, size_t cap, const char *msg)
{
	if (errZ && cap) {
		snprintf(errZ, cap, "%s", msg);
	}
}

int
PieFX_PngFileToClipboard(const char *pathZ, char *errZ, size_t err_max)
{
	if (errZ && err_max) {
		errZ[0] = 0;
	}
	if (!pathZ || !*pathZ) {
		SetErr(errZ, err_max, "no frame path");
		return 0;
	}

	@autoreleasepool {
		NSString *path	= [NSString stringWithUTF8String:pathZ];
		NSData	 *png	= path ? [NSData dataWithContentsOfFile:path] : nil;

		if (!png || [png length] == 0) {
			SetErr(errZ, err_max, "the frame file could not be read");
			return 0;
		}

		//	Declared before it is written, which is the NSPasteboard contract:
		//	clearContents bumps the change count and the owner is only allowed
		//	to write the types it has declared.
		NSPasteboard *pb = [NSPasteboard generalPasteboard];

		[pb clearContents];

		//	PNG only. AppKit will hand a TIFF to anything that asks for one,
		//	alpha intact, so there is no second format to build here — see the
		//	header for why Windows needs three and this needs one.
		if (![pb setData:png forType:NSPasteboardTypePNG]) {
			SetErr(errZ, err_max, "the pasteboard refused the image");
			return 0;
		}
	}
	return 1;
}
