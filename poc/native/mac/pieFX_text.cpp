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
	CFStringRef	str;
	Boolean		ok;

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

	//	Already UTF-8 — which is what an English install gives, since ASCII is
	//	a subset. Copied straight through rather than round-tripped.
	if (IsUtf8(in)) {
		if (strlen(in) >= cap) {
			return 0;
		}
		memcpy(out, in, strlen(in) + 1);
		return 1;
	}

	str = CFStringCreateWithCString(kCFAllocatorDefault, in,
									CFStringGetSystemEncoding());
	if (!str) {
		return 0;
	}
	ok = CFStringGetCString(str, out, (CFIndex)cap, kCFStringEncodingUTF8);
	CFRelease(str);

	if (!ok) {
		out[0] = 0;
		return 0;
	}
	return 1;
}
