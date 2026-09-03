//	pieFX — where the three shared files live, on macOS.
//
//	settings.json, effects.json and recents.json are the one part of this
//	project that BOTH processes touch: the overlay's settings window writes the
//	settings, the plug-in reads them; the plug-in writes the catalogue, the
//	overlay's search window reads it. So the location is not a plug-in decision
//	and it is not an overlay decision — it is one decision, taken once, and the
//	matching half of it lives in overlay/src-tauri/src/lib.rs (`piefx_dir`).
//
//	The answer is `~/Library/Application Support/pieFX/`, which is what
//	%APPDATA%\pieFX\ means here: per-user, not per-machine, not backed by
//	iCloud, and not somewhere a user is invited to browse.
//
//	`~/Library/Containers/...` is deliberately NOT used. After Effects is not
//	sandboxed, so it has no container, and pointing at one would put the files
//	somewhere the overlay — a separate, also unsandboxed process — would have
//	to guess at.
//
//	The one thing that must NOT differ between the two sides is the spelling of
//	the leaf files, so both sides build the same path from the same two pieces:
//	a base directory and a relative name. On Windows that name is
//	`pieFX\settings.json`; here it is `pieFX/settings.json`, and PIEFX_PATH_SEP
//	in pieFX.h is what keeps the two spellings in one place.
#ifndef PIEFX_PATHS_H
#define PIEFX_PATHS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//	The per-user configuration base — `~/Library/Application Support` — with no
//	trailing separator. Returns 0 and leaves `out` empty if there is no home
//	directory to speak of, which is a real possibility for a process launched
//	from a context with no environment at all.
int PieFX_ConfigBase(char *out, size_t cap);

//	Base + separator + `rel`, with every directory along the way created.
//	`rel` is a PIEFX_*_REL macro, i.e. it carries the `pieFX` folder as its
//	first component; the folder is created here because the settings window may
//	never have run on this machine, exactly as CreateDirectoryA does on the
//	Windows side.
//
//	Returns 1 on success. A failure to CREATE the directory is not a failure
//	here: the caller is about to fopen the path and will report that in terms
//	of the file it actually wanted, which is a better message than "mkdir".
int PieFX_ConfigPath(const char *rel, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif	// PIEFX_PATHS_H
