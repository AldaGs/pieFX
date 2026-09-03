//	pieFX — the clipboard, in a host that is not AE.
//
//	The half of copy-frame that AE owns (render a frame to a PNG) is
//	ExtendScript and cannot be driven from here. The half THIS side owns —
//	those bytes onto the pasteboard — needs no AE at all, so it gets checked
//	here rather than by pasting into something by hand and squinting.
//
//	What is worth asserting is not "did setData return YES". It is that a
//	consumer reading the pasteboard afterwards gets the same image back,
//	including its ALPHA, because alpha is the whole reason the Windows version
//	needs three formats and this one needs one. So the test writes a PNG with a
//	transparent half, puts it on the pasteboard, and reads it back BOTH ways a
//	real consumer would: as PNG data, and as an NSImage — which is the path
//	that would silently flatten alpha if anything here were wrong.
//
//	  ./poc/native/mac/build_clipboard_test.sh
//	  poc/overlay/src-tauri/target/release/pieFX_clipboard_test
//
//	NOTE: this REPLACES the contents of the user's clipboard. That is inherent
//	to testing a clipboard, and it is why this is a separate harness rather
//	than something the other tests run in passing.
#import <AppKit/AppKit.h>
#include "pieFX_clipboard.h"
#include <stdio.h>

static int S_pass = 0;
static int S_fail = 0;

static void
Check(int ok, const char *what)
{
	printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
	ok ? S_pass++ : S_fail++;
}

//	A 4x4 PNG: the left half opaque red, the right half fully transparent.
static NSString *
WriteTestPng(void)
{
	NSBitmapImageRep *rep =
		[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
											   pixelsWide:4
											   pixelsHigh:4
											bitsPerSample:8
										  samplesPerPixel:4
												 hasAlpha:YES
												 isPlanar:NO
										   colorSpaceName:NSDeviceRGBColorSpace
											  bytesPerRow:0
											 bitsPerPixel:0];

	for (NSInteger y = 0; y < 4; y++) {
		for (NSInteger x = 0; x < 4; x++) {
			NSUInteger px[4] = { 255, 0, 0, (x < 2) ? 255u : 0u };

			[rep setPixel:px atX:x y:y];
		}
	}

	NSData	 *png  = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
	NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:
					  @"pieFX_clipboard_test.png"];

	return [png writeToFile:path atomically:YES] ? path : nil;
}

int
main(void)
{
	@autoreleasepool {
		char err[200] = { 0 };

		printf("pieFX_clipboard_test\n\n");
		printf("  NOTE: this replaces your clipboard contents.\n\n");

		NSString *path = WriteTestPng();

		Check(path != nil, "a 4x4 test PNG with a transparent half was written");
		if (!path) { return 1; }
		printf("         %s\n", [path UTF8String]);

		printf("\nputting it on the pasteboard\n");
		Check(PieFX_PngFileToClipboard([path UTF8String], err, sizeof(err)) == 1,
			  "PieFX_PngFileToClipboard succeeds");
		Check(err[0] == 0, "and leaves no error message behind");

		printf("\nreading it back as a consumer would\n");
		NSPasteboard *pb = [NSPasteboard generalPasteboard];

		NSData *back = [pb dataForType:NSPasteboardTypePNG];
		Check(back != nil && [back length] > 0, "PNG data comes back off the pasteboard");

		NSData *onDisk = [NSData dataWithContentsOfFile:path];
		Check(back && [back isEqualToData:onDisk],
			  "byte-identical to the file, i.e. nothing re-encoded it");

		//	The path that matters. AppKit derives this from the PNG we wrote,
		//	and it is what an app that wants a bitmap rather than a file
		//	actually receives.
		NSImage *img = [[NSImage alloc] initWithPasteboard:pb];

		Check(img != nil, "an NSImage can be built from the pasteboard");

		if (img) {
			NSBitmapImageRep *rep = nil;

			for (NSImageRep *r in [img representations]) {
				if ([r isKindOfClass:[NSBitmapImageRep class]]) {
					rep = (NSBitmapImageRep *)r;
					break;
				}
			}
			Check(rep != nil, "and it carries a bitmap representation");

			if (rep) {
				Check([rep pixelsWide] == 4 && [rep pixelsHigh] == 4,
					  "at the original 4x4, not rescaled");
				Check([rep hasAlpha], "with alpha, NOT flattened");

				NSColor *opaque = [rep colorAtX:0 y:0];
				NSColor *clear  = [rep colorAtX:3 y:0];

				Check(opaque && [opaque alphaComponent] > 0.9,
					  "the opaque half is still opaque");
				//	This is the assertion the Windows CF_DIB fallback CANNOT
				//	satisfy: there, a transparent pixel comes back as whatever
				//	was behind it, usually black.
				Check(clear && [clear alphaComponent] < 0.1,
					  "the transparent half is still transparent");
			}
		}

		printf("\nrefusals\n");
		Check(PieFX_PngFileToClipboard("/nope/does/not/exist.png", err, sizeof(err)) == 0,
			  "a missing file fails");
		Check(err[0] != 0, "and says why, in a sentence fit for a toast");
		printf("         \"%s\"\n", err);

		Check(PieFX_PngFileToClipboard(NULL, err, sizeof(err)) == 0, "null is refused");

		printf("\n%d passed, %d failed\n", S_pass, S_fail);
		return S_fail ? 1 : 0;
	}
}
