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
#include <string.h>

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

		//	The bug this harness did not catch the first time, and could not
		//	have: it only ever handed the clipboard a COMPLETE file.
		//
		//	A 6656x2270 frame pasted as 6656x804, and nothing reported an
		//	error, because a truncated PNG does not fail to decode — its IHDR
		//	is in the first 33 bytes and the pixel rows are not, so it decodes
		//	at the full advertised size with only the rows that arrived. The
		//	two assertions below are the ones that would have caught it: the
		//	dimensions AGREE with a good file while the pixels do not.
		printf("\na truncated PNG is not a failed PNG — which is the whole problem\n");
		//	A REAL frame size, not the 4x4 above. That matters: half of a tiny
		//	PNG loses the whole IDAT and fails honestly, which is why a small
		//	test image would have reported that all was well. The bug needs an
		//	image big enough for a fraction of its rows to be worth decoding —
		//	which every actual comp is.
		{
			const NSInteger	kW = 1600, kH = 1200;
			NSBitmapImageRep *big =
				[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
													   pixelsWide:kW
													   pixelsHigh:kH
													bitsPerSample:8
												  samplesPerPixel:4
														 hasAlpha:YES
														 isPlanar:NO
												   colorSpaceName:NSDeviceRGBColorSpace
													  bytesPerRow:0
													 bitsPerPixel:0];
			//	Noise, so it does not compress down to nothing and leave the
			//	"half the file" slice meaninglessly large.
			unsigned char  *d	= [big bitmapData];
			NSInteger		bpr	= [big bytesPerRow];

			for (NSInteger y = 0; y < kH; y++) {
				for (NSInteger x = 0; x < kW; x++) {
					unsigned char *px = d + y * bpr + x * 4;

					px[0] = (unsigned char)(x * 7 + y * 13);
					px[1] = (unsigned char)(x * 3);
					px[2] = (unsigned char)(y * 5);
					px[3] = 255;
				}
			}

			NSData *whole = [big representationUsingType:NSBitmapImageFileTypePNG
											  properties:@{}];
			NSData *cut   = [whole subdataWithRange:NSMakeRange(0, [whole length] / 2)];

			NSBitmapImageRep *r = [NSBitmapImageRep imageRepWithData:cut];

			Check(r != nil, "a half-written PNG still produces an image rep");
			Check(r && [r pixelsWide] == kW && [r pixelsHigh] == kH,
				  "at the FULL advertised size — so size proves nothing");
			printf("         %ld bytes whole, %ld truncated, still reports %ldx%ld\n",
				   (long)[whole length], (long)[cut length],
				   r ? (long)[r pixelsWide] : 0, r ? (long)[r pixelsHigh] : 0);

			//	Hence the check the plug-in now makes instead. IEND is a fixed
			//	12 bytes and is the last thing in a PNG, so "is it finished?"
			//	has an exact answer that needs no timing.
			static const unsigned char k_iend[12] =
				{ 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82 };

			Check([whole length] >= 12 &&
				  0 == memcmp((const unsigned char *)[whole bytes] + [whole length] - 12,
							  k_iend, 12),
				  "a complete PNG ends in IEND");
			Check(!([cut length] >= 12 &&
					0 == memcmp((const unsigned char *)[cut bytes] + [cut length] - 12,
								k_iend, 12)),
				  "a truncated one does not — which is what WaitForFrameFile now asks");

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
