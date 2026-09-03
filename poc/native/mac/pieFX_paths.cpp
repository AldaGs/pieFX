#include "pieFX_paths.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>
#include <limits.h>

#define PIEFX_APPSUPPORT	"/Library/Application Support"

//	$HOME, falling back to the password database. The fallback is not
//	theoretical: a process inherits whatever environment its launcher gave it,
//	and getpwuid is the account's own answer rather than an inherited one.
static const char *
HomeDir(void)
{
	const char *h = getenv("HOME");

	if (h && *h) {
		return h;
	}
	{
		struct passwd *pw = getpwuid(getuid());

		if (pw && pw->pw_dir && *pw->pw_dir) {
			return pw->pw_dir;
		}
	}
	return NULL;
}

int
PieFX_ConfigBase(char *out, size_t cap)
{
	const char *home = HomeDir();

	if (!out || !cap) {
		return 0;
	}
	out[0] = 0;
	if (!home) {
		return 0;
	}
	if (strlen(home) + sizeof(PIEFX_APPSUPPORT) > cap) {
		return 0;
	}
	snprintf(out, cap, "%s%s", home, PIEFX_APPSUPPORT);
	return 1;
}

//	mkdir -p, over the parent of `path`. Every component is attempted and
//	EEXIST is success, which is the whole point: `Application Support` is
//	always there and `pieFX` usually is not.
static void
MakeParentDirs(const char *path)
{
	char	work[PATH_MAX];
	char	*p;

	if (!path || strlen(path) >= sizeof(work)) {
		return;
	}
	snprintf(work, sizeof(work), "%s", path);

	//	Drop the leaf: only DIRECTORIES are created, never the file itself.
	p = strrchr(work, '/');
	if (!p || p == work) {
		return;
	}
	*p = 0;

	for (p = work + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = 0;
		mkdir(work, 0755);
		*p = '/';
	}
	mkdir(work, 0755);
}

int
PieFX_ConfigPath(const char *rel, char *out, size_t cap)
{
	char base[PATH_MAX];

	if (!out || !cap) {
		return 0;
	}
	out[0] = 0;
	if (!rel || !*rel) {
		return 0;
	}
	if (!PieFX_ConfigBase(base, sizeof(base))) {
		return 0;
	}
	if (strlen(base) + 1 + strlen(rel) + 1 > cap) {
		return 0;
	}
	snprintf(out, cap, "%s/%s", base, rel);

	MakeParentDirs(out);
	return 1;
}
