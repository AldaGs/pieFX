#include "pieFX_text.h"

#include <string.h>
#include <CoreFoundation/CoreFoundation.h>

//	Is `s` already well-formed UTF-8? Asked FIRST, and that order is the point.
//
//	CFStringGetSystemEncoding() on this machine is MacRoman, and MacRoman has a
//	meaning for every byte value — so decoding UTF-8 bytes as MacRoman does not
//	fail, it SUCCEEDS and produces mojibake. A conversion that cannot fail is a
//	conversion that cannot be checked, so the bytes are tested against the
//	stricter grammar first and only fall through to the lossy guess.
//	Pure ASCII is the fast path AND the safe one: nothing about it can be
//	mis-decoded or mis-composed.
static int
IsAscii(const char *s)
{
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p >= 0x80) { return 0; }
	}
	return 1;
}

static int
IsUtf8(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;

	while (*p) {
		unsigned char c = *p++;
		int			  n;

		if (c < 0x80)						{ continue; }
		else if ((c & 0xE0) == 0xC0)		{ n = 1; if (c < 0xC2) { return 0; } }
		else if ((c & 0xF0) == 0xE0)		{ n = 2; }
		else if ((c & 0xF8) == 0xF0)		{ n = 3; if (c > 0xF4) { return 0; } }
		else								{ return 0; }

		while (n--) {
			if ((*p & 0xC0) != 0x80) { return 0; }
			p++;
		}
	}
	return 1;
}

int
PieFX_LegacyToUtf8(const char *in, char *out, size_t cap)
{
	CFMutableStringRef	str;
	Boolean				ok;

	if (!out || !cap) {
		return 0;
	}
	out[0] = 0;
	if (!in) {
		return 0;
	}
	if (!*in) {
		return 1;					// empty is a legal, faithful conversion
	}

	//	ASCII cannot be anything but itself: it is valid UTF-8, it is identical
	//	in every legacy encoding pieFX will meet, and it has no composed form.
	//	Copied straight through, which is the whole catalogue on an English
	//	install.
	if (IsAscii(in)) {
		if (strlen(in) >= cap) {
			return 0;
		}
		memcpy(out, in, strlen(in) + 1);
		return 1;
	}

	//	Non-ASCII takes the long way round, because two different things can be
	//	wrong with it and only one of them is an encoding problem. See the
	//	normalisation note below.
	str = CFStringCreateMutable(kCFAllocatorDefault, 0);
	if (!str) {
		return 0;
	}
	{
		CFStringRef tmp = CFStringCreateWithCString(
			kCFAllocatorDefault, in,
			IsUtf8(in) ? kCFStringEncodingUTF8 : CFStringGetSystemEncoding());

		if (!tmp) {
			CFRelease(str);
			return 0;
		}
		CFStringAppend(str, tmp);
		CFRelease(tmp);
	}

	//	NFC, and this is not tidiness.
	//
	//	Preset names come from readdir, and APFS is normalisation-PRESERVING:
	//	it stores whatever bytes the installer wrote. Measured on this machine,
	//	AE's own Presets tree is MIXED — 136 of 213 accented names are
	//	decomposed and the rest are not. A keyboard produces composed text, and
	//	the search window matches with a plain lowercased substring test, so
	//	every decomposed name would have been unfindable by typing it. The
	//	preset would be in the list and would not come back from a search for
	//	its own name.
	//
	//	Normalising here rather than in the search means the file itself is
	//	canonical, so anything that reads it later gets the same answer.
	CFStringNormalize(str, kCFStringNormalizationFormC);

	ok = CFStringGetCString(str, out, (CFIndex)cap, kCFStringEncodingUTF8);
	CFRelease(str);

	if (!ok) {
		out[0] = 0;
		return 0;
	}
	return 1;
}
