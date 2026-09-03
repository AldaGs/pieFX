//	pieFX — the config path and the text conversion, checked without AE.
//
//	Two things are worth a harness here and the rest is not. The PATH has a
//	directory-creation side effect and a two-process agreement to keep, and the
//	agreement is exactly what no other harness can catch: every driver in this
//	project passes --settings/--effects explicitly, so a wrong DEFAULT is
//	invisible to all of them. The TEXT conversion has a decision in it — try
//	UTF-8 first, because the fallback encoding cannot fail and therefore cannot
//	be checked — and that decision is worth pinning down.
//
//	  ./poc/native/mac/build_paths_test.sh && .../pieFX_paths_test
#include "pieFX_paths.h"
#include "pieFX_text.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>

static int S_pass = 0;
static int S_fail = 0;

static void
Check(int ok, const char *what)
{
	printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
	ok ? S_pass++ : S_fail++;
}

static void
CheckStr(const char *got, const char *want, const char *what)
{
	int ok = got && want && strcmp(got, want) == 0;

	printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
	if (!ok) { printf("         got  %s\n         want %s\n", got ? got : "(null)", want); }
	ok ? S_pass++ : S_fail++;
}

//	The relative names, spelled here EXACTLY as the overlay spells them in
//	lib.rs. If the two ever drift, this is where it shows.
#define REL_SETTINGS	"pieFX/settings.json"
#define REL_EFFECTS		"pieFX/effects.json"
#define REL_RECENTS		"pieFX/recents.json"

int
main(void)
{
	char	base[1024];
	char	path[1024];
	char	want[1024];
	char	utf8[1024];

	printf("pieFX_paths_test\n\n");
	printf("  system encoding: %lu  (MacRoman is 0)\n\n",
		   (unsigned long)CFStringGetSystemEncoding());

	printf("the base directory\n");
	Check(PieFX_ConfigBase(base, sizeof(base)), "PieFX_ConfigBase succeeds");
	printf("         %s\n", base);
	snprintf(want, sizeof(want), "%s/Library/Application Support", getenv("HOME"));
	CheckStr(base, want, "it is ~/Library/Application Support");
	Check(base[strlen(base) - 1] != '/', "no trailing separator");

	printf("\nthe three shared files\n");
	Check(PieFX_ConfigPath(REL_SETTINGS, path, sizeof(path)), "settings path built");
	snprintf(want, sizeof(want), "%s/%s", base, REL_SETTINGS);
	CheckStr(path, want, "settings.json is where the overlay looks");

	Check(PieFX_ConfigPath(REL_EFFECTS, path, sizeof(path)), "effects path built");
	snprintf(want, sizeof(want), "%s/%s", base, REL_EFFECTS);
	CheckStr(path, want, "effects.json is where the overlay looks");

	//	recents.json is written by the OVERLAY only — the plug-in never touches
	//	it. It is here so that all three names are pinned in one place.
	snprintf(want, sizeof(want), "%s/%s", base, REL_RECENTS);
	printf("  [--] recents.json (overlay-only): %s\n", want);

	printf("\nthe directory is created, not assumed\n");
	{
		struct stat st;

		snprintf(want, sizeof(want), "%s/pieFX", base);
		Check(stat(want, &st) == 0 && S_ISDIR(st.st_mode),
			  "the pieFX folder exists after PieFX_ConfigPath");
		Check(PieFX_ConfigPath(REL_SETTINGS, path, sizeof(path)),
			  "a second call over an existing folder is not an error");
	}

	printf("\nrefusals\n");
	Check(!PieFX_ConfigPath(NULL, path, sizeof(path)), "a null relative name is refused");
	Check(!PieFX_ConfigPath(REL_SETTINGS, path, 8), "a too-small buffer is refused");
	Check(path[0] == 0, "and the buffer is left empty rather than half-built");

	printf("\ntext: what is already UTF-8 passes through\n");
	Check(PieFX_LegacyToUtf8("Gaussian Blur", utf8, sizeof(utf8)), "ASCII converts");
	CheckStr(utf8, "Gaussian Blur", "unchanged");
	//	"Desenfoque gaussiano" with a real UTF-8 accent, i.e. what a correctly
	//	encoded Spanish name looks like. MacRoman would happily mangle this
	//	into two characters, which is why UTF-8 is tried first.
	Check(PieFX_LegacyToUtf8("Distorsi\xc3\xb3n", utf8, sizeof(utf8)), "UTF-8 converts");
	CheckStr(utf8, "Distorsi\xc3\xb3n", "byte-identical, not round-tripped");
	Check(PieFX_LegacyToUtf8("", utf8, sizeof(utf8)), "empty is a legal conversion");

	printf("\ntext: legacy bytes become UTF-8\n");
	//	0x97 is 'o-acute' in MacRoman — the encoding this machine actually
	//	reports — and a lone 0x97 is not valid UTF-8, so this can only have
	//	taken the legacy path. The expected result is the real Spanish name.
	{
		int ok = PieFX_LegacyToUtf8("Distorsi\x97n", utf8, sizeof(utf8));

		Check(ok, "a legacy byte converts");
		printf("         %s\n", utf8);
		CheckStr(utf8, "Distorsi\xc3\xb3n", "decoded to the right character, in UTF-8");
	}

	printf("\ntext: refusals leave the caller able to fall back\n");
	Check(!PieFX_LegacyToUtf8("Distorsi\xc3\xb3n", utf8, 4), "a too-small buffer is refused");
	Check(utf8[0] == 0, "and the buffer is empty, so a fallback is unambiguous");
	Check(!PieFX_LegacyToUtf8(NULL, utf8, sizeof(utf8)), "null is refused");

	printf("\n%d passed, %d failed\n", S_pass, S_fail);
	return S_fail ? 1 : 0;
}
