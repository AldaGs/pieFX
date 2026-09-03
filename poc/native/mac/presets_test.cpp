//	pieFX — the preset walk, against the REAL After Effects install, with no AE
//	running.
//
//	`WritePresets` touches no AEGP suite: it is a directory walk and some JSON.
//	That makes it testable outside After Effects, and it should be — the walk
//	has a depth cap, a count cap, a category rule and a two-root search in it,
//	and the last time this project shipped an untested feature the hand-check
//	found a bug in it within the hour.
//
//	It is `static`, so this harness includes the translation unit rather than
//	linking against it. That is deliberate and it is the point: what runs here
//	is the SHIPPING function, not a copy of it that could drift.
//
//	  ./poc/native/mac/build_presets_test.sh
//	  poc/overlay/src-tauri/target/release/pieFX_presets_test
#include "../pieFX.cpp"

#include <sys/stat.h>

static int S_pass = 0;
static int S_fail = 0;

static void
Check(int ok, const char *what)
{
	printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
	ok ? S_pass++ : S_fail++;
}

int
main(int argc, char **argv)
{
	//	The install to walk. Defaults to the one on this machine; a different
	//	AE version can be passed in.
	const char *install = (argc > 1) ? argv[1] : "/Applications/Adobe After Effects 2026";

	printf("pieFX_presets_test\n\n");

	printf("the install's shape — which is what ShippedPresetRoot relies on\n");
	{
		char presets[MAX_PATH];
		char plugins[MAX_PATH];

		snprintf(presets, sizeof(presets), "%s/Presets", install);
		snprintf(plugins, sizeof(plugins), "%s/Plug-ins", install);
		Check(DirExists(presets), "an install has a Presets folder");
		Check(DirExists(plugins), "and a Plug-ins folder beside it");
		printf("         %s\n", install);
	}

	printf("\nthe two platform lookups\n");
	{
		char docs[MAX_PATH] = { 0 };
		char self[MAX_PATH] = { 0 };

		Check(DocumentsDir(docs, sizeof(docs)), "the Documents folder resolves");
		printf("         %s\n", docs);
		Check(DirExists(docs), "and it exists");

		Check(SelfModulePath(self, sizeof(self)), "the module path resolves");
		printf("         %s\n", self);
	}

	//	The walk itself, over the real Presets tree, into a real file.
	printf("\nthe walk, over the real shipped presets\n");
	{
		char	root[MAX_PATH];
		char	out[MAX_PATH];
		A_long	count = 0;
		FILE	*fp;

		snprintf(root, sizeof(root), "%s/Presets", install);
		snprintf(out, sizeof(out), "%s/pieFX_presets_test.json", getenv("TMPDIR"));

		fp = fopen(out, "wb");
		Check(fp != NULL, "a scratch file opened");
		if (!fp) { return 1; }

		fprintf(fp, "{\n  \"presets\": [\n");
		WalkPresetFolder(fp, root, "", "", &count, 0);
		fprintf(fp, "\n  ]\n}\n");
		fclose(fp);

		printf("         found %d, wrote %s\n", (int)count, out);
		Check(count > 0, "presets were found at all");
		//	621 on the author's machine. Asserted as a FLOOR rather than an
		//	exact number, because this walks whatever AE is installed and a
		//	different version ships a different set — an exact figure would
		//	fail for the wrong reason on anyone else's machine.
		Check(count > 500, "and a plausible number of them (AE ships ~620)");
		Check(count < PIEFX_PRESET_MAX, "under the cap, so nothing was truncated");
	}

	printf("\nwhat it wrote\n");
	{
		char	out[MAX_PATH];
		FILE	*fp;
		char	line[4096];
		int		with_cat = 0, bad_path = 0, cat_has_ffx = 0, lines = 0;

		snprintf(out, sizeof(out), "%s/pieFX_presets_test.json", getenv("TMPDIR"));
		fp = fopen(out, "rb");
		Check(fp != NULL, "the file reads back");
		if (!fp) { return 1; }

		while (fgets(line, sizeof(line), fp)) {
			const char *p = strstr(line, "\"path\": \"");
			const char *c = strstr(line, "\"category\": \"");

			if (!p || !c) { continue; }
			lines++;

			//	Every path must be openable. A preset the search offers and
			//	cannot apply is worse than one it never offered.
			{
				char		path[MAX_PATH];
				const char *q = p + strlen("\"path\": \"");
				const char *e = strchr(q, '"');
				struct stat	st;

				if (e && (size_t)(e - q) < sizeof(path)) {
					memcpy(path, q, (size_t)(e - q));
					path[e - q] = 0;
					if (stat(path, &st) != 0) { bad_path++; }
				}
			}
			//	The category is a FOLDER. If the filename leaked into it — which
			//	is exactly what happens when the joined relative path is passed
			//	where the folder belongs — it would end in .ffx.
			{
				const char *q = c + strlen("\"category\": \"");
				const char *e = strchr(q, '"');

				if (e && e > q) {
					with_cat++;
					if (e - q > 4 && 0 == strncasecmp(e - 4, ".ffx", 4)) { cat_has_ffx++; }
				}
			}
		}
		fclose(fp);

		printf("         %d entries, %d with a category\n", lines, with_cat);
		Check(lines > 500, "every preset produced a line");
		Check(bad_path == 0, "every path exists on disk");
		Check(with_cat > 0, "categories were assigned");
		Check(cat_has_ffx == 0, "and no category is a FILENAME rather than a folder");
	}

	printf("\n%d passed, %d failed\n", S_pass, S_fail);
	return S_fail ? 1 : 0;
}
