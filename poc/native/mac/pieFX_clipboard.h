//	pieFX — a rendered frame onto the clipboard, on macOS.
//
//	This is the one place in the port where the code gets SMALLER, and the
//	reason is worth recording. On Windows the same job needs three clipboard
//	formats: "PNG" (the file bytes, which modern apps prefer), CF_DIBV5 (32bpp
//	with a real alpha channel), and CF_DIB (the same pixels forced opaque, as
//	the universal fallback) — because CF_DIB cannot express alpha and something
//	has to be there for consumers that know nothing else. Producing the two DIBs
//	means decoding the PNG with WIC and building bitmap headers by hand.
//
//	NSPasteboard needs none of it. It takes the PNG bytes as they are under
//	NSPasteboardTypePNG, and AppKit renders that for whatever asks — including
//	as TIFF, with alpha intact, for consumers that want a bitmap. So the WIC
//	decode, the DIB construction and the force-opaque fallback all disappear:
//	roughly 230 lines become roughly 40.
//
//	The half that is already portable stays where it is. The frame still
//	arrives as a PNG on disk, written by ExtendScript's `saveFrameToPng`,
//	because that is the only way AE will render a frame for us and this side is
//	the only one that can touch a clipboard.
#ifndef PIEFX_CLIPBOARD_H
#define PIEFX_CLIPBOARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//	The PNG at `pathZ` onto the general pasteboard. Returns 1 on success; on
//	failure `errZ` carries a sentence the caller can put in front of a user,
//	because every one of these failures is reported as a toast rather than
//	swallowed — a copy-frame that quietly does nothing is indistinguishable
//	from one that worked until the user tries to paste.
int PieFX_PngFileToClipboard(const char *pathZ, char *errZ, size_t err_max);

#ifdef __cplusplus
}
#endif

#endif	// PIEFX_CLIPBOARD_H
