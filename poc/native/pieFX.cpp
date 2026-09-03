/*
	pieFX.cpp - POC: the Anchor wheel (Windows).

	See pieFX.h for the shape. The flow, all on AE's UI thread except the pipe
	accept:

	  right-button DOWN ....... swallowed (AE opens its menu on DOWN; S2D)
	  held past 200ms ......... summon: query selection, tell the overlay to
	                            show the wheel at the cursor
	  mouse moves ............. stream the raw cursor position to the overlay
	  right-button UP ......... if it was a hold: swallow the UP too and send
	                            release. The overlay decides what was chosen and
	                            sends back an action, which IdleHook executes -
	                            never from inside the mouse hook.
	                            if it was a short click: replay it so AE's normal
	                            context menu still appears

	The native side deliberately has NO opinion about which slot is under the
	cursor. The overlay draws the wheel, so it owns the geometry and the
	hit-testing; deciding here as well would fire every action twice.

	Modelled on Persisto (five-param entry point) and on the frozen Phase-0
	spike (../../pieFX.cpp), whose gesture engine this reuses almost verbatim.
*/

#include "pieFX.h"

#ifndef AE_OS_WIN
	//	The regions that genuinely ARE Windows, in their macOS form. Each is
	//	proven on its own OUTSIDE After Effects by the harnesses in
	//	poc/native/mac, which is why they can be relied on here before this
	//	file has ever been loaded by AE at all.
	#include "mac/pieFX_fifo.h"
	#include "mac/pieFX_launch.h"
	#include "mac/pieFX_gesture.h"
#endif

// ---------------------------------------------------------------------------
//	globals
// ---------------------------------------------------------------------------
static AEGP_Command		S_toggle_cmd	= 0L;
static AEGP_Command		S_selftest_cmd	= 0L;
static AEGP_Command		S_cmdprobe_cmd	= 0L;
static AEGP_Command		S_settings_cmd	= 0L;
static AEGP_PluginID	S_my_id			= 0L;
static SPBasicSuite		*sP				= NULL;

static A_Boolean		S_active		= FALSE;	// POC armed?
static char				S_log_path[MAX_PATH] = { 0 };

//	Read from settings.json at the first idle. `armOnLaunch` ships ON, so an
//	install is "restart AE and flick" rather than "remember to arm it first";
//	Window > pieFX remains the manual toggle and the way out.
static A_Boolean		S_arm_on_launch	= TRUE;
static UINT				S_hold_ms		= PIEFX_HOLD_MS;
static A_Boolean		S_settings_read	= FALSE;
static A_Boolean		S_launch_armed	= FALSE;	// the auto-arm has had its one go
static A_Boolean		S_effects_dumped = FALSE;	// the catalogue walk has had its one go

//	Defined with the rest of the lifecycle, below; the idle hook needs them.
static void			ResolveLogPath(void);
static void			ReadSettings(void);
static A_Boolean	Arm(AEGP_SuiteHandler &suites, A_Boolean announce);

//	gesture state (from the S2D spike)
//
//	The HOOK and the TIMER are Windows objects and the state machine that uses
//	them lives in MouseProc below. On macOS both, and the state machine, belong
//	to poc/native/mac/pieFX_gesture.mm — a local NSEvent monitor and a
//	dispatch_after — so none of this is declared there.
#ifdef AE_OS_WIN
static HHOOK			S_mouse_hook	= NULL;
static UINT_PTR			S_hold_timer	= 0;
static DWORD			S_rdown_tick	= 0;
static POINT			S_rdown_pt		= { 0, 0 };
static A_Boolean		S_rdownB		= FALSE;
static A_Boolean		S_hold_firedB	= FALSE;
static int				S_replay_pending = 0;
#endif

//	summon session. No cell here: the overlay owns the wheel geometry and does
//	its own hit-testing, so the native side only reports where the cursor is.
static LONG				S_summon_cx		= 0;
static LONG				S_summon_cy		= 0;
#ifdef AE_OS_WIN
static POINT			S_last_sent		= { 0, 0 };
#endif

//	selection context, refreshed in IdleHook so the summon (which runs inside the
//	mouse hook, where AEGP calls would be reentrant) can read a cached value.
static A_Boolean		S_has_selection	= FALSE;
static A_Boolean		S_has_comp		= FALSE;
static A_long			S_layer_count	= 0;

// ---------------------------------------------------------------------------
//	actions coming back from the overlay
//
//	The overlay owns the slot tree, so it sends a finished action rather than a
//	slot index; the native side is a dumb executor with a switch on kind. See
//	SETTINGS.md for the wire format and why free text is base64.
// ---------------------------------------------------------------------------
enum PieKind {
	PK_NONE = 0,
	PK_AE_CMD,		//	AEGP_DoCommand(id)
	PK_SNIPPET,		//	ExtendScript source
	PK_FILE,		//	path to a .jsx, run through $.evalFile
	PK_EFFECT,		//	install-by-match-name (S5)
	PK_ANCHOR,		//	builtin: the 3x3 anchor grid
	PK_COPY_FRAME,	//	builtin: current frame -> the clipboard
	PK_PRESET		//	animation preset (.ffx) applied to the selected layer
};

struct PieAction {
	PieKind	kind;
	long	id;
	int		cell;
	char	text[PIEFX_TEXT_MAX];
};

//	Ring buffer. Actions are user-initiated one at a time, so this only ever
//	holds one - the depth is there so a burst can never overwrite a pending one.
static PieAction		S_queue[PIEFX_QUEUE_LEN];
static int				S_q_head		= 0;
static int				S_q_tail		= 0;

//	pipe (native = server). Writes happen on the UI thread; a background thread
//	owns accept/re-accept. The critical section guards the handle + flags.
//	Resolved at arm time. The base names are used when free, so the plain dev
//	flow (running the overlay by hand) keeps working; a SECOND AE instance finds
//	them taken and falls back to a pid suffix instead of silently having no
//	overlay at all. The launched overlay is told which names to use.
static char				S_tx_name[128]	= { 0 };
static char				S_rx_name[128]	= { 0 };

//	The pipe HANDLES, the job object and the critical section are all Windows
//	objects with no macOS counterpart to declare here: the FIFO server owns its
//	own descriptors (poc/native/mac/pieFX_fifo.cpp) and the launcher owns the
//	child pid and its process group (pieFX_launch.cpp). What survives on both
//	sides is the NAMES and the connected flag, which the portable code reads.
#ifdef AE_OS_WIN
static HANDLE			S_overlay_proc	= NULL;
//	Kept open for the life of the process ON PURPOSE. The job is configured to
//	kill everything in it when its last handle closes, and process exit closes
//	it - so the overlay cannot outlive AE even if AE never runs a death hook.
static HANDLE			S_overlay_job	= NULL;

static HANDLE			S_pipe			= INVALID_HANDLE_VALUE;	// TX: UI thread writes
static HANDLE			S_pipe_rx		= INVALID_HANDLE_VALUE;	// RX: pipe thread reads
static HANDLE			S_pipe_thread	= NULL;
static HANDLE			S_pipe_dead_evt	= NULL;	// UI -> bg: this connection broke
static HANDLE			S_pipe_stop_evt	= NULL;	// UI -> bg: shut down
static A_Boolean		S_pipe_connected = FALSE;
#endif	// AE_OS_WIN

//	Outside the guard: this also serialises the ACTION QUEUE, which is written
//	on the transport thread and drained on AE's UI thread on both platforms.
static CRITICAL_SECTION	S_pipe_cs;
static A_Boolean		S_pipe_cs_ready	= FALSE;

// ---------------------------------------------------------------------------
//	logging
// ---------------------------------------------------------------------------
static void
Log(const char *fmtZ, ...)
{
	FILE *fp = NULL;
	if (!S_log_path[0] || fopen_s(&fp, S_log_path, "a") || !fp) {
		return;
	}
	va_list args;
	va_start(args, fmtZ);
	vfprintf(fp, fmtZ, args);
	va_end(args);
	fclose(fp);
}

//	Append that TRUNCATES instead of throwing the buffer away. strcat_s on
//	overflow empties the destination, so one line too many silently discarded
//	the whole report - which is one of the two ways the probe's output went
//	missing. The other was the dialog itself: AEGP_ReportInfo will not show a
//	long message, and there is no way to ask it how much it kept.
static void
Append(char *dstZ, size_t max, const char *srcZ)
{
	strncat_s(dstZ, max, srcZ, _TRUNCATE);
}

//	Put a long report where it can actually be read: a .txt beside the log,
//	opened in whatever handles text on this machine. The dialog then carries
//	only the path, which always fits.
static void
WriteReport(AEGP_SuiteHandler &suites, const char *fileZ, const char *bodyZ, const char *headZ)
{
	char path[MAX_PATH] = { 0 };
	DWORD len = GetTempPathA(MAX_PATH, path);
	if (!len || len > MAX_PATH - 32) {
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, bodyZ);
		return;
	}
	strcat_s(path, MAX_PATH, fileZ);

	FILE *fp = NULL;
	if (fopen_s(&fp, path, "w") || !fp) {
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, bodyZ);
		return;
	}
	fputs(bodyZ, fp);
	fclose(fp);

	ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);

	char msg[MAX_PATH + 256] = { 0 };
	Append(msg, sizeof(msg), headZ);
	Append(msg, sizeof(msg), "\n\nThe full report is in:\n");
	Append(msg, sizeof(msg), path);
	Append(msg, sizeof(msg), "\n\n(opened for you; it is rewritten each run)");
	suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
}

// ---------------------------------------------------------------------------
//	the generalised anchor script (S1, from centre to any grid fraction)
//
//	fx, fy in {0, 0.5, 1} pick the anchor's position on the layer's source rect.
//	The position compensation - the delta pushed through Scale and Z-Rotation so
//	the pixels stay put - is the S1 spike's; fx/fy are what got parameterised.
//
//	ANIMATION. The spike only ever called setValue, which is wrong twice over on
//	an animated layer: it throws on a property that has keyframes, and even where
//	it did not, one static offset is the wrong answer whenever Scale or Rotation
//	are themselves animated, because the layer-space delta maps to a DIFFERENT
//	parent-space offset at every time. So the compensation is sampled per
//	keyframe: each Position key is rewritten at its own time with the delta
//	computed from Scale and Rotation as they are at that time. The keys do not
//	move in time, and their interpolation, temporal eases, spatial tangents and
//	roving are captured and put back, because setValueAtTime does not preserve
//	them. Separated dimensions are handled through their followers, and an
//	animated Anchor Point is shifted wholesale by the same layer-space delta so
//	its own animation survives.
//
//	Match names, not English property names: 'Anchor Point' does not exist on a
//	localised After Effects.
// ---------------------------------------------------------------------------
static const char *S_anchor_script_fmt =
	"(function(){"
	"  var c = app.project.activeItem;"
	"  if (!(c instanceof CompItem)) { return 'no active comp'; }"
	"  var sel = c.selectedLayers;"
	"  if (sel.length === 0) { return 'no layer selected'; }"
	"  var T = c.time;"
	"  function snap(p, k) {"
	"    return { ii: p.keyInInterpolationType(k), oi: p.keyOutInterpolationType(k),"
	"             ie: p.keyInTemporalEase(k), oe: p.keyOutTemporalEase(k),"
	"             is: p.isSpatial ? p.keyInSpatialTangent(k) : null,"
	"             os: p.isSpatial ? p.keyOutSpatialTangent(k) : null,"
	"             rv: p.isSpatial ? p.keyRoving(k) : false };"
	"  }"
	"  function restore(p, k, s) {"
	"    if (p.isSpatial) {"
	"      p.setSpatialTangentsAtKey(k, s.is, s.os);"
	"      try { p.setRovingAtKey(k, s.rv); } catch (e) {}"
	"    }"
	"    p.setInterpolationTypeAtKey(k, s.ii, s.oi);"
	"    if (s.ii === KeyframeInterpolationType.BEZIER || s.oi === KeyframeInterpolationType.BEZIER) {"
	"      try { p.setTemporalEaseAtKey(k, s.ie, s.oe); } catch (e) {}"
	"    }"
	"  }"
	"  function add(v, d) {"
	"    if (v instanceof Array) {"
	"      var o = [];"
	"      for (var i = 0; i < v.length; i++) { o[i] = v[i] + (d[i] || 0); }"
	"      return o;"
	"    }"
	"    return v + (d[0] || 0);"
	"  }"
	"  function shift(p, dfn) {"
	"    if (p.numKeys > 0) {"
	"      var n = p.numKeys, ts = [], sn = [], k;"
	"      for (k = 1; k <= n; k++) { ts.push(p.keyTime(k)); sn.push(snap(p, k)); }"
	"      for (k = 0; k < n; k++) { p.setValueAtTime(ts[k], add(p.valueAtTime(ts[k], false), dfn(ts[k]))); }"
	"      for (k = 1; k <= n; k++) { restore(p, k, sn[k - 1]); }"
	"      return n;"
	"    }"
	"    p.setValue(add(p.value, dfn(T)));"
	"    return 0;"
	"  }"
	"  app.beginUndoGroup('pieFX: Anchor');"
	"  var moved = 0, animated = 0, skipped = 0;"
	"  for (var i = 0; i < sel.length; i++) {"
	"    var L = sel[i];"
	"    try {"
	"      var tg = L.property('ADBE Transform Group');"
	"      var ap = tg.property('ADBE Anchor Point');"
	"      var pos = tg.property('ADBE Position');"
	"      var scl = tg.property('ADBE Scale');"
	"      var rot = tg.property('ADBE Rotate Z');"
	"      var r = L.sourceRectAtTime(T, false);"
	"      var oldA = ap.valueAtTime(T, false);"
	"      var dx = r.left + r.width * (%f) - oldA[0];"
	"      var dy = r.top + r.height * (%f) - oldA[1];"
	"      var dfn = (function (dx, dy, scl, rot) { return function (t) {"
	"        var s = scl.valueAtTime(t, false);"
	"        var th = rot.valueAtTime(t, false) * Math.PI / 180;"
	"        var sx = dx * s[0] / 100, sy = dy * s[1] / 100;"
	"        return [sx * Math.cos(th) - sy * Math.sin(th), sx * Math.sin(th) + sy * Math.cos(th), 0];"
	"      }; })(dx, dy, scl, rot);"
	"      var keys = 0;"
	"      if (pos.dimensionsSeparated) {"
	"        keys += shift(pos.getSeparationFollower(0), function (t) { return [dfn(t)[0]]; });"
	"        keys += shift(pos.getSeparationFollower(1), function (t) { return [dfn(t)[1]]; });"
	"      } else {"
	"        keys += shift(pos, dfn);"
	"      }"
	"      shift(ap, (function (dx, dy) { return function () { return [dx, dy, 0]; }; })(dx, dy));"
	"      if (keys > 0) { animated++; }"
	"      moved++;"
	"    } catch (e) { skipped++; }"
	"  }"
	"  app.endUndoGroup();"
	"  return 'moved ' + moved + ' layer(s)'"
	"       + (animated ? ', ' + animated + ' animated' : '')"
	"       + (skipped ? ', ' + skipped + ' skipped' : '');"
	"})()";

#ifdef AE_OS_WIN
// ---------------------------------------------------------------------------
//	pipe: write side (UI thread)
// ---------------------------------------------------------------------------
static void
PipeWrite(const char *jsonZ)
{
	if (!S_pipe_cs_ready) {
		return;
	}
	EnterCriticalSection(&S_pipe_cs);
	if (S_pipe_connected && S_pipe != INVALID_HANDLE_VALUE) {
		//	OVERLAPPED, and waited on with a deadline, because this runs on AE's
		//	UI thread. A synchronous WriteFile to an overlay that has stopped
		//	reading never returns, and AE is frozen for exactly as long as that
		//	lasts - which was until someone killed pieFX-overlay.exe by hand.
		OVERLAPPED	ov;
		DWORD		written	= 0;
		size_t		len		= strlen(jsonZ);
		BOOL		ok		= FALSE;

		ZeroMemory(&ov, sizeof(ov));
		ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

		if (ov.hEvent) {
			if (WriteFile(S_pipe, jsonZ, (DWORD)len, &written, &ov)) {
				ok = TRUE;
			} else if (GetLastError() == ERROR_IO_PENDING) {
				if (WaitForSingleObject(ov.hEvent, PIEFX_PIPE_WRITE_MS) == WAIT_OBJECT_0) {
					ok = GetOverlappedResult(S_pipe, &ov, &written, FALSE);
				} else {
					//	Cancel, then wait for the cancel to land: the buffer is
					//	on this stack, and the kernel has to be done with it
					//	before we leave the frame.
					CancelIoEx(S_pipe, &ov);
					GetOverlappedResult(S_pipe, &ov, &written, TRUE);
					Log("  pipe: write timed out after %ums, treating the overlay as gone\n",
						(unsigned)PIEFX_PIPE_WRITE_MS);
				}
			}
			CloseHandle(ov.hEvent);
		}

		if (!ok || written != len) {
			//	Client went away. Let the bg thread recycle the instance.
			S_pipe_connected = FALSE;
			if (S_pipe_dead_evt) {
				SetEvent(S_pipe_dead_evt);
			}
		}
	}
	LeaveCriticalSection(&S_pipe_cs);
}

#else	// AE_OS_WIN

//	macOS: the same guarantee, less machinery. The fd is already O_NONBLOCK, so
//	a full pipe returns EAGAIN instead of parking and poll() supplies the
//	deadline — see PieFX_PipeWrite. Bounded because this runs on AE's UI
//	thread, which is the fault b9a73eb fixed on the Windows side.
static void
PipeWrite(const char *jsonZ)
{
	PieFX_PipeWrite(jsonZ);
}

#endif	// AE_OS_WIN


static void SendSummon(LONG x, LONG y, A_Boolean hasSel, A_Boolean hasComp, A_long layers)
{
	char m[224];
	sprintf_s(m, sizeof(m),
		"{\"type\":\"summon\",\"x\":%ld,\"y\":%ld,\"hasSelection\":%s,\"hasComp\":%s,\"layerCount\":%ld}\n",
		//	(long) casts: A_long is 32-bit and %ld is 64-bit on arm64. See
		//	MAC_RESULTS.md — this is Phase 0's third bug, in new code.
		(long)x, (long)y, hasSel ? "true" : "false",
		hasComp ? "true" : "false", (long)layers);
	PipeWrite(m);
}
//	Raw position only. The overlay owns the wheel geometry and hit-tests for
//	itself, so there is no cell to report.
static void SendCursor(LONG x, LONG y)
{
	char m[128];
	sprintf_s(m, sizeof(m), "{\"type\":\"cursor\",\"x\":%ld,\"y\":%ld}\n", x, y);
	PipeWrite(m);
}
static void SendRelease(void) { PipeWrite("{\"type\":\"release\"}\n"); }
static void SendCancel(void)  { PipeWrite("{\"type\":\"cancel\"}\n"); }
//	Ask the overlay to quit ITSELF, while the pipes are still healthy. A
//	process terminated with a read pending on a pipe whose server is already
//	half torn down can sit there un-dead, which is what "TerminateProcess
//	succeeded and the process was still in Task Manager" means. Letting it
//	unwind its own read is the only exit that is actually clean.
static void SendQuit(void)    { PipeWrite("{\"type\":\"quit\"}\n"); }

//	Window > pieFX Settings. The overlay opens a second, ordinary window in
//	its OWN process - the one process that already owns the slot tree and the
//	settings file, so nothing has to be kept in sync and no two processes race
//	on the file. The plug-in only asks.
static void SendSettings(void) { PipeWrite("{\"type\":\"settings\"}\n"); }

//	Errors have to reach the user, but AEGP_ReportInfo is MODAL - throwing a
//	dialog at someone mid-gesture is worse than the failure it reports. The
//	overlay is already on screen and already non-modal, so it draws a toast.
//
//	Quotes and control characters are STRIPPED rather than escaped. These strings
//	are ours and short, and a hand-rolled escaper is precisely the thing that has
//	bitten this project before.
static void SendToast(const char *levelZ, const char *textZ)
{
	char	safe[256];
	size_t	o = 0;

	for (const char *p = textZ; *p && o + 1 < sizeof(safe); p++) {
		char c = *p;

		if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
			c = ' ';
		}
		safe[o++] = c;
	}
	safe[o] = 0;

	char m[400];
	sprintf_s(m, sizeof(m),
		"{\"type\":\"toast\",\"level\":\"%s\",\"text\":\"%s\"}\n", levelZ, safe);
	PipeWrite(m);
}

// ---------------------------------------------------------------------------
//	minimal JSON scanning + base64
//
//	Deliberately not a JSON parser. The producer is our own overlay and the wire
//	format is fixed: keys are known, values are either numbers or tokens from a
//	closed set, and anything free-form is base64. So a scanner that reads to the
//	next quote is sufficient AND cannot be surprised by user input.
// ---------------------------------------------------------------------------
static A_Boolean
JsonStr(const char *srcZ, const char *keyZ, char *outZ, size_t out_max)
{
	const char *p = strstr(srcZ, keyZ);

	if (!p) {
		return FALSE;
	}
	p += strlen(keyZ);

	while (*p == ' ') {
		p++;
	}
	if (*p != '"') {
		return FALSE;
	}
	p++;

	size_t i = 0;
	while (*p && *p != '"' && i + 1 < out_max) {
		outZ[i++] = *p++;
	}
	outZ[i] = 0;
	return TRUE;
}

static A_Boolean
JsonNum(const char *srcZ, const char *keyZ, long *outP)
{
	const char *p = strstr(srcZ, keyZ);

	if (!p) {
		return FALSE;
	}
	p += strlen(keyZ);

	while (*p == ' ') {
		p++;
	}
	*outP = strtol(p, NULL, 10);
	return TRUE;
}

static int
B64Val(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

//	Returns FALSE when the payload did not fit. Silent truncation would hand
//	ExtendScript half a script, which fails as a syntax error a long way from
//	its cause - and the script bootstrap made long payloads ordinary.
static A_Boolean
B64Decode(const char *inZ, char *outZ, size_t out_max)
{
	size_t	o		= 0;
	int		acc		= 0,
			bits	= 0;

	for (const char *p = inZ; *p; p++) {
		int v = B64Val(*p);

		if (v < 0) {
			continue;			//	'=' and any stray whitespace
		}
		acc = (acc << 6) | v;
		bits += 6;

		if (bits >= 8) {
			bits -= 8;
			if (o + 1 < out_max) {
				outZ[o++] = (char)((acc >> bits) & 0xFF);
			}
		}
	}
	outZ[o] = 0;
	return (o + 1 < out_max) ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------
//	action queue (written on the pipe thread, drained on AE's UI thread)
// ---------------------------------------------------------------------------
static void
QueuePush(const PieAction *aP)
{
	if (!S_pipe_cs_ready) {
		return;
	}
	EnterCriticalSection(&S_pipe_cs);

	int next = (S_q_tail + 1) % PIEFX_QUEUE_LEN;

	if (next != S_q_head) {			//	drop rather than overwrite a pending one
		S_queue[S_q_tail] = *aP;
		S_q_tail = next;
	}
	LeaveCriticalSection(&S_pipe_cs);
}

static A_Boolean
QueuePop(PieAction *outP)
{
	A_Boolean got = FALSE;

	if (!S_pipe_cs_ready) {
		return FALSE;
	}
	EnterCriticalSection(&S_pipe_cs);

	if (S_q_head != S_q_tail) {
		*outP = S_queue[S_q_head];
		S_q_head = (S_q_head + 1) % PIEFX_QUEUE_LEN;
		got = TRUE;
	}
	LeaveCriticalSection(&S_pipe_cs);
	return got;
}

//	One newline-delimited message from the overlay.
static void
HandleOverlayLine(const char *lineZ)
{
	char type[32] = { 0 };

	if (!JsonStr(lineZ, "\"type\":", type, sizeof(type)) || strcmp(type, "fire")) {
		return;
	}

	char kind[40] = { 0 };

	if (!JsonStr(lineZ, "\"kind\":", kind, sizeof(kind))) {
		return;
	}

	PieAction a;
	ZeroMemory(&a, sizeof(a));
	a.cell = -1;

	if (!strcmp(kind, "ae-command")) {
		//	A name is preferred and an id is the fallback, so a binding can
		//	be validated against the running AE rather than trusted. The name
		//	arrives base64 for the usual reason - it contains spaces, dots and
		//	apostrophes, and hand-rolled escaping is what keeps biting here.
		char b64[PIEFX_B64_MAX] = { 0 };

		if (JsonStr(lineZ, "\"b64\":", b64, sizeof(b64))) {
			if (!B64Decode(b64, a.text, sizeof(a.text))) {
				SendToast("error", "pieFX: menu name too long");
				return;
			}
		}
		JsonNum(lineZ, "\"id\":", &a.id);

		if (!a.text[0] && !a.id) {
			return;		// neither a name nor an id: nothing to run
		}
		a.kind = PK_AE_CMD;
	} else if (!strcmp(kind, "script-snippet") ||
			   !strcmp(kind, "script-file") ||
			   !strcmp(kind, "preset") ||
			   !strcmp(kind, "effect")) {
		char b64[PIEFX_B64_MAX] = { 0 };

		if (!JsonStr(lineZ, "\"b64\":", b64, sizeof(b64))) {
			return;
		}
		if (!B64Decode(b64, a.text, sizeof(a.text))) {
			SendToast("error", "pieFX: script too long for one action");
			return;
		}

		a.kind = !strcmp(kind, "script-snippet") ? PK_SNIPPET
			   : !strcmp(kind, "script-file")	? PK_FILE
			   : !strcmp(kind, "preset")		? PK_PRESET
												: PK_EFFECT;
	} else if (!strcmp(kind, "builtin")) {
		char name[40] = { 0 };

		if (!JsonStr(lineZ, "\"name\":", name, sizeof(name))) {
			return;
		}
		if (!strcmp(name, "copy-frame")) {
			a.kind = PK_COPY_FRAME;
		} else if (!strcmp(name, "anchor-grid")) {
			long cell = -1;
			JsonNum(lineZ, "\"cell\":", &cell);
			a.kind = PK_ANCHOR;
			a.cell = (int)cell;
		} else {
			//	Silence is the worst answer: the wheel offers the slot, the
			//	user flicks to it, and nothing whatsoever happens.
			char t[160];
			sprintf_s(t, sizeof(t), "\"%s\" is not built yet", name);
			Log("  overlay: builtin \"%s\" not implemented yet\n", name);
			SendToast("info", t);
			return;
		}
	} else {
		return;
	}

	Log("  overlay -> fire kind=%s id=%ld cell=%d %s%s\n",
		kind, a.id, a.cell, a.text[0] ? "text=" : "", a.text);
	QueuePush(&a);
}

#ifndef AE_OS_WIN
// ---------------------------------------------------------------------------
//	bridges into the macOS modules
// ---------------------------------------------------------------------------
//	The modules are deliberately free of AEGP and of this file's globals, so
//	they can be built and proven standalone, outside After Effects. These two
//	functions are the entire coupling between them and the plug-in.
static void
PieFXLogBridge(const char *msg, void *)
{
	Log("%s", msg);
}

static void
PieFXOverlayLineBridge(const char *line, void *)
{
	HandleOverlayLine(line);
}
#endif	// !AE_OS_WIN

#ifdef AE_OS_WIN
// ---------------------------------------------------------------------------
//	pipe: server thread (accept + read + re-accept)
// ---------------------------------------------------------------------------
//	Wait for the overlay on the overlapped TX pipe. The wait watches the stop
//	event too, so a disarm gets this thread moving without depending on the
//	poke-connect in StopPipeServer.
static BOOL
ConnectTxOverlapped(HANDLE tx)
{
	OVERLAPPED	ov;
	BOOL		ok	= FALSE;

	ZeroMemory(&ov, sizeof(ov));
	ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!ov.hEvent) {
		return FALSE;
	}

	if (ConnectNamedPipe(tx, &ov)) {
		ok = TRUE;
	} else {
		DWORD e = GetLastError();

		if (e == ERROR_PIPE_CONNECTED) {
			ok = TRUE;
		} else if (e == ERROR_IO_PENDING) {
			HANDLE	waits[2]	= { ov.hEvent, S_pipe_stop_evt };
			DWORD	n			= 0;
			DWORD	w			= WaitForMultipleObjects(2, waits, FALSE, INFINITE);

			if (w == WAIT_OBJECT_0) {
				ok = GetOverlappedResult(tx, &ov, &n, FALSE);
			} else {
				CancelIoEx(tx, &ov);
				GetOverlappedResult(tx, &ov, &n, TRUE);
			}
		}
	}

	CloseHandle(ov.hEvent);
	return ok;
}

static DWORD WINAPI
PipeServerThread(LPVOID)
{
	for (;;) {
		//	BOTH pipes are created before either is connected, so the overlay can
		//	open them back to back. TX is outbound only and is the handle the UI
		//	thread writes to; RX is inbound only and is the one this thread parks
		//	on. Neither handle ever carries both directions - see PIEFX_PIPE_TX.
		//	FILE_FLAG_OVERLAPPED on TX only. It is the handle the UI thread
		//	writes to, and only an overlapped handle can have a write given a
		//	deadline - see PipeWrite. RX stays synchronous: it is read on this
		//	thread, where blocking is the point.
		HANDLE tx = CreateNamedPipeA(
			S_tx_name,
			PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1, 4096, 4096, 0, NULL);

		HANDLE rx = CreateNamedPipeA(
			S_rx_name,
			PIPE_ACCESS_INBOUND,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1, 4096, 4096, 0, NULL);

		if (tx == INVALID_HANDLE_VALUE || rx == INVALID_HANDLE_VALUE) {
			Log("  pipe: CreateNamedPipe failed, err=%lu\n", (unsigned long)GetLastError());
			if (tx != INVALID_HANDLE_VALUE) CloseHandle(tx);
			if (rx != INVALID_HANDLE_VALUE) CloseHandle(rx);
			return 1;
		}

		BOOL tx_ok = ConnectTxOverlapped(tx);
		BOOL rx_ok = tx_ok && (ConnectNamedPipe(rx, NULL)
			? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED));

		if (WaitForSingleObject(S_pipe_stop_evt, 0) == WAIT_OBJECT_0) {
			CloseHandle(tx);
			CloseHandle(rx);
			return 0;
		}
		if (!tx_ok || !rx_ok) {
			CloseHandle(tx);
			CloseHandle(rx);
			continue;
		}

		EnterCriticalSection(&S_pipe_cs);
		S_pipe		= tx;
		S_pipe_rx	= rx;
		S_pipe_connected = TRUE;
		LeaveCriticalSection(&S_pipe_cs);
		Log("  pipe: overlay connected (tx+rx)\n");

		//	Park on RX only. This thread is not AE's UI thread, and because the
		//	UI thread writes to a DIFFERENT handle its writes can never queue
		//	behind this read.
		{
			char	buf[1024];
			char	acc[PIEFX_LINE_MAX];
			size_t	acc_len = 0;

			for (;;) {
				DWORD got = 0;

				if (!ReadFile(rx, buf, sizeof(buf), &got, NULL) || got == 0) {
					break;
				}
				for (DWORD i = 0; i < got; i++) {
					char ch = buf[i];

					if (ch == '\n') {
						acc[acc_len] = 0;
						if (acc_len) {
							HandleOverlayLine(acc);
						}
						acc_len = 0;
					} else if (ch != '\r' && acc_len + 1 < sizeof(acc)) {
						acc[acc_len++] = ch;
					}
				}
			}
		}

		EnterCriticalSection(&S_pipe_cs);
		S_pipe_connected = FALSE;
		S_pipe		= INVALID_HANDLE_VALUE;
		S_pipe_rx	= INVALID_HANDLE_VALUE;
		LeaveCriticalSection(&S_pipe_cs);

		DisconnectNamedPipe(tx);
		DisconnectNamedPipe(rx);
		CloseHandle(tx);
		CloseHandle(rx);
		Log("  pipe: overlay disconnected\n");

		if (WaitForSingleObject(S_pipe_stop_evt, 0) == WAIT_OBJECT_0) {
			return 0;
		}
		// else: loop and re-accept (overlay restarted)
	}
}

//	Pick pipe names that are actually free. Trying the base names first keeps the
//	dev flow working (an overlay started by hand knows only those); a second AE
//	instance finds them taken and gets its own pair rather than silently running
//	with no overlay, which is what a fixed name with nMaxInstances=1 produced.
static void
ResolvePipeNames(void)
{
	strcpy_s(S_tx_name, sizeof(S_tx_name), PIEFX_PIPE_TX);
	strcpy_s(S_rx_name, sizeof(S_rx_name), PIEFX_PIPE_RX);

	HANDLE probe = CreateNamedPipeA(S_tx_name, PIPE_ACCESS_OUTBOUND,
									PIPE_TYPE_BYTE | PIPE_WAIT, 1, 512, 512, 0, NULL);

	if (probe != INVALID_HANDLE_VALUE) {
		CloseHandle(probe);				// free, and closing releases it again
		return;
	}

	DWORD pid = GetCurrentProcessId();
	sprintf_s(S_tx_name, sizeof(S_tx_name), "%s-%lu", PIEFX_PIPE_TX, (unsigned long)pid);
	sprintf_s(S_rx_name, sizeof(S_rx_name), "%s-%lu", PIEFX_PIPE_RX, (unsigned long)pid);
	Log("  pipe: base names taken (another AE?), using %s / %s\n", S_tx_name, S_rx_name);
}

static void
StartPipeServer(void)
{
	ResolvePipeNames();
	S_pipe_dead_evt = CreateEvent(NULL, FALSE, FALSE, NULL);	// auto-reset
	S_pipe_stop_evt = CreateEvent(NULL, TRUE,  FALSE, NULL);	// manual-reset
	S_pipe_thread = CreateThread(NULL, 0, PipeServerThread, NULL, 0, NULL);
	if (!S_pipe_thread) {
		Log("  pipe: CreateThread failed, err=%lu\n", (unsigned long)GetLastError());
	}
}

static void
StopPipeServer(void)
{
	if (S_pipe_stop_evt) {
		SetEvent(S_pipe_stop_evt);
	}

	//	Unblock the ReadFile parked on RX. Disconnect only - the reader owns the
	//	handle and closes it, so this cannot pull the handle out from under it.
	if (S_pipe_cs_ready) {
		EnterCriticalSection(&S_pipe_cs);
		if (S_pipe_connected && S_pipe_rx != INVALID_HANDLE_VALUE) {
			DisconnectNamedPipe(S_pipe_rx);
		}
		LeaveCriticalSection(&S_pipe_cs);
	}

	//	Nudge a blocking ConnectNamedPipe to return by opening+closing the pipes.
	HANDLE poke_tx = CreateFileA(S_tx_name, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (poke_tx != INVALID_HANDLE_VALUE) {
		CloseHandle(poke_tx);
	}
	HANDLE poke_rx = CreateFileA(S_rx_name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (poke_rx != INVALID_HANDLE_VALUE) {
		CloseHandle(poke_rx);
	}
	if (S_pipe_thread) {
		WaitForSingleObject(S_pipe_thread, 2000);
		CloseHandle(S_pipe_thread);
		S_pipe_thread = NULL;
	}
	if (S_pipe_dead_evt) { CloseHandle(S_pipe_dead_evt); S_pipe_dead_evt = NULL; }
	if (S_pipe_stop_evt) { CloseHandle(S_pipe_stop_evt); S_pipe_stop_evt = NULL; }
	S_pipe_connected = FALSE;
	S_pipe		= INVALID_HANDLE_VALUE;
	S_pipe_rx	= INVALID_HANDLE_VALUE;
}

#else	// AE_OS_WIN

// ---------------------------------------------------------------------------
//	the transport, macOS
// ---------------------------------------------------------------------------
//	poc/native/mac/pieFX_fifo.cpp. A mkfifo pair rather than a named-pipe
//	server, which keeps the overlay's end unchanged — it opens both with a
//	plain File::open on a path. What differs is open() semantics and SIGPIPE,
//	and both are argued at their call sites there.
static void
StartPipeServer(void)
{
	if (!PieFX_StartPipeServer(S_tx_name, sizeof(S_tx_name),
							   S_rx_name, sizeof(S_rx_name),
							   PieFXOverlayLineBridge, NULL,
							   PieFXLogBridge, NULL)) {
		Log("  pipe: server would not start\n");
	}
}

static void
StopPipeServer(void)
{
	PieFX_StopPipeServer();
}

#endif	// AE_OS_WIN


#ifdef AE_OS_WIN
// ---------------------------------------------------------------------------
//	launch the overlay (best-effort; it retries connecting on its own)
// ---------------------------------------------------------------------------
static void
LaunchOverlay(void)
{
	char dir[MAX_PATH] = { 0 };
	HMODULE self = NULL;
	if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&LaunchOverlay), &self)) {
		return;
	}
	if (!GetModuleFileNameA(self, dir, MAX_PATH)) {
		return;
	}
	char *slash = strrchr(dir, '\\');
	if (!slash) {
		return;
	}
	slash[1] = 0;

	char exe[MAX_PATH];
	if (strcpy_s(exe, sizeof(exe), dir) || strcat_s(exe, sizeof(exe), PIEFX_OVERLAY_EXE)) {
		return;
	}
	if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
		Log("  overlay: %s not found beside .aex (run it by hand for dev)\n", exe);
		return;
	}

	//	Arm -> disarm -> arm used to start a second overlay every time, leaving
	//	the earlier ones alive and spinning on a pipe they will never be given.
	if (S_overlay_proc && WaitForSingleObject(S_overlay_proc, 0) == WAIT_TIMEOUT) {
		Log("  overlay: already running, not launching another\n");
		return;
	}
	if (S_overlay_proc) {
		CloseHandle(S_overlay_proc);
		S_overlay_proc = NULL;
	}

	//	Tell it which pipes to use, so a second AE instance's overlay finds its
	//	own pair rather than the first instance's.
	char cmd[MAX_PATH + 320];
	//	DIRECTION-EXPLICIT flags, not tx/rx. "tx" from this side is "rx" from the
	//	overlay's, so the two agreed on the words and disagreed on the meaning:
	//	the launched overlay opened the wrong pipe for reading and never
	//	connected, while a hand-started one used the defaults and worked. Naming
	//	a channel after what flows through it removes the perspective entirely.
	//	--owner-pid is the overlay's lifetime. The pipes cannot be: they close on
	//	every disarm, and the overlay is meant to survive that. Without it an
	//	orphan overlay outlives AE, invisible except in Task Manager.
	sprintf_s(cmd, sizeof(cmd), "\"%s\" --events %s --actions %s --owner-pid %lu",
			  exe, S_tx_name, S_rx_name, (unsigned long)GetCurrentProcessId());

	//	A JOB OBJECT is the actual guarantee, and the reason the two earlier
	//	attempts were not enough. The death hook DID fire and DID terminate
	//	the overlay - the log says so - and AE still would not finish
	//	quitting, because terminating the overlay leaves its WebView2 children
	//	behind, and that is the process tree that hangs on. A job kills the
	//	whole tree, and kills it on HANDLE CLOSE, which process exit does for
	//	us - so it also covers the crash the death hook never reaches, with
	//	neither side waiting on the other.
	if (!S_overlay_job) {
		S_overlay_job = CreateJobObjectA(NULL, NULL);
		if (S_overlay_job) {
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
			ZeroMemory(&li, sizeof(li));
			li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (!SetInformationJobObject(S_overlay_job, JobObjectExtendedLimitInformation,
										 &li, sizeof(li))) {
				Log("  overlay: job limit not set, err=%lu\n", (unsigned long)GetLastError());
			}
		} else {
			Log("  overlay: CreateJobObject failed, err=%lu\n", (unsigned long)GetLastError());
		}
	}

	//	Suspended, so the process is inside the job before it can spawn a
	//	single WebView2 child. Assign-then-resume is the whole point: a child
	//	born in the gap would be outside the job, and outside the job is
	//	exactly the leak this is here to stop.
	STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
	PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
	if (CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi)) {
		if (S_overlay_job && !AssignProcessToJobObject(S_overlay_job, pi.hProcess)) {
			Log("  overlay: AssignProcessToJobObject failed, err=%lu\n",
				(unsigned long)GetLastError());
		}
		ResumeThread(pi.hThread);
		CloseHandle(pi.hThread);
		S_overlay_proc = pi.hProcess;		// kept, to answer "is it still up?"
		Log("  overlay: launched %s (tx=%s, job=%s)\n", exe, S_tx_name,
			S_overlay_job ? "yes" : "NO");
	} else {
		Log("  overlay: CreateProcess failed, err=%lu\n", (unsigned long)GetLastError());
	}
}

#else	// AE_OS_WIN

//	macOS: poc/native/mac/pieFX_launch.cpp. The Windows job object splits in
//	two here — the process GROUP covers a deliberate teardown and takes the
//	WebKit children with it, --owner-pid covers AE crashing — and neither half
//	covers both cases. See MAC_RESULTS.md.
static void
LaunchOverlay(void)
{
	PieFX_LaunchOverlay(S_tx_name, S_rx_name, 0, PieFXLogBridge, NULL);
}

#endif	// AE_OS_WIN


#ifdef AE_OS_WIN
// ---------------------------------------------------------------------------
//	the gesture engine (reused from S2D). Swallow is ALWAYS on while armed.
// ---------------------------------------------------------------------------
static void CancelHoldTimer(void)
{
	if (S_hold_timer) { KillTimer(NULL, S_hold_timer); S_hold_timer = 0; }
}

//	Fires once, when the hold threshold is crossed: summon the wheel.
static void OnHoldDetected(void)
{
	S_hold_firedB = TRUE;
	S_summon_cx = S_rdown_pt.x;
	S_summon_cy = S_rdown_pt.y;
	S_last_sent = S_rdown_pt;

	//	Always summon, selection or not: the overlay greys only the slots whose
	//	action declares what it needs (`requires`), so the wheel still teaches
	//	the gesture and the commands that need nothing still fire.
	SendSummon(S_summon_cx, S_summon_cy, S_has_selection, S_has_comp, S_layer_count);
	Log("  HOLD -> summon at (%ld,%ld) hasSel=%d hasComp=%d layers=%ld\n",
		S_summon_cx, S_summon_cy, S_has_selection, S_has_comp, S_layer_count);
}

static VOID CALLBACK
HoldTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
	CancelHoldTimer();
	if (S_rdownB && !S_hold_firedB) {
		OnHoldDetected();
	}
}

static void ReplayRightClick(void)
{
	INPUT in[2];
	ZeroMemory(in, sizeof(in));
	in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
	in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
	S_replay_pending = 2;
	if (SendInput(2, in, sizeof(INPUT)) != 2) {
		S_replay_pending = 0;
		Log("  !! replay SendInput failed, err=%lu\n", (unsigned long)GetLastError());
	}
}

static LRESULT CALLBACK
MouseProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HC_ACTION && S_active) {
		MOUSEHOOKSTRUCT *mhs = reinterpret_cast<MOUSEHOOKSTRUCT*>(lParam);

		//	Our own replayed click coming back - let it through, don't re-gesture.
		if (S_replay_pending > 0 &&
			(wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP)) {
			S_replay_pending--;
			return CallNextHookEx(S_mouse_hook, code, wParam, lParam);
		}

		switch (wParam) {
			case WM_RBUTTONDOWN: {
				S_rdownB	  = TRUE;
				S_hold_firedB = FALSE;
				S_rdown_tick  = GetTickCount();
				S_rdown_pt	  = mhs ? mhs->pt : S_rdown_pt;
				CancelHoldTimer();
				S_hold_timer = SetTimer(NULL, 0, S_hold_ms, HoldTimerProc);
				//	AE opens its context menu on DOWN, so the DOWN must go.
				return 1;
			}

			case WM_MOUSEMOVE: {
				if (S_rdownB && !S_hold_firedB &&
					(GetTickCount() - S_rdown_tick) >= S_hold_ms) {
					OnHoldDetected();
				}
				//	Raw cursor only. The overlay owns the wheel geometry, so it
				//	does its own hit-testing; the native side deliberately no
				//	longer has an opinion about which slot is under the cursor.
				if (S_rdownB && S_hold_firedB && mhs) {
					if (mhs->pt.x != S_last_sent.x || mhs->pt.y != S_last_sent.y) {
						S_last_sent = mhs->pt;
						SendCursor(mhs->pt.x, mhs->pt.y);
					}
				}
			} break;

			case WM_RBUTTONUP: {
				CancelHoldTimer();
				if (S_rdownB) {
					if (S_hold_firedB) {
						//	Ours. AE saw neither DOWN nor UP.
						//
						//	We do NOT decide what fires: the overlay knows which
						//	slot the cursor is on and sends back a finished
						//	action, which IdleHook executes. Deciding here as
						//	well would fire twice.
						S_rdownB = FALSE;
						SendRelease();
						Log("  UP -> release sent; awaiting the overlay's action\n");
						return 1;
					} else {
						//	Short click: give AE back its right-click.
						S_rdownB = FALSE;
						ReplayRightClick();
						return 1;
					}
				}
			} break;

			default: break;
		}
	}
	return CallNextHookEx(S_mouse_hook, code, wParam, lParam);
}

#else	// AE_OS_WIN

// ---------------------------------------------------------------------------
//	the gesture engine, macOS
// ---------------------------------------------------------------------------
//	The same state machine, in poc/native/mac/pieFX_gesture.mm: a local NSEvent
//	monitor instead of SetWindowsHookEx, dispatch_after with a generation
//	counter instead of SetTimer/KillTimer (a dispatch block cannot be
//	cancelled, so a stale one is invalidated rather than stopped), and the
//	ORIGINAL NSEvent re-posted instead of a synthesised SendInput replay.
//
//	The module reports through these three callbacks and decides nothing else:
//	which slot is under the cursor, and what should fire, remain the overlay's
//	business exactly as they are on Windows.
static void
MacOnHold(int x, int y, void *)
{
	S_summon_cx = x;
	S_summon_cy = y;
	SendSummon(x, y, S_has_selection, S_has_comp, S_layer_count);
	Log("  HOLD -> summon at (%d,%d) hasSel=%d hasComp=%d layers=%ld\n",
		x, y, S_has_selection, S_has_comp, (long)S_layer_count);
}

static void
MacOnMove(int x, int y, void *)
{
	SendCursor(x, y);
}

static void
MacOnRelease(void *)
{
	//	We do NOT decide what fires: the overlay knows which slot the cursor is
	//	on and sends back a finished action, which IdleHook executes.
	SendRelease();
	Log("  UP -> release sent; awaiting the overlay's action\n");
}

static void CancelHoldTimer(void) { /* the module owns its own clock */ }

#endif	// AE_OS_WIN


// ---------------------------------------------------------------------------
//	selection context (safe here in idle), and the deferred anchor action
// ---------------------------------------------------------------------------
//	Which LAYER a selected thing belongs to.
//
//	The comp selection is not a list of layers: a mask, an effect, a property, a
//	keyframe and a mask vertex are all selectable, and every one of them carries
//	the layer that contains it. Counting collection ITEMS therefore counts one
//	layer many times over - twirl a layer down, click its Position and one of its
//	keyframes, and a count of items says three layers are selected when one is.
//
//	Every type but STREAMREF names its owner, so the walk resolves each item to a
//	layer handle and the caller counts DISTINCT handles. STREAMREF carries no
//	union at all and is skipped rather than guessed at.
static AEGP_LayerH
LayerOfCollectionItem(const AEGP_CollectionItemV2 *itemP)
{
	if (!itemP) { return NULL; }

	switch (itemP->type) {
		case AEGP_CollectionItemType_LAYER:
			return itemP->u.layer.layerH;
		case AEGP_CollectionItemType_MASK:
			return itemP->u.mask.layerH;
		case AEGP_CollectionItemType_EFFECT:
			return itemP->u.effect.layerH;
		case AEGP_CollectionItemType_MASK_VERTEX:
			return itemP->u.mask_vertex.mask_sel.layerH;
		case AEGP_CollectionItemType_STREAM:
			switch (itemP->u.stream.type) {
				case AEGP_StreamCollectionItemType_LAYER:
					return itemP->u.stream.u.layer_stream.layerH;
				case AEGP_StreamCollectionItemType_MASK:
					return itemP->u.stream.u.mask_stream.mask.layerH;
				case AEGP_StreamCollectionItemType_EFFECT:
					return itemP->u.stream.u.effect_stream.effect.layerH;
				default:
					return NULL;
			}
		case AEGP_CollectionItemType_KEYFRAME:
			switch (itemP->u.keyframe.stream_coll.type) {
				case AEGP_StreamCollectionItemType_LAYER:
					return itemP->u.keyframe.stream_coll.u.layer_stream.layerH;
				case AEGP_StreamCollectionItemType_MASK:
					return itemP->u.keyframe.stream_coll.u.mask_stream.mask.layerH;
				case AEGP_StreamCollectionItemType_EFFECT:
					return itemP->u.keyframe.stream_coll.u.effect_stream.effect.layerH;
				default:
					return NULL;
			}
		default:
			return NULL;
	}
}

static void
RefreshSelectionContext(AEGP_SuiteHandler &suites)
{
	A_Boolean	hasSel	= FALSE;
	A_Boolean	hasComp	= FALSE;
	A_long		count	= 0;
	AEGP_ItemH	itemH	= NULL;
	AEGP_ItemType type	= AEGP_ItemType_NONE;
	A_Err		err		= A_Err_NONE, err2 = A_Err_NONE;

	ERR(suites.ItemSuite6()->AEGP_GetActiveItem(&itemH));
	if (!err && itemH) {
		ERR(suites.ItemSuite6()->AEGP_GetItemType(itemH, &type));
	}
	if (!err && itemH && type == AEGP_ItemType_COMP) {
		hasComp = TRUE;
		AEGP_CompH		compH = NULL;
		AEGP_Collection2H collH = NULL;
		ERR(suites.CompSuite11()->AEGP_GetCompFromItem(itemH, &compH));
		if (!err && compH) {
			ERR(suites.CompSuite11()->AEGP_GetNewCollectionFromCompSelection(S_my_id, compH, &collH));
			if (!err && collH) {
				A_u_long n = 0;
				ERR2(suites.CollectionSuite2()->AEGP_GetCollectionNumItems(collH, &n));
				hasSel = (n > 0);

				//	Distinct layers, not items. The cap is not a limit on the
				//	selection, only on how many handles are remembered: every
				//	consumer of this number asks "one, or more than one", so an
				//	undercount above the cap cannot change an answer.
				AEGP_LayerH		seen[64];
				A_long			nseen = 0;

				for (A_u_long i = 0; i < n; i++) {
					AEGP_CollectionItemV2	item;
					AEGP_LayerH				layerH = NULL;

					memset(&item, 0, sizeof(item));
					ERR2(suites.CollectionSuite2()->AEGP_GetCollectionItemByIndex(collH, i, &item));
					layerH = LayerOfCollectionItem(&item);
					if (!layerH) { continue; }

					A_Boolean known = FALSE;
					for (A_long k = 0; k < nseen; k++) {
						if (seen[k] == layerH) { known = TRUE; break; }
					}
					if (!known && nseen < (A_long)(sizeof(seen) / sizeof(seen[0]))) {
						seen[nseen++] = layerH;
					}
				}
				count = nseen;
				ERR2(suites.CollectionSuite2()->AEGP_DisposeCollection(collH));
			}
		}
	}
	S_has_selection = hasSel;
	S_has_comp		= hasComp;
	S_layer_count	= count;
}

//	Run ExtendScript and log both channels. Product behaviour: silent on success.
//
//	Deliberately opens NO undo group of its own. A user snippet may legitimately
//	want to own several, and a group we opened could be left dangling by a throw
//	we cannot see - which wedges AE's undo stack until some other script happens
//	to close one. Scripts own their own undo, the way ag_masterNull.jsx does.
//	Last value a script returned, so a probe can put it in front of the user
//	instead of leaving it in a log nobody opens.
static char			S_last_result[2048] = { 0 };

static void
RunScript(AEGP_SuiteHandler &suites, const char *codeZ, const char *whatZ)
{
	A_Boolean		avail	= FALSE;
	AEGP_MemHandle	resultH	= NULL,
					errorH	= NULL;
	A_Err			err		= A_Err_NONE,
					err2	= A_Err_NONE;

	ERR(suites.UtilitySuite6()->AEGP_IsScriptingAvailable(&avail));

	if (err || !avail) {
		Log("  %s: scripting unavailable\n", whatZ);
		SendToast("error", "Scripting is disabled in AE preferences");
		return;
	}

	ERR(suites.UtilitySuite6()->AEGP_ExecuteScript(S_my_id, codeZ, FALSE, &resultH, &errorH));

	if (errorH) {
		A_char *t = NULL;
		if (!suites.MemorySuite1()->AEGP_LockMemHandle(errorH, reinterpret_cast<void**>(&t)) && t && t[0]) {
			Log("  %s ERROR: %s\n", whatZ, t);
			//	Covers the commonest real failure: a snippet whose script has
			//	not been loaded, so its global does not exist yet.
			SendToast("error", t);
		}
		ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(errorH));
		ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(errorH));
	}
	if (resultH) {
		A_char *t = NULL;
		if (!suites.MemorySuite1()->AEGP_LockMemHandle(resultH, reinterpret_cast<void**>(&t)) && t) {
			//	A function with no return statement yields "undefined", which
			//	is SUCCESS - addMasterNull in ag_masterNull.jsx is exactly
			//	that. Logging it verbatim made every successful Master Null
			//	read like a failure.
			if (!t[0] || 0 == strcmp(t, "undefined")) {
				Log("  %s: ok\n", whatZ);
			} else {
				Log("  %s: %s\n", whatZ, t);
			}
			//	Truncate rather than lose it: strcpy_s empties the destination on
			//	overflow, so a long return value would read as no result at all.
			strncpy_s(S_last_result, sizeof(S_last_result), t, _TRUNCATE);
		}
		ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(resultH));
		ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(resultH));
	}
}

static void
RunAnchorAction(AEGP_SuiteHandler &suites, int cell)
{
	int		row = cell / 3,
			col = cell % 3;
	double	fx	= col * 0.5,	// 0, 0.5, 1
			fy	= row * 0.5;
	//	Sized FROM THE FORMAT STRING, not from a number someone typed once. The
	//	fixed 2048 that used to be here fitted the S1 spike's script exactly, and
	//	became a Debug Assertion inside sprintf_s the moment the keyframe-aware
	//	version landed - which takes After Effects down with it. A script that
	//	grows is not a reason to guess a bigger constant.
	size_t	need	= strlen(S_anchor_script_fmt) + 64;	// two %f -> a few chars
	char   *script	= (char *)malloc(need);
	char	what[64];

	if (!script) {
		SendToast("error", "pieFX: out of memory building the anchor script");
		return;
	}
	_snprintf_s(script, need, _TRUNCATE, S_anchor_script_fmt, fx, fy);
	sprintf_s(what, sizeof(what), "anchor cell %d (fx=%.1f fy=%.1f)", cell, fx, fy);
	RunScript(suites, script, what);
	free(script);
}

//	Effect names come from third-party plug-ins as well as Adobe, so they are
//	NOT ours the way the toast strings are, and stripping a quote out of one
//	would silently change an identity the overlay then cannot look up. This is
//	the one place in the native side that escapes rather than strips.
static void
JsonEscapeInto(const char *srcZ, char *outZ, size_t out_max)
{
	size_t o = 0;

	for (const unsigned char *p = (const unsigned char *)srcZ; *p; p++) {
		char esc[8];
		size_t n;

		if (*p == '"')       { strcpy_s(esc, sizeof(esc), "\\\""); }
		else if (*p == '\\') { strcpy_s(esc, sizeof(esc), "\\\\"); }
		else if (*p < 0x20)  { sprintf_s(esc, sizeof(esc), "\\u%04x", (unsigned)*p); }
		else                 { esc[0] = (char)*p; esc[1] = 0; }

		n = strlen(esc);
		if (o + n >= out_max) { break; }
		memcpy(outZ + o, esc, n);
		o += n;
	}
	outZ[o] = 0;
}

//	ANIMATION PRESETS, which are not effects and are not in any AEGP suite.
//
//	AE's own Effects & Presets panel lists both together, so a search that
//	offers only effects is a search that answers half the question the user is
//	asking. There is no enumeration API for presets - the panel finds them by
//	SCANNING FOLDERS, and so does this.
//
//	Two roots:
//	  <install>/Support Files/Presets   what AE ships. Found from our OWN module
//	                                    path (the .aex sits in
//	                                    Support Files/Plug-ins), so it needs no
//	                                    registry key and no guess at a drive.
//	  <Documents>/Adobe/After Effects*/User Presets
//	                                    what the user has saved.
//
//	Documents is NOT %USERPROFILE%\\Documents. Measured on the author's machine:
//	it is C:\\Users\\aldai\\OneDrive\\Documentos - redirected to OneDrive AND
//	localised. SHGetFolderPath is what follows both; building the path by hand
//	would have found zero user presets and reported it as "you have none".
#ifdef AE_OS_WIN
static void
WalkPresetFolder(FILE *fp, const char *rootZ, const char *labelZ, const char *relZ,
				 A_long *countP, int depth)
{
	if (depth > 8 || *countP >= PIEFX_PRESET_MAX) { return; }

	char glob[MAX_PATH];

	sprintf_s(glob, sizeof(glob), "%s\\%s%s*", rootZ, relZ, relZ[0] ? "\\" : "");

	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(glob, &fd);

	if (h == INVALID_HANDLE_VALUE) { return; }

	do {
		if (fd.cFileName[0] == '.') { continue; }

		char rel[MAX_PATH];

		if (relZ[0]) {
			sprintf_s(rel, sizeof(rel), "%s\\%s", relZ, fd.cFileName);
		} else {
			strcpy_s(rel, sizeof(rel), fd.cFileName);
		}

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			WalkPresetFolder(fp, rootZ, labelZ, rel, countP, depth + 1);
			continue;
		}

		size_t n = strlen(fd.cFileName);

		if (n < 5 || _stricmp(fd.cFileName + n - 4, ".ffx")) { continue; }
		if (*countP >= PIEFX_PRESET_MAX) { break; }

		//	The NAME is the file without .ffx, and the CATEGORY is the folder
		//	path it was found under - which is exactly how AE's own panel groups
		//	them, and the only grouping a .ffx file carries. `relZ` is already
		//	that folder, because this recursion descends a directory at a time.
		//
		//	`labelZ` is what the ROOT is called: empty for AE's own presets,
		//	whose top-level folders are the categories people know ("Text",
		//	"Transitions - Dissolves"), and the version folder for a user's, so
		//	a preset saved under an older AE is visibly from there.
		char name[MAX_PATH];
		char cat[MAX_PATH] = { 0 };
		char full[MAX_PATH];

		strcpy_s(name, sizeof(name), fd.cFileName);
		name[n - 4] = 0;

		if (labelZ[0] && relZ[0])	{ sprintf_s(cat, sizeof(cat), "%s/%s", labelZ, relZ); }
		else if (labelZ[0])			{ strcpy_s(cat, sizeof(cat), labelZ); }
		else						{ strcpy_s(cat, sizeof(cat), relZ); }

		for (char *q = cat; *q; q++) {
			if (*q == '\\') { *q = '/'; }
		}
		sprintf_s(full, sizeof(full), "%s\\%s", rootZ, rel);

		char e_name[MAX_PATH * 6];
		char e_path[MAX_PATH * 6];
		char e_cat[MAX_PATH * 6];

		JsonEscapeInto(name, e_name, sizeof(e_name));
		JsonEscapeInto(full, e_path, sizeof(e_path));
		JsonEscapeInto(cat,  e_cat,  sizeof(e_cat));

		fprintf(fp, "%s    { \"name\": \"%s\", \"path\": \"%s\", \"category\": \"%s\" }",
				*countP ? ",\n" : "", e_name, e_path, e_cat);
		(*countP)++;
	} while (FindNextFileA(h, &fd));

	FindClose(h);
}

//	<install>/Support Files/Presets, found by CLIMBING from our own module path
//	rather than by counting folders up from it.
//
//	Counting is what the first version did - two levels, on the assumption that
//	the .aex sits directly in Plug-ins - and it was wrong on the author's own
//	machine, where the plug-in is installed in `Plug-ins\AGS\`. It looked for
//	`Plug-ins\Presets`, found nothing, and reported zero shipped presets while
//	the user-presets half of the same walk worked perfectly. A plug-in folder is
//	the USER's to organise; nesting is normal and the depth is not ours to
//	assume.
//
//	So: climb, and at each ancestor ask whether it looks like Support Files -
//	which is the folder that contains BOTH `Presets` and `Plug-ins`. That pair
//	is what makes the answer specific; a bare `Presets` folder somewhere up the
//	tree could be anyone's. If no ancestor has both, the first one with a
//	`Presets` at all is taken as a fallback, because being slightly too willing
//	beats offering nothing.
static A_Boolean
ShippedPresetRoot(char *outZ, size_t out_max)
{
	char	dir[MAX_PATH] = { 0 };
	char	fallback[MAX_PATH] = { 0 };
	HMODULE	self = NULL;

	if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&ShippedPresetRoot), &self)) {
		return FALSE;
	}
	if (!GetModuleFileNameA(self, dir, MAX_PATH)) { return FALSE; }

	Log("  presets: module is %s\n", dir);

	//	Up to eight ancestors. AE's own install is two, and nobody nests a
	//	plug-in six deep, but the loop costs nothing and the constant is not a
	//	claim about anyone's folders.
	for (int i = 0; i < 8; i++) {
		char *slash = strrchr(dir, '\\');

		if (!slash) { break; }
		*slash = 0;

		char presets[MAX_PATH];
		char plugins[MAX_PATH];

		if (strcpy_s(presets, sizeof(presets), dir) ||
			strcat_s(presets, sizeof(presets), "\\Presets")) {
			break;
		}
		if (GetFileAttributesA(presets) == INVALID_FILE_ATTRIBUTES) { continue; }

		if (!fallback[0]) { strcpy_s(fallback, sizeof(fallback), presets); }

		strcpy_s(plugins, sizeof(plugins), dir);
		strcat_s(plugins, sizeof(plugins), "\\Plug-ins");

		if (GetFileAttributesA(plugins) != INVALID_FILE_ATTRIBUTES) {
			strcpy_s(outZ, out_max, presets);
			Log("  presets: Support Files found at %s\n", dir);
			return TRUE;
		}
	}

	if (fallback[0]) {
		strcpy_s(outZ, out_max, fallback);
		Log("  presets: no Plug-ins sibling; falling back to %s\n", fallback);
		return TRUE;
	}
	return FALSE;
}

static A_long
WritePresets(FILE *fp)
{
	A_long	count = 0;
	char	root[MAX_PATH] = { 0 };

	if (ShippedPresetRoot(root, sizeof(root))) {
		WalkPresetFolder(fp, root, "", "", &count, 0);
		Log("  presets: %ld under %s\n", count, root);
	} else {
		Log("  presets: no shipped Presets folder beside the plug-in\n");
	}

	char docs[MAX_PATH] = { 0 };

	if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docs))) {
		char glob[MAX_PATH];

		//	Every "After Effects*" folder, not just the running version's. The
		//	version folder becomes part of the category, so a preset saved under
		//	an older AE is offered AND is visibly from there - which is better
		//	than a mapping from AE's version number to its marketing year, a
		//	thing that has already changed once.
		sprintf_s(glob, sizeof(glob), "%s\\Adobe\\After Effects*", docs);

		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA(glob, &fd);

		if (h != INVALID_HANDLE_VALUE) {
			do {
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { continue; }
				if (fd.cFileName[0] == '.') { continue; }

				char up[MAX_PATH];
				char label[MAX_PATH];
				A_long before = count;

				sprintf_s(up, sizeof(up), "%s\\Adobe\\%s\\User Presets", docs, fd.cFileName);
				if (GetFileAttributesA(up) == INVALID_FILE_ATTRIBUTES) { continue; }

				//	"User Presets (After Effects 2026)" - the version is in the
				//	category because more than one of these folders exists on
				//	any machine that has upgraded, and a preset from the wrong
				//	one is worth being able to see.
				sprintf_s(label, sizeof(label), "User Presets (%s)", fd.cFileName);
				WalkPresetFolder(fp, up, label, "", &count, 0);
				if (count > before) {
					Log("  presets: %ld under %s\n", count - before, up);
				}
			} while (FindNextFileA(h, &fd));
			FindClose(h);
		}
	} else {
		Log("  presets: could not resolve the Documents folder\n");
	}
	return count;
}

//	The S5A walk, ported out of the frozen spike and pointed at a file the
//	overlay can read instead of a human-readable dump.
//
//	Everything walked is written, obsolete and uncategorised entries included.
//	The three sharp edges the catalogue has (31-character match names, 50
//	`_Obsolete` entries that collide with live ones on display name, 107 with no
//	category at all) are FILTERING decisions, and filtering is the search UI's
//	job - the overlay owns what the user sees. What the plug-in owes it is the
//	API's own strings, unedited, with the category that makes them separable.
//
//	`claimed` is written beside the count for the same reason S5A reported it:
//	the interesting number is not how many there are, it is whether walking the
//	list agrees with what AE says is in it.

#else	// AE_OS_WIN

//	MAC_PORT.md step 5. The presets live under a localised, possibly
//	redirected Documents folder on Windows, which is why SHGetFolderPath is
//	used rather than a hand-built path; the macOS equivalent has its own
//	answer and its own place to look, and that is a decision rather than a
//	translation.
//
//	Returning 0 is honest here in a way the clipboard stub cannot be: the
//	catalogue is a LIST, and an empty one degrades to "no presets found"
//	rather than to a silent wrong answer. The search window still works on the
//	effects catalogue.
static A_long
WritePresets(FILE *fp)
{
	(void)fp;
	Log("  presets: not enumerated on macOS yet (MAC_PORT step 5)\n");
	return 0;
}

#endif	// AE_OS_WIN

#ifdef AE_OS_WIN
static void
WriteEffectCatalogue(AEGP_SuiteHandler &suites)
{
	A_Err	err		= A_Err_NONE;
	A_long	claimed	= 0;
	char	dir[MAX_PATH];
	char	path[MAX_PATH];
	DWORD	len;
	FILE	*fp		= NULL;

	len = GetEnvironmentVariableA("APPDATA", dir, MAX_PATH);
	if (!len || len >= MAX_PATH - (DWORD)strlen(PIEFX_EFFECTS_REL) - 2) {
		Log("  effects: no APPDATA; catalogue not written\n");
		return;
	}
	strcpy_s(path, MAX_PATH, dir);
	strcat_s(path, MAX_PATH, "\\pieFX");
	//	The settings window may never have run on this machine, so the folder is
	//	not a given. An existing one is not an error.
	CreateDirectoryA(path, NULL);

	strcpy_s(path, MAX_PATH, dir);
	strcat_s(path, MAX_PATH, "\\");
	strcat_s(path, MAX_PATH, PIEFX_EFFECTS_REL);

	if (fopen_s(&fp, path, "wb") || !fp) {
		Log("  effects: cannot write %s\n", path);
		return;
	}

	ERR(suites.EffectSuite4()->AEGP_GetNumInstalledEffects(&claimed));

	//	No BOM, ever. The overlay's JSON.parse rejects one outright, and a
	//	settings file with a BOM is exactly how this project already lost a
	//	session once.
	fprintf(fp, "{\n  \"effects\": [\n");

	AEGP_InstalledEffectKey	key		= AEGP_InstalledEffectKey_NONE;
	A_long					walked	= 0;

	while (!err) {
		ERR(suites.EffectSuite4()->AEGP_GetNextInstalledEffect(key, &key));

		if (err || AEGP_InstalledEffectKey_NONE == key) {
			break;
		}

		A_char	name[AEGP_MAX_EFFECT_NAME_SIZE]			= { 0 };
		A_char	match[AEGP_MAX_EFFECT_MATCH_NAME_SIZE]	= { 0 };
		A_char	cat[AEGP_MAX_EFFECT_CATEGORY_NAME_SIZE]	= { 0 };
		char	e_name[AEGP_MAX_EFFECT_NAME_SIZE * 6];
		char	e_match[AEGP_MAX_EFFECT_MATCH_NAME_SIZE * 6];
		char	e_cat[AEGP_MAX_EFFECT_CATEGORY_NAME_SIZE * 6];

		ERR(suites.EffectSuite4()->AEGP_GetEffectName(key, name));
		ERR(suites.EffectSuite4()->AEGP_GetEffectMatchName(key, match));
		ERR(suites.EffectSuite4()->AEGP_GetEffectCategory(key, cat));

		if (err) { break; }

		JsonEscapeInto(name,  e_name,  sizeof(e_name));
		JsonEscapeInto(match, e_match, sizeof(e_match));
		JsonEscapeInto(cat,   e_cat,   sizeof(e_cat));

		fprintf(fp, "%s    { \"name\": \"%s\", \"match\": \"%s\", \"category\": \"%s\" }",
				walked ? ",\n" : "", e_name, e_match, e_cat);
		walked++;
	}

	fprintf(fp, "\n  ],\n  \"presets\": [\n");

	//	Presets share the file because they share the SEARCH. One file, one read,
	//	one moment in the session when the catalogue is built - splitting them
	//	would mean two files that can disagree about how old they are.
	A_long presets = WritePresets(fp);

	fprintf(fp, "\n  ],\n  \"walked\": %ld,\n  \"claimed\": %ld,\n  \"presets_found\": %ld\n}\n",
			walked, claimed, presets);
	fclose(fp);

	Log("  effects: wrote %ld entries (AE claims %ld)%s -> %s\n",
		walked, claimed, (walked == claimed) ? "" : "  *** MISMATCH ***", path);

	if (err) {
		Log("  effects: enumeration stopped early with AEGP error %d\n", (int)err);
	}
}

#else	// AE_OS_WIN

//	MAC_PORT.md step 5, blocked on the same %APPDATA% agreement as
//	ReadSettings — the overlay READS this file.
//
//	And there is a second reason it is not merely a path fix. Phase 0 measured
//	AEGP_GetEffectName returning single-byte legacy text on a Spanish AE, and
//	these names go straight into effects.json, where JSON.parse on invalid
//	UTF-8 is not a graceful failure. The Unicode accessors are a PREREQUISITE
//	for this function on a localised Mac, not a polish item.
static void
WriteEffectCatalogue(AEGP_SuiteHandler &suites)
{
	(void)suites;
	Log("  effects: catalogue not written on macOS yet (MAC_PORT step 5)\n");
}

#endif	// AE_OS_WIN


// ---------------------------------------------------------------------------
//	Current frame -> the clipboard.
//
//	Why a file is in the middle of this: ExtendScript can write a PNG
//	(`saveFrameToPng`) and can do NOTHING with the clipboard, and this side can
//	own the clipboard and cannot render a frame without taking on the whole
//	render-queue API. So the frame goes out through a temp PNG and comes straight
//	back in as pixels. The file is written, read and left in %TEMP%, overwritten
//	by the next copy.
//
//	THREE formats go on the clipboard, and the reason is alpha. A comp frame can
//	be transparent, and the classic CF_DIB cannot say so - every consumer reads
//	its fourth byte differently, and most read it as nothing. So:
//	  "PNG"       the original file bytes. Photoshop, Chrome, Figma, Slack and
//	              anything modern take this first, and it is exactly the frame.
//	  CF_DIBV5    32bpp with a real alpha mask, for consumers that understand it.
//	  CF_DIB      the same pixels forced opaque, as the universal fallback. A
//	              transparent frame pasted through THIS one shows whatever was
//	              behind the alpha, which is usually black - that is a property
//	              of the format, not a bug here.
//	Order is a preference hint, not a rule; the consumer chooses.
#ifdef AE_OS_WIN
static A_Boolean
ReadWholeFile(const char *pathZ, BYTE **bufPP, DWORD *sizeP)
{
	FILE	*fp = NULL;
	long	 n	= 0;

	*bufPP = NULL;
	*sizeP = 0;

	if (fopen_s(&fp, pathZ, "rb") || !fp) { return FALSE; }
	fseek(fp, 0, SEEK_END);
	n = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (n <= 0) { fclose(fp); return FALSE; }

	BYTE *buf = (BYTE *)malloc((size_t)n);
	if (!buf) { fclose(fp); return FALSE; }

	size_t got = fread(buf, 1, (size_t)n, fp);
	fclose(fp);

	if (got != (size_t)n) { free(buf); return FALSE; }

	*bufPP = buf;
	*sizeP = (DWORD)n;
	return TRUE;
}

//	Copy a block into a GMEM_MOVEABLE handle, ready to hand to the clipboard.
//	Ownership passes to the clipboard on success, so nothing here is freed after
//	a successful SetClipboardData - freeing it is exactly how a paste turns into
//	garbage some seconds later.
static HGLOBAL
GlobalFrom(const void *srcP, size_t bytes)
{
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);

	if (!h) { return NULL; }

	void *p = GlobalLock(h);
	if (!p) { GlobalFree(h); return NULL; }
	memcpy(p, srcP, bytes);
	GlobalUnlock(h);
	return h;
}

static A_Boolean
PngFileToClipboard(const char *pathZ, char *errZ, size_t err_max)
{
	IWICImagingFactory	*factoryP	= NULL;
	IWICBitmapDecoder	*decoderP	= NULL;
	IWICBitmapFrameDecode *frameP	= NULL;
	IWICFormatConverter	*convP		= NULL;
	BYTE				*pngP		= NULL;
	BYTE				*pixP		= NULL;
	DWORD				 pngBytes	= 0;
	UINT				 w = 0, h = 0;
	A_Boolean			 ok			= FALSE;
	HRESULT				 hr			= S_OK;

	errZ[0] = 0;

	//	AE's UI thread is already in an apartment; asking again is how you find
	//	out without assuming. S_FALSE means "already initialised by us",
	//	RPC_E_CHANGED_MODE means "already initialised differently" - both are
	//	fine to work in, and only the first of them owes a CoUninitialize.
	HRESULT coHr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	A_Boolean weInitedCom = (coHr == S_OK || coHr == S_FALSE);

	if (!ReadWholeFile(pathZ, &pngP, &pngBytes)) {
		strcpy_s(errZ, err_max, "the frame file could not be read back");
		goto done;
	}

	hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
						  IID_PPV_ARGS(&factoryP));
	if (FAILED(hr)) { sprintf_s(errZ, err_max, "WIC unavailable (0x%08lx)", (unsigned long)hr); goto done; }

	{
		//	Decoded from MEMORY, not from the path a second time: the file has
		//	already been read for the "PNG" clipboard format, and reading it
		//	twice invites the two copies to disagree.
		IWICStream *streamP = NULL;

		hr = factoryP->CreateStream(&streamP);
		if (SUCCEEDED(hr)) { hr = streamP->InitializeFromMemory(pngP, pngBytes); }
		if (SUCCEEDED(hr)) {
			hr = factoryP->CreateDecoderFromStream(streamP, NULL,
												   WICDecodeMetadataCacheOnLoad, &decoderP);
		}
		if (streamP) { streamP->Release(); }
		if (FAILED(hr)) { sprintf_s(errZ, err_max, "the frame PNG could not be decoded (0x%08lx)", (unsigned long)hr); goto done; }
	}

	hr = decoderP->GetFrame(0, &frameP);
	if (SUCCEEDED(hr)) { hr = factoryP->CreateFormatConverter(&convP); }
	if (SUCCEEDED(hr)) {
		hr = convP->Initialize(frameP, GUID_WICPixelFormat32bppBGRA,
							   WICBitmapDitherTypeNone, NULL, 0.0,
							   WICBitmapPaletteTypeCustom);
	}
	if (SUCCEEDED(hr)) { hr = convP->GetSize(&w, &h); }
	if (FAILED(hr) || !w || !h) { sprintf_s(errZ, err_max, "the frame has no pixels (0x%08lx)", (unsigned long)hr); goto done; }

	{
		const UINT stride = w * 4;
		const size_t bytes = (size_t)stride * h;

		pixP = (BYTE *)malloc(bytes);
		if (!pixP) { strcpy_s(errZ, err_max, "out of memory reading the frame"); goto done; }

		hr = convP->CopyPixels(NULL, stride, (UINT)bytes, pixP);
		if (FAILED(hr)) { sprintf_s(errZ, err_max, "the frame could not be read (0x%08lx)", (unsigned long)hr); goto done; }

		//	DIBs are bottom-up. Flipping here rather than asking WIC to do it
		//	keeps one representation in play.
		for (UINT y = 0; y < h / 2; y++) {
			BYTE *a = pixP + (size_t)y * stride;
			BYTE *b = pixP + (size_t)(h - 1 - y) * stride;

			for (UINT i = 0; i < stride; i++) {
				BYTE t = a[i]; a[i] = b[i]; b[i] = t;
			}
		}

		//	CF_DIBV5: alpha kept, and the channel masks spelled out so a reader
		//	does not have to guess which byte is which.
		size_t v5Bytes = sizeof(BITMAPV5HEADER) + bytes;
		BYTE *v5P = (BYTE *)malloc(v5Bytes);
		//	CF_DIB: the same pixels, forced opaque.
		size_t dibBytes = sizeof(BITMAPINFOHEADER) + bytes;
		BYTE *dibP = (BYTE *)malloc(dibBytes);

		if (!v5P || !dibP) {
			free(v5P); free(dibP);
			strcpy_s(errZ, err_max, "out of memory building the clipboard image");
			goto done;
		}

		BITMAPV5HEADER *v5 = (BITMAPV5HEADER *)v5P;
		memset(v5, 0, sizeof(*v5));
		v5->bV5Size			= sizeof(BITMAPV5HEADER);
		v5->bV5Width		= (LONG)w;
		v5->bV5Height		= (LONG)h;
		v5->bV5Planes		= 1;
		v5->bV5BitCount		= 32;
		v5->bV5Compression	= BI_BITFIELDS;
		v5->bV5SizeImage	= (DWORD)bytes;
		v5->bV5RedMask		= 0x00FF0000;
		v5->bV5GreenMask	= 0x0000FF00;
		v5->bV5BlueMask		= 0x000000FF;
		v5->bV5AlphaMask	= 0xFF000000;
		v5->bV5CSType		= LCS_WINDOWS_COLOR_SPACE;
		v5->bV5Intent		= LCS_GM_IMAGES;
		memcpy(v5P + sizeof(BITMAPV5HEADER), pixP, bytes);

		BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)dibP;
		memset(bi, 0, sizeof(*bi));
		bi->biSize			= sizeof(BITMAPINFOHEADER);
		bi->biWidth			= (LONG)w;
		bi->biHeight		= (LONG)h;
		bi->biPlanes		= 1;
		bi->biBitCount		= 32;
		bi->biCompression	= BI_RGB;
		bi->biSizeImage		= (DWORD)bytes;
		memcpy(dibP + sizeof(BITMAPINFOHEADER), pixP, bytes);
		{
			BYTE *q = dibP + sizeof(BITMAPINFOHEADER);
			for (size_t i = 3; i < bytes; i += 4) { q[i] = 0xFF; }
		}

		HGLOBAL hPng = GlobalFrom(pngP, pngBytes);
		HGLOBAL hV5  = GlobalFrom(v5P, v5Bytes);
		HGLOBAL hDib = GlobalFrom(dibP, dibBytes);

		free(v5P);
		free(dibP);

		if (!hPng || !hV5 || !hDib) {
			if (hPng) { GlobalFree(hPng); }
			if (hV5)  { GlobalFree(hV5); }
			if (hDib) { GlobalFree(hDib); }
			strcpy_s(errZ, err_max, "out of memory handing the image to Windows");
			goto done;
		}

		//	NULL owner: this process has no window of its own on AE's thread,
		//	and the clipboard does not require one.
		if (!OpenClipboard(NULL)) {
			GlobalFree(hPng); GlobalFree(hV5); GlobalFree(hDib);
			sprintf_s(errZ, err_max, "another app is holding the clipboard (err %lu)",
					  (unsigned long)GetLastError());
			goto done;
		}
		EmptyClipboard();

		UINT cfPng = RegisterClipboardFormatA("PNG");
		A_Boolean placed = FALSE;

		if (cfPng && SetClipboardData(cfPng, hPng)) { placed = TRUE; } else { GlobalFree(hPng); }
		if (SetClipboardData(CF_DIBV5, hV5))		{ placed = TRUE; } else { GlobalFree(hV5); }
		if (SetClipboardData(CF_DIB, hDib))			{ placed = TRUE; } else { GlobalFree(hDib); }

		CloseClipboard();

		if (!placed) {
			strcpy_s(errZ, err_max, "Windows refused every clipboard format");
			goto done;
		}
		ok = TRUE;
	}

done:
	if (convP)		{ convP->Release(); }
	if (frameP)		{ frameP->Release(); }
	if (decoderP)	{ decoderP->Release(); }
	if (factoryP)	{ factoryP->Release(); }
	free(pixP);
	free(pngP);
	if (weInitedCom) { CoUninitialize(); }
	return ok;
}

//	Is the frame on disk yet, and finished?
//
//	`saveFrameToPng` returns before the bytes are necessarily readable, so this
//	polls instead of asking once. It insists on the same non-zero size twice in
//	a row: a file that has appeared is not a file that has been written, and a
//	half-written PNG decodes to an error that reads like a broken frame.
static A_Boolean
WaitForFrameFile(const char *pathZ, DWORD wait_ms)
{
	const DWORD	step	= 40;
	DWORD		waited	= 0;
	LARGE_INTEGER last;

	last.QuadPart = -1;

	for (;;) {
		WIN32_FILE_ATTRIBUTE_DATA fad;

		if (GetFileAttributesExA(pathZ, GetFileExInfoStandard, &fad)) {
			LARGE_INTEGER now;

			now.HighPart = (LONG)fad.nFileSizeHigh;
			now.LowPart  = fad.nFileSizeLow;

			if (now.QuadPart > 0 && now.QuadPart == last.QuadPart) {
				return TRUE;
			}
			last = now;
		}
		if (waited >= wait_ms) { return FALSE; }
		Sleep(step);
		waited += step;
	}
}

static void
CopyFrameToClipboard(AEGP_SuiteHandler &suites)
{
	char	dir[MAX_PATH]	= { 0 };
	char	path[MAX_PATH]	= { 0 };
	char	fwd[MAX_PATH]	= { 0 };
	DWORD	len				= GetTempPathA(MAX_PATH, dir);

	if (!len || len > MAX_PATH - 32) {
		SendToast("error", "pieFX: no temp folder to write the frame to");
		return;
	}
	strcpy_s(path, MAX_PATH, dir);
	strcat_s(path, MAX_PATH, "pieFX_clipboard_frame.png");

	strcpy_s(fwd, MAX_PATH, path);
	for (char *p = fwd; *p; p++) {
		if (*p == '\\') { *p = '/'; }
	}

	//	Deleted HERE, by the side that will read it back. A previous copy's file
	//	is the one thing that could make a failed save look like a success, and
	//	"is the frame there?" is only a real question if we are the ones who
	//	took the old one away.
	DeleteFileA(path);

	//	The frame number is the one the TIMELINE shows, which is not
	//	time/frameDuration: a comp can start at any frame, and reporting 0 for a
	//	comp whose first frame is 1001 would be a number the user cannot find.
	//	resolutionFactor is forced to 1:1 and restored for the same reason
	//	`Save Frame as PNG` does it - copying at Half because that is how you
	//	happened to be previewing is a silent wrong answer.
	char code[1600];

	sprintf_s(code, sizeof(code),
		"(function(){"
		"var c=app.project.activeItem;"
		"if(!(c&&c instanceof CompItem))return'pieFX: no comp is active';"
		"if(typeof c.saveFrameToPng!=='function')return'pieFX: this AE has no saveFrameToPng';"
		"var f=new File(\"%s\");"
		"var res=c.resolutionFactor;"
		"try{c.resolutionFactor=[1,1];c.saveFrameToPng(c.time,f);}"
		"finally{c.resolutionFactor=res;}"
		"var off=(typeof c.displayStartFrame==='number')?c.displayStartFrame"
		":Math.round(c.displayStartTime/c.frameDuration);"
		"var fr=Math.round(c.time/c.frameDuration)+off;"
		//	fsName LAST, and it is a diagnostic, not decoration: if AE ever
		//	resolves this File somewhere other than where we are about to look,
		//	that fact is in the log the first time it happens instead of after
		//	another build.
		"return'ok|'+fr+'|'+c.name+'|'+f.fsName;"
		"})()", fwd);

	S_last_result[0] = 0;
	RunScript(suites, code, "copy-frame");

	//	RunScript has already toasted anything it threw. What is left to check is
	//	the RETURNED value, which is how the script reports the failures that are
	//	not exceptions.
	if (0 != strncmp(S_last_result, "ok|", 3)) {
		if (!strncmp(S_last_result, "pieFX: ", 7)) {
			SendToast("error", S_last_result + 7);
		}
		Log("  copy-frame: script said \"%s\"\n", S_last_result);
		return;
	}

	char	frame[32]	= { 0 };
	char	comp[200]	= { 0 };
	char	wrote[MAX_PATH] = { 0 };
	const char *p	 = S_last_result + 3;
	const char *bar	 = strchr(p, '|');
	//	A comp may legitimately be named "A|B", so the frame is the FIRST field
	//	and the path is the LAST; the name is whatever is between them.
	const char *last = strrchr(p, '|');

	if (bar) {
		size_t n = (size_t)(bar - p);
		if (n >= sizeof(frame)) { n = sizeof(frame) - 1; }
		memcpy(frame, p, n);
		frame[n] = 0;

		if (last && last != bar) {
			size_t m = (size_t)(last - (bar + 1));
			if (m >= sizeof(comp)) { m = sizeof(comp) - 1; }
			memcpy(comp, bar + 1, m);
			comp[m] = 0;
			strcpy_s(wrote, sizeof(wrote), last + 1);
		} else {
			strcpy_s(comp, sizeof(comp), bar + 1);
		}
	}
	Log("  copy-frame: AE says it wrote \"%s\"\n", wrote[0] ? wrote : "(no path returned)");

	//	WAIT for the file, rather than asking ExtendScript whether it is there.
	//	The first version had the script check `f.exists` immediately after
	//	saveFrameToPng and report "AE wrote no frame", which is what it did on
	//	the first live run - and that check could not tell "AE failed" from "the
	//	bytes are not on disk yet". This side has to open the file anyway, so it
	//	is the side that should decide whether it exists, and it can afford to
	//	wait a moment for it.
	//
	//	Two equal non-zero sizes in a row, because a file that has appeared is
	//	not necessarily a file that has been finished.
	if (!WaitForFrameFile(path, 4000)) {
		char t[MAX_PATH + 120];

		if (wrote[0] && 0 != _stricmp(wrote, path)) {
			sprintf_s(t, sizeof(t), "Frame not copied: AE wrote to %s, not %s", wrote, path);
		} else {
			sprintf_s(t, sizeof(t), "Frame not copied: AE wrote nothing to %s", path);
		}
		Log("  copy-frame: no file after saveFrameToPng at %s\n", path);
		SendToast("error", t);
		return;
	}

	char err[200] = { 0 };

	if (!PngFileToClipboard(path, err, sizeof(err))) {
		char t[280];
		sprintf_s(t, sizeof(t), "Frame not copied: %s", err[0] ? err : "unknown error");
		Log("  copy-frame: %s\n", t);
		SendToast("error", t);
		return;
	}

	char t[280];
	sprintf_s(t, sizeof(t), "Copied Frame %s from %s", frame[0] ? frame : "?", comp);
	Log("  copy-frame: %s\n", t);
	SendToast("info", t);
}

#else	// AE_OS_WIN

//	MAC_PORT.md step 5. Not written yet, and stubbed LOUDLY rather than
//	silently: a copy-frame that quietly does nothing looks identical to a copy
//	that worked, and the user would find out at paste time.
//
//	The port makes this code SMALLER. Three clipboard formats exist on Windows
//	because CF_DIB cannot express alpha; NSPasteboard takes the PNG bytes as
//	they are, so the WIC decode, the DIB construction and the force-opaque
//	fallback all disappear.
static void
CopyFrameToClipboard(AEGP_SuiteHandler &suites)
{
	Log("  copy-frame: not implemented on macOS yet (MAC_PORT step 5)\n");
	suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id,
		"pieFX: copying the frame is not available on macOS yet.");
}

#endif	// AE_OS_WIN


//	S5's lookup: walk the installed catalogue for an exact match name.
static A_Boolean
FindEffectKeyByMatchName(
	AEGP_SuiteHandler		&suites,
	const char				*wantedZ,
	AEGP_InstalledEffectKey	*keyP)
{
	A_Err					err = A_Err_NONE;
	AEGP_InstalledEffectKey	key = AEGP_InstalledEffectKey_NONE;
	A_char					match[AEGP_MAX_EFFECT_MATCH_NAME_SIZE];

	while (!err) {
		ERR(suites.EffectSuite4()->AEGP_GetNextInstalledEffect(key, &key));

		if (err || AEGP_InstalledEffectKey_NONE == key) {
			break;
		}
		match[0] = 0;
		ERR(suites.EffectSuite4()->AEGP_GetEffectMatchName(key, match));

		if (!err && 0 == strcmp(match, wantedZ)) {
			*keyP = key;
			return TRUE;
		}
	}
	return FALSE;
}

static void
ApplyEffectByMatchName(AEGP_SuiteHandler &suites, const char *matchZ)
{
	A_Err					err		= A_Err_NONE;
	AEGP_LayerH				layerH	= NULL;
	AEGP_InstalledEffectKey	key		= AEGP_InstalledEffectKey_NONE;
	AEGP_EffectRefH			refH	= NULL;

	//	Returns a layer only when exactly one is selected.
	ERR(suites.LayerSuite9()->AEGP_GetActiveLayer(&layerH));

	if (err || !layerH) {
		Log("  effect \"%s\": no single selected layer\n", matchZ);
		SendToast("error", "Select one layer first");
		return;
	}
	if (!FindEffectKeyByMatchName(suites, matchZ, &key)) {
		//	Match names truncate at 31 chars (PF_MAX_EFFECT_NAME_LEN), so a name
		//	copied from documentation can legitimately fail to match what the
		//	API returns. Always round-trip through the catalogue's own strings.
		Log("  effect \"%s\": not installed (or the stored name was not round-tripped)\n", matchZ);
		char t[320];
		sprintf_s(t, sizeof(t), "Effect not installed: %s", matchZ);
		SendToast("error", t);
		return;
	}

	suites.UtilitySuite3()->AEGP_StartUndoGroup("pieFX: Apply Effect");
	ERR(suites.EffectSuite4()->AEGP_ApplyEffect(S_my_id, layerH, key, &refH));

	if (refH) {
		suites.EffectSuite4()->AEGP_DisposeEffect(refH);
	}
	suites.UtilitySuite3()->AEGP_EndUndoGroup();
	Log("  effect \"%s\": applied\n", matchZ);
}

// ---------------------------------------------------------------------------
//	the executor table
// ---------------------------------------------------------------------------
//	Apply an animation preset to the selected layer.
//
//	Through ExtendScript, unlike `effect`, because `applyPreset` is a LAYER
//	method in the scripting DOM and has no AEGP equivalent - the same reason
//	presets have to be found by scanning folders rather than enumerated. The
//	undo group is opened here because a preset can add half a dozen effects and
//	keyframes, and a user who does not like the result wants ONE undo.
static void
ApplyPreset(AEGP_SuiteHandler &suites, const char *pathZ)
{
	char fwd[PIEFX_TEXT_MAX];
	char code[PIEFX_TEXT_MAX + 640];

	strcpy_s(fwd, sizeof(fwd), pathZ);
	for (char *p = fwd; *p; p++) {
		if (*p == '\\') { *p = '/'; }
	}

	//	The single-layer rule is the same one the search window is gated on, and
	//	it is restated here because this executor can also be reached from a
	//	bound hexagon, where nothing checked it on the way in.
	sprintf_s(code, sizeof(code),
		"(function(){"
		"var c=app.project.activeItem;"
		"if(!(c&&c instanceof CompItem))return'pieFX: no comp is active';"
		"var s=c.selectedLayers;"
		"if(!s||s.length===0)return'pieFX: select a layer first';"
		"if(s.length>1)return'pieFX: select a single layer';"
		"var f=new File(\"%s\");"
		"if(!f.exists)return'pieFX: preset not found';"
		"app.beginUndoGroup('pieFX: Apply Preset');"
		"try{s[0].applyPreset(f);}finally{app.endUndoGroup();}"
		"return'ok';"
		"})()", fwd);

	S_last_result[0] = 0;
	RunScript(suites, code, "preset");

	if (0 != strcmp(S_last_result, "ok") && !strncmp(S_last_result, "pieFX: ", 7)) {
		SendToast("error", S_last_result + 7);
	}
}

static void
ExecuteAction(AEGP_SuiteHandler &suites, const PieAction *aP)
{
	//	ERR2 assigns into BOTH err2 and err, so both must be in scope.
	A_Err	err		= A_Err_NONE,
			err2	= A_Err_NONE;

	switch (aP->kind) {
		case PK_AE_CMD: {
			//	app.executeCommand, NOT AEGP_DoCommand. Measured: every id fired
			//	through DoCommand from a MENU command worked, and every id fired
			//	through it from HERE - the idle hook, where the gesture lands -
			//	silently did nothing while still returning A_Err_NONE. That is the
			//	S2B finding again: UpdateMenuHook fires when AE REBUILDS ITS MENUS,
			//	so enable-state is stale during idle and the command is dropped.
			//
			//	A NAME is preferred over an id when the binding carries one.
			//	findMenuCommandId resolves it against the running AE, so the binding
			//	is checkable the moment it is made and cannot silently rot into some
			//	other command the way a stale id does. Names are localised, so the
			//	id stays as the fallback.
			//
			//	It measures itself either way: a command that returns cleanly and
			//	changes nothing is the failure that cost two sessions.
			char code[1024];
			char what[96];

			if (aP->text[0]) {
				sprintf_s(code, sizeof(code),
					"(function(){"
					"  var id = app.findMenuCommandId('%s');"
					"  if (!id) { return 'NO SUCH MENU COMMAND'; }"
					"  var c = app.project.activeItem;"
					"  var isC = (c && c instanceof CompItem);"
					"  var L0 = isC ? c.numLayers : -1;"
					"  var R0 = app.project.renderQueue.numItems;"
					"  app.executeCommand(id);"
					"  var L1 = isC ? c.numLayers : -1;"
					"  var R1 = app.project.renderQueue.numItems;"
					"  return 'id ' + id + ', layers ' + L0 + '->' + L1 + ', rq ' + R0 + '->' + R1;"
					"})()", aP->text);
				sprintf_s(what, sizeof(what), "ae-command '%s'", aP->text);
			} else {
				sprintf_s(code, sizeof(code),
					"(function(){"
					"  var c = app.project.activeItem;"
					"  var isC = (c && c instanceof CompItem);"
					"  var L0 = isC ? c.numLayers : -1;"
					"  var R0 = app.project.renderQueue.numItems;"
					"  app.executeCommand(%ld);"
					"  var L1 = isC ? c.numLayers : -1;"
					"  var R1 = app.project.renderQueue.numItems;"
					"  return 'layers ' + L0 + '->' + L1 + ', rq ' + R0 + '->' + R1;"
					"})()", aP->id);
				sprintf_s(what, sizeof(what), "ae-command %ld", aP->id);
			}

			RunScript(suites, code, what);
			if (0 == strcmp(S_last_result, "NO SUCH MENU COMMAND")) {
				char t[200];
				sprintf_s(t, sizeof(t), "No menu command named \"%s\"", aP->text);
				SendToast("error", t);
			}
		} break;

		case PK_SNIPPET:
			RunScript(suites, aP->text, "snippet");
			break;

		case PK_FILE: {
			//	Forward slashes: ExtendScript accepts them, and it removes the
			//	backslash-escaping question from the generated source entirely.
			char path[PIEFX_TEXT_MAX];
			char code[PIEFX_TEXT_MAX + 64];

			strcpy_s(path, sizeof(path), aP->text);
			for (char *p = path; *p; p++) {
				if (*p == '\\') {
					*p = '/';
				}
			}
			sprintf_s(code, sizeof(code), "$.evalFile(\"%s\")", path);
			RunScript(suites, code, "script-file");
		} break;

		case PK_EFFECT:
			ApplyEffectByMatchName(suites, aP->text);
			break;

		case PK_ANCHOR:
			if (aP->cell >= 0) {
				RunAnchorAction(suites, aP->cell);
			}
			break;

		case PK_COPY_FRAME:
			CopyFrameToClipboard(suites);
			break;

		case PK_PRESET:
			ApplyPreset(suites, aP->text);
			break;

		//	NOT a silent break. A kind that reaches here is one the wire parser
		//	accepted and this switch forgot - which is exactly what happened to
		//	copy-frame: parsed, queued, and dropped, so the gesture fired and
		//	absolutely nothing happened, no toast and no log line to say why.
		//	The rule this project keeps re-learning is that silence is the worst
		//	answer, and the default arm of a dispatch switch is where it hides.
		default: {
			char t[120];

			sprintf_s(t, sizeof(t), "pieFX: action kind %d has no executor", (int)aP->kind);
			Log("  %s\n", t);
			SendToast("error", t);
		} break;
	}
}

// ---------------------------------------------------------------------------
//	self-test
//
//	Four of the five executor kinds could be reached by the wheel but had never
//	actually been run inside AE - they were code, not facts. This fires one of
//	each in sequence so a single click settles all of them, and reports what
//	happened rather than leaving it in a log nobody opens.
//
//	Everything it does is visible and one undo away. It needs exactly one
//	selected layer, which is also what the ae-command and effect probes need.
// ---------------------------------------------------------------------------
static A_Boolean
SelfTestScriptFile(char *pathZ, size_t path_max)
{
	//	Written here rather than shipped, so the probe covers the whole path:
	//	temp resolution, the backslash-to-forward-slash normalisation, and
	//	$.evalFile actually finding and running the file.
	if (!GetTempPathA((DWORD)path_max, pathZ)) {
		return FALSE;
	}
	if (strcat_s(pathZ, path_max, "piefx_selftest.jsx")) {
		return FALSE;
	}

	FILE *fp = NULL;
	if (fopen_s(&fp, pathZ, "w") || !fp) {
		return FALSE;
	}
	fprintf(fp, "$.global.__piefx_selftest_ran = true;\n");
	fclose(fp);
	return TRUE;
}

static void
RunSelfTest(AEGP_SuiteHandler &suites)
{
	char		summary[1024]	= { 0 };
	char		line[256];
	AEGP_LayerH	layerH			= NULL;

	Log("\n=== self-test ===\n");
	suites.LayerSuite9()->AEGP_GetActiveLayer(&layerH);

	Append(summary, sizeof(summary),
		layerH ? "One layer selected - all six probes can run.\n\n"
			   : "NO single layer selected: the ae-command, effect and anchor\n"
				 "probes will report failure for that reason alone.\n\n");

	PieAction a;

	//	1. script-snippet - the only kind already proven live, included so the
	//	   test has a known-good control. A failure here means the harness is
	//	   wrong, not the executor.
	ZeroMemory(&a, sizeof(a));
	a.kind = PK_SNIPPET;
	strcpy_s(a.text, sizeof(a.text), "(function(){ return 'snippet ok'; })()");
	ExecuteAction(suites, &a);
	Append(summary, sizeof(summary), "1. script-snippet  -> see log ('snippet ok')\n");

	//	2. script-file - never run before.
	ZeroMemory(&a, sizeof(a));
	a.kind = PK_FILE;
	if (SelfTestScriptFile(a.text, sizeof(a.text))) {
		ExecuteAction(suites, &a);

		//	Ask AE whether the file really executed, rather than trusting that
		//	evalFile returning quietly means anything.
		PieAction check;
		ZeroMemory(&check, sizeof(check));
		check.kind = PK_SNIPPET;
		strcpy_s(check.text, sizeof(check.text),
			"(function(){ return $.global.__piefx_selftest_ran ? "
			"'script-file ok' : 'script-file DID NOT RUN'; })()");
		ExecuteAction(suites, &check);
		Append(summary, sizeof(summary), "2. script-file     -> see log ('script-file ok')\n");
	} else {
		Append(summary, sizeof(summary), "2. script-file     -> SKIPPED (no temp path)\n");
	}

	//	3. ae-command - the biggest unknown, because the id map was hand-tested
	//	   by someone else against a different AE version.
	ZeroMemory(&a, sizeof(a));
	a.kind	= PK_AE_CMD;
	a.id	= PIEFX_TEST_COMMAND;
	ExecuteAction(suites, &a);
	sprintf_s(line, sizeof(line),
		"3. ae-command %d  -> did the layer CENTRE IN VIEW?\n", PIEFX_TEST_COMMAND);
	Append(summary, sizeof(summary), line);

	//	4. effect by match name - proven in the S5 spike, never in the product.
	ZeroMemory(&a, sizeof(a));
	a.kind = PK_EFFECT;
	strcpy_s(a.text, sizeof(a.text), PIEFX_TEST_EFFECT);
	ExecuteAction(suites, &a);
	Append(summary, sizeof(summary), "4. effect          -> is there a Gaussian Blur on the layer?\n");

	//	5. builtin anchor - worked through the OLD native hit-test path, never
	//	   through the overlay's action path this now shares.
	ZeroMemory(&a, sizeof(a));
	a.kind = PK_ANCHOR;
	a.cell = 4;						// centre
	ExecuteAction(suites, &a);
	Append(summary, sizeof(summary), "5. anchor-grid     -> did the anchor snap to the layer centre?\n");

	//	6. builtin copy-frame - the only executor that leaves After Effects
	//	   entirely. The toast reports success, but the only real check is a
	//	   paste, so the summary asks for one.
	ZeroMemory(&a, sizeof(a));
	a.kind = PK_COPY_FRAME;
	ExecuteAction(suites, &a);
	Append(summary, sizeof(summary),
		"6. copy-frame      -> paste into Photoshop or Paint: is it this frame?\n");

	Append(summary, sizeof(summary),
		"\nJudge 3, 4, 5 and 6 BY EYE - the log records what was attempted, not\n"
		"what you saw. Undo a few times to put everything back.\n\nLog: ");
	Append(summary, sizeof(summary), S_log_path);

	Log("=== self-test done ===\n");
	WriteReport(suites, "pieFX_selftest.txt", summary,
		"pieFX: executor self-test done. Judge 3, 4, 5 and 6 by eye, then undo.");
}

// ---------------------------------------------------------------------------
//	AE command probe
//
//	The executor self-test showed ae-command 3819 working (the layer really did
//	centre) while 2279 and 2767 returned SUCCESS and did nothing visible. That is
//	the silent-wrong-id case the command map was always going to produce, but it
//	has two possible causes and they need different fixes:
//
//	  a) the ids are wrong for AE 2026 (the map was hand-tested on 2025), or
//	  b) the ids are right but a Layer > New command will not run in the state the
//	     gesture leaves AE in - note 3819 fired from a MENU command while 2279 and
//	     2767 fired from the gesture path.
//
//	This probe fires them from a menu command. If layers appear here but not from
//	the wheel, it is (b). If nothing appears either way, it is (a).
//
//	It COUNTS THE LAYERS before and after rather than asking whether DoCommand
//	returned an error, because that return value is exactly what lied last time.
// ---------------------------------------------------------------------------
static A_long
CountCompLayers(AEGP_SuiteHandler &suites)
{
	A_Err		err		= A_Err_NONE;
	AEGP_ItemH	itemH	= NULL;
	AEGP_CompH	compH	= NULL;
	A_long		n		= -1;

	ERR(suites.ItemSuite6()->AEGP_GetActiveItem(&itemH));

	if (err || !itemH) {
		return -1;
	}
	ERR(suites.CompSuite11()->AEGP_GetCompFromItem(itemH, &compH));

	if (err || !compH) {
		return -1;
	}
	ERR(suites.LayerSuite9()->AEGP_GetCompNumLayers(compH, &n));
	return err ? -1 : n;
}

//	useScript picks the dispatch path, because THAT is the variable that turned
//	out to matter - not the id. AEGP_DoCommand works from a menu command and
//	silently does nothing from the idle hook, where the gesture lands.
static void
ProbeCommand(AEGP_SuiteHandler &suites, long id, const char *nameZ,
			 A_Boolean useScript, char *summaryZ, size_t summary_max)
{
	A_Err	err2	= A_Err_NONE, err = A_Err_NONE;
	A_long	before	= CountCompLayers(suites);

	if (useScript) {
		char code[160];
		sprintf_s(code, sizeof(code), "app.executeCommand(%ld)", id);
		RunScript(suites, code, "probe");
	} else {
		ERR2(suites.CommandSuite1()->AEGP_DoCommand((AEGP_Command)id));
	}

	A_long		after	= CountCompLayers(suites);
	const char	*howZ	= useScript ? "executeCommand" : "DoCommand    ";
	char		line[220];

	if (before < 0 || after < 0) {
		sprintf_s(line, sizeof(line), "  %-15s %4ld %s : NO ACTIVE COMP\n", nameZ, id, howZ);
	} else if (after > before) {
		sprintf_s(line, sizeof(line), "  %-15s %4ld %s : WORKS (%ld -> %ld)\n",
				  nameZ, (long)id, howZ, (long)before, (long)after);
	} else {
		sprintf_s(line, sizeof(line), "  %-15s %4ld %s : DID NOTHING (%ld, err %d)\n",
				  nameZ, (long)id, howZ, (long)before, (int)err2);
	}
	Log("%s", line);
	Append(summaryZ, summary_max, line);
}

static void
ResolveMenuNames(AEGP_SuiteHandler &suites, char *summaryZ, size_t summary_max)
{
	//	app.findMenuCommandId turns a MENU NAME into an id, which means a
	//	binding can be validated at the moment it is made: a name that does not
	//	resolve comes back 0. Ids never offered that, and the hand-tested map
	//	has already produced three wrong entries and two duplicate names.
	//
	//	This asks AE which spellings actually exist on THIS install, so the
	//	defaults are copied from a measurement instead of guessed again.
	const char *codeZ =
		"(function(){"
		"  var n = ["
		"    'Precompose...',"
		"    'Pre-compose...',"
		"    'Precompose',"
		"    'Add to Render Queue',"
		"    'Add To Render Queue',"
		"    'Null Object',"
		"    'New Null Object',"
		"    'Adjustment Layer',"
		"    'New Adjustment Layer',"
		"    'Solid...',"
		"    'New Solid...',"
		"    'Text',"
		"    'Light...',"
		"    'Camera...',"
		"    'Duplicate',"
		"    'Split Layer',"
		"    'Center Anchor Point in Layer Content',"
		"    'Save Frame As...',"
		"    'Save Frame As',"
		"    'File...',"
		"    'Photoshop Layers...',"
		"    'Center in View',"
		"    'Center In View',"
		"    'Center in view',"
		"    'Fit to Comp',"
		"    'Solid Settings...',"
		"    'New Composition...',"
		"    'New Composition',"
		"    'Composition...',"
		"    'Composition Settings...'"
		"  ];"
		"  var o = [];"
		"  for (var i = 0; i < n.length; i++) {"
		"    var id = 0;"
		"    try { id = app.findMenuCommandId(n[i]); } catch (e) { id = -1; }"
		"    o.push((id ? id : '  --') + '  ' + n[i]);"
		"  }"
		"  return o.join(String.fromCharCode(10));"
		"})()";

	S_last_result[0] = 0;
	RunScript(suites, codeZ, "menu-name lookup");
	Append(summaryZ, summary_max, S_last_result);
}
static void
RunCommandProbe(AEGP_SuiteHandler &suites)
{
	char summary[32768] = { 0 };

	Log("\n=== ae-command probe ===\n");
	Append(summary, sizeof(summary),
		"Each id fired BOTH ways from a menu command, counting the comp's\n"
		"layers around each call. The wheel itself now uses executeCommand,\n"
		"because DoCommand works here and does nothing from the idle hook.\n\n");


	Append(summary, sizeof(summary), "Menu names AE resolves on THIS install:\n");
	ResolveMenuNames(suites, summary, sizeof(summary));
	Append(summary, sizeof(summary), "\n\nId dispatch comparison:\n");

	ProbeCommand(suites, PIEFX_PROBE_NULL,  "Null Object",   FALSE, summary, sizeof(summary));
	ProbeCommand(suites, PIEFX_PROBE_NULL,  "Null Object",   TRUE,  summary, sizeof(summary));
	ProbeCommand(suites, PIEFX_PROBE_ADJ_A, "Adjustment(A)", FALSE, summary, sizeof(summary));
	ProbeCommand(suites, PIEFX_PROBE_ADJ_A, "Adjustment(A)", TRUE,  summary, sizeof(summary));
	ProbeCommand(suites, PIEFX_PROBE_ADJ_B, "Adjustment(B)", TRUE,  summary, sizeof(summary));

	Append(summary, sizeof(summary), "\nUndo three times to clean up.");
	Log("=== ae-command probe done ===\n");
	WriteReport(suites, "pieFX_probe.txt", summary,
		"pieFX: AE Commands probe done. Undo three times to clean up.");
}

// ---------------------------------------------------------------------------
//	AEGP hooks
// ---------------------------------------------------------------------------
static A_Err
IdleHook(AEGP_GlobalRefcon, AEGP_IdleRefcon, A_long *max_sleepPL)
{
	AEGP_SuiteHandler suites(sP);

	//	Arm without being asked, once, on the first idle after AE has finished
	//	coming up. NOT from EntryPointFunc: the mouse hook binds to the calling
	//	thread and the overlay is a process launch, and neither belongs in the
	//	middle of AE loading its plug-ins. Idle is guaranteed to be AE's UI
	//	thread and guaranteed to be after startup, which is exactly the two
	//	things arming needs.
	//
	//	It runs silently. A modal 'pieFX: ON' on every launch would be the
	//	feature announcing itself forever; the wheel appearing on the first
	//	right-hold is the confirmation.
	if (!S_launch_armed) {
		S_launch_armed = TRUE;
		if (!S_log_path[0])   { ResolveLogPath(); }
		if (!S_settings_read) { ReadSettings(); }
		if (S_arm_on_launch) {
			Log("  armOnLaunch: arming without being asked\n");
			Arm(suites, FALSE);
		} else {
			Log("  armOnLaunch off; waiting for the menu item\n");
		}
	}

	//	The effects catalogue, once, on an idle AFTER arming rather than inside
	//	it. Walking 519 entries is not free and arming already launches a
	//	process and installs a hook; an idle later costs the user nothing and
	//	still lands long before the first right-hold. Once per session, because
	//	the installed set does not change while AE is running.
	if (S_active && !S_effects_dumped) {
		S_effects_dumped = TRUE;
		WriteEffectCatalogue(suites);
	}

	//	Backstop for a press whose UP we never saw. The thread-local WH_MOUSE hook
	//	only sees AE's thread, so a right-release over a NON-AE window is invisible
	//	to us: S_rdownB stays set and the wheel is left showing. Idle is starved
	//	during the modal press loop, so this can only evaluate once the press has
	//	actually ended - at which point, if we still think the button is down but
	//	it is physically up, the release happened off-AE. Treat it as a cancel.
#ifdef AE_OS_WIN
	if (S_active && S_rdownB && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {
		CancelHoldTimer();
		if (S_hold_firedB) {
			SendCancel();
		}
		S_rdownB	  = FALSE;
		S_hold_firedB = FALSE;
		Log("  backstop: right-up unseen (released off-AE) -> cancel, wheel hidden\n");
	}
#else
	//	Same backstop, same reason: a LOCAL NSEvent monitor has the identical
	//	blind spot, and asks the HID layer the same question through
	//	CGEventSourceButtonState. The press state lives in the module, so the
	//	check does too.
	if (S_active) {
		PieFX_GesturePoll();
	}
#endif

	//	Keep selection context warm while armed (cheap; only when not dragging).
#ifdef AE_OS_WIN
	if (S_active && !S_rdownB) {
#else
	if (S_active && !PieFX_GestureBusy()) {
#endif
		RefreshSelectionContext(suites);
	}

	//	Drain actions the overlay sent back. They arrive on the pipe thread and
	//	are executed HERE, on AE's UI thread, for the same reason the anchor move
	//	is deferred: no AEGP call may run off the UI thread or inside the hook.
	{
		PieAction a;

		while (QueuePop(&a)) {
			ExecuteAction(suites, &a);
		}
	}

	if (max_sleepPL && S_active) {
		*max_sleepPL = 30;	// stay responsive while armed
	}
	return A_Err_NONE;
}

//	Take the overlay down with us. Three mechanisms, because the first two were
//	measured to be insufficient: the death hook fired and terminated the overlay
//	and AE still would not finish quitting.
//
//	  1. the job object, which kills the whole process TREE - the overlay and
//	     the WebView2 children it spawns - and does it on handle close, so it
//	     works whether or not this code ever runs;
//	  2. this terminate, which is the orderly route when the hook does run;
//	  3. the overlay's own --owner-pid watchdog, for a crash that reaches
//	     neither of the above.
static void
StopOverlay(void)
{
#ifdef AE_OS_WIN
	if (S_overlay_proc) {
		if (WaitForSingleObject(S_overlay_proc, 0) == WAIT_TIMEOUT) {
			BOOL  ok = TerminateProcess(S_overlay_proc, 0);
			DWORD w  = WaitForSingleObject(S_overlay_proc, 2000);
			//	Both halves get reported. Last time the terminate was logged as
			//	if it had worked, and the process was still in Task Manager -
			//	which cost a round trip to find out.
			Log("  overlay: terminate ok=%d, then %s\n", (int)ok,
				w == WAIT_OBJECT_0 ? "gone" : "STILL UP (un-dead: a pending I/O)");
		}
		CloseHandle(S_overlay_proc);
		S_overlay_proc = NULL;
	}

	//	Unconditional, and NOT inside the branch above: terminating the overlay
	//	leaves its WebView2 children running, and closing the last job handle is
	//	what takes those with it.
	if (S_overlay_job) {
		CloseHandle(S_overlay_job);
		S_overlay_job = NULL;
		Log("  overlay: job closed (takes the WebView2 children with it)\n");
	}
#else
	//	ONE call, because the process GROUP is what the job object was for: the
	//	kill reaches the WebKit children too, so there is no separate "and now
	//	the children" step. It escalates SIGTERM -> SIGKILL rather than going
	//	straight to force.
	PieFX_EndOverlay(2000);
#endif
}

static A_Err
DeathHook(AEGP_GlobalRefcon, AEGP_DeathRefcon)
{
	Log("=== death hook ===\n");
	CancelHoldTimer();
#ifdef AE_OS_WIN
	if (S_mouse_hook) { UnhookWindowsHookEx(S_mouse_hook); S_mouse_hook = NULL; }
#else
	PieFX_DisarmGesture();
#endif
	S_active = FALSE;

	//	ORDER IS THE FIX. Ask first, while the pipe it is reading is still
	//	whole, and give it a moment to go. Tearing the pipes down first is
	//	what left it blocked in a read that no longer had a server, and a
	//	process in that state survives being terminated.
	SendQuit();
#ifdef AE_OS_WIN
	if (S_overlay_proc) {
		DWORD w = WaitForSingleObject(S_overlay_proc, 2000);
		Log("  overlay: asked to quit -> %s\n",
			w == WAIT_OBJECT_0 ? "gone" : "still up after 2s");
	}
#else
	//	Same order, same reason: ask while the FIFO it is reading is still
	//	whole, and give it a moment. StopPipeServer below depends on this
	//	having happened first.
	{
		int waited = 0;

		while (waited < 2000 && PieFX_OverlayAlive()) {
			Sleep(50);
			waited += 50;
		}
		Log("  overlay: asked to quit -> %s\n",
			PieFX_OverlayAlive() ? "still up after 2s" : "gone");
	}
#endif

	StopPipeServer();
	StopOverlay();
	return A_Err_NONE;
}

static A_Err
UpdateMenuHook(AEGP_GlobalRefcon, AEGP_UpdateMenuRefcon, AEGP_WindowType)
{
	AEGP_SuiteHandler suites(sP);
	//	Always available - arming is a global mode, not a per-comp action.
	suites.CommandSuite1()->AEGP_EnableCommand(S_toggle_cmd);
	suites.CommandSuite1()->AEGP_EnableCommand(S_settings_cmd);
#if PIEFX_SHOW_SELFTESTS
	suites.CommandSuite1()->AEGP_EnableCommand(S_selftest_cmd);
	suites.CommandSuite1()->AEGP_EnableCommand(S_cmdprobe_cmd);
#endif
	return A_Err_NONE;
}

static void
ResolveLogPath(void)
{
	DWORD len = GetTempPathA(MAX_PATH, S_log_path);
	if (!len || len > MAX_PATH - 24) { S_log_path[0] = 0; return; }
	strcat_s(S_log_path, MAX_PATH, "pieFX_poc.txt");
}

//	The two settings the plug-in has to know before the overlay exists.
//
//	Hand-scanned, not parsed. The rest of settings.json is the overlay's - it
//	owns the slot tree and writes the file - and a real JSON parser here would
//	be a second implementation of a format that already has one. What is needed
//	is two scalars out of a file we wrote ourselves, and a scan for the key
//	followed by the next token does exactly that without pretending to be
//	general.
//
//	A missing or unreadable file is not an error: it is a machine where the
//	settings window has never been opened, and the shipped defaults are right.
static const char *
FindKey(const char *bufZ, const char *keyZ)
{
	const char *p = strstr(bufZ, keyZ);

	if (!p) { return NULL; }
	p += strlen(keyZ);
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; }
	if (*p != ':') { return NULL; }
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; }
	return p;
}

#ifdef AE_OS_WIN
static void
ReadSettings(void)
{
	char	path[MAX_PATH];
	DWORD	len;
	FILE	*fp = NULL;
	char	buf[8192];
	size_t	got;

	S_settings_read = TRUE;

	len = GetEnvironmentVariableA("APPDATA", path, MAX_PATH);
	if (!len || len >= MAX_PATH - (DWORD)strlen(PIEFX_SETTINGS_REL) - 2) {
		Log("  settings: no APPDATA; using defaults\n");
		return;
	}
	strcat_s(path, MAX_PATH, "\\");
	strcat_s(path, MAX_PATH, PIEFX_SETTINGS_REL);

	if (fopen_s(&fp, path, "rb") || !fp) {
		Log("  settings: none at %s; defaults (armOnLaunch=on, %ums)\n",
			path, S_hold_ms);
		return;
	}
	got = fread(buf, 1, sizeof(buf) - 1, fp);
	fclose(fp);
	buf[got] = 0;

	{
		const char *v = FindKey(buf, "\"armOnLaunch\"");

		if (v) { S_arm_on_launch = (strncmp(v, "false", 5) != 0); }
	}
	{
		const char *v = FindKey(buf, "\"holdMs\"");

		if (v) {
			int ms = atoi(v);

			//	Clamped rather than trusted. A zero here would summon the wheel
			//	on every right-click and take AE's context menu away entirely -
			//	a setting nobody could undo without editing the file by hand.
			if (ms < 80)   { ms = 80; }
			if (ms > 2000) { ms = 2000; }
			S_hold_ms = (UINT)ms;
		}
	}
	Log("  settings: %s -> armOnLaunch=%s holdMs=%u\n",
		path, S_arm_on_launch ? "true" : "false", S_hold_ms);
}

#else	// AE_OS_WIN

//	MAC_PORT.md step 5. %APPDATA% has a documented macOS answer
//	(~/Library/Application Support/pieFX), but PIEFX_SETTINGS_REL is a
//	BACKSLASH path and the OVERLAY writes the same file from its own side — so
//	the two ends have to agree, and that is one decision to take rather than
//	half a decision to take here.
//
//	Falling through to the built-in defaults is safe: they are the same
//	defaults the overlay uses under --settings none, which is how every
//	harness in this project already runs.
static void
ReadSettings(void)
{
	S_settings_read = TRUE;
	Log("  settings: not read on macOS yet (MAC_PORT step 5); using defaults\n");
}

#endif	// AE_OS_WIN


static void
Disarm(AEGP_SuiteHandler &suites, A_Boolean announce)
{
	S_active = FALSE;
	CancelHoldTimer();
#ifdef AE_OS_WIN
	if (S_mouse_hook) { UnhookWindowsHookEx(S_mouse_hook); S_mouse_hook = NULL; }
#else
	PieFX_DisarmGesture();
#endif
	SendCancel();

	//	Same order as the death hook, for the same reason: ask the overlay to
	//	quit while the pipe it is reading is still whole, THEN tear the pipes
	//	down. Disarm used to leave the overlay running against pipes that had
	//	just been closed, so it sat there un-dead - and that wreckage is what
	//	AE's next write hung on. Arm() launches a fresh one, so turning pieFX
	//	back on costs nothing.
	SendQuit();
#ifdef AE_OS_WIN
	if (S_overlay_proc) {
		DWORD w = WaitForSingleObject(S_overlay_proc, 2000);
		Log("  overlay: asked to quit -> %s\n",
			w == WAIT_OBJECT_0 ? "gone" : "still up after 2s");
	}
#else
	//	Same order, same reason: ask while the FIFO it is reading is still
	//	whole, and give it a moment. StopPipeServer below depends on this
	//	having happened first.
	{
		int waited = 0;

		while (waited < 2000 && PieFX_OverlayAlive()) {
			Sleep(50);
			waited += 50;
		}
		Log("  overlay: asked to quit -> %s\n",
			PieFX_OverlayAlive() ? "still up after 2s" : "gone");
	}
#endif
	StopPipeServer();
	StopOverlay();
	Log("=== pieFX disarmed ===\n");
	if (announce) {
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, "pieFX: OFF.");
	}
}

//	`announce` is the whole reason arming is a function rather than the second
//	half of a toggle. A modal dialog is right when the user has just clicked the
//	menu item and wants to know it took; it is intolerable on every AE launch,
//	which is what auto-arm would otherwise be.
static A_Boolean
Arm(AEGP_SuiteHandler &suites, A_Boolean announce)
{
	if (S_active) { return TRUE; }

	ResolveLogPath();

	FILE *fp = NULL;
	if (!fopen_s(&fp, S_log_path, "w") && fp) {
		fprintf(fp, "pieFX log. Hold %ums. Thread %lu.\n\n",
				S_hold_ms, (unsigned long)GetCurrentThreadId());
		fclose(fp);
	}

	{
		//	Installing the gesture is the one step of arming that can fail, and
		//	it fails differently on each platform. Everything around it — the
		//	log, the pipe server, the overlay — is identical.
		A_Boolean	hooked;
		char		m[160];

#ifdef AE_OS_WIN
		S_mouse_hook = SetWindowsHookEx(WH_MOUSE, MouseProc, NULL, GetCurrentThreadId());
		hooked = (S_mouse_hook != NULL);
		sprintf_s(m, sizeof(m), "pieFX: SetWindowsHookEx failed, err=%lu",
				  (unsigned long)GetLastError());
#else
		PieFXGestureCallbacks cb;

		cb.hold		= MacOnHold;
		cb.move		= MacOnMove;
		cb.release	= MacOnRelease;
		cb.user		= NULL;
		hooked = PieFX_ArmGesture(&cb, PieFXLogBridge, NULL) ? TRUE : FALSE;
		sprintf_s(m, sizeof(m), "pieFX: could not install the event monitor");
#endif
		if (!hooked) {
			Log("  %s\n", m);
			//	A silent auto-arm that fails still has to be reachable: it
			//	leaves S_active FALSE, so the menu item is the way to try again
			//	AND to see why. A modal error thrown during AE's startup is not.
			if (announce) { suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, m); }
			return FALSE;
		}
	}

	StartPipeServer();
	LaunchOverlay();

	S_active = TRUE;
#ifdef AE_OS_WIN
	S_rdownB = FALSE;
	S_hold_firedB = FALSE;
	S_replay_pending = 0;
#endif
	Log("=== pieFX armed (hold %ums) ===\n", S_hold_ms);

	if (!announce) { return TRUE; }

	char msg[MAX_PATH + 320];
	sprintf_s(msg, sizeof(msg),
		"pieFX: ON.\n\n"
		"Right-press and hold past %ums: the wheel appears under the cursor.\n"
		"Flick to a hexagon and release to fire it. A quick right-click still\n"
		"opens AE's normal menu.\n\n"
		"Window > pieFX Settings edits the wheel.\n\nLog: %s",
		S_hold_ms, S_log_path);
	suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
	return TRUE;
}

static void
ToggleActive(AEGP_SuiteHandler &suites)
{
	if (S_active) { Disarm(suites, TRUE); }
	else          { Arm(suites, TRUE); }
}

//	The settings window needs the overlay to exist, and the overlay only exists
//	while pieFX is armed. So this arms first if it has to - which is also the
//	honest reading of "open the settings for the thing", and it means the menu
//	item never appears to do nothing.
static void
OpenSettings(AEGP_SuiteHandler &suites)
{
	if (!S_active && !Arm(suites, FALSE)) {
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id,
			"pieFX could not start, so there is nothing to configure. "
			"The log says why: %TEMP%\\pieFX_poc.txt");
		return;
	}
	Log("  settings window requested\n");
	SendSettings();
}

static A_Err
CommandHook(AEGP_GlobalRefcon, AEGP_CommandRefcon, AEGP_Command command,
			AEGP_HookPriority, A_Boolean, A_Boolean *handledPB)
{
	AEGP_SuiteHandler suites(sP);
	if (command == S_toggle_cmd) {
		ToggleActive(suites);
		*handledPB = TRUE;
	} else if (command == S_settings_cmd) {
		if (!S_log_path[0]) {
			ResolveLogPath();
		}
		OpenSettings(suites);
		*handledPB = TRUE;
#if PIEFX_SHOW_SELFTESTS
	} else if (command == S_cmdprobe_cmd) {
		if (!S_log_path[0]) {
			ResolveLogPath();
		}
		RunCommandProbe(suites);
		*handledPB = TRUE;
	} else if (command == S_selftest_cmd) {
		if (!S_log_path[0]) {
			ResolveLogPath();
		}
		RunSelfTest(suites);
		*handledPB = TRUE;
#endif
	}
	return A_Err_NONE;
}

// ---------------------------------------------------------------------------
//	entry point (FIVE params - see the header note)
// ---------------------------------------------------------------------------
DllExport A_Err
EntryPointFunc(
	struct SPBasicSuite		*pica_basicP,
	A_long					major_versionL,
	A_long					minor_versionL,
	AEGP_PluginID			aegp_plugin_id,
	AEGP_GlobalRefcon		*global_refconP)
{
	A_Err err = A_Err_NONE, err2 = A_Err_NONE;

	sP		= pica_basicP;
	S_my_id	= aegp_plugin_id;

	InitializeCriticalSection(&S_pipe_cs);
	S_pipe_cs_ready = TRUE;

	AEGP_SuiteHandler suites(pica_basicP);

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_toggle_cmd));
	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(S_toggle_cmd, PIEFX_MENU_NAME,
														AEGP_Menu_WINDOW, AEGP_MENU_INSERT_SORTED));

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_settings_cmd));
	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(S_settings_cmd, PIEFX_SETTINGS_NAME,
														AEGP_Menu_WINDOW, AEGP_MENU_INSERT_SORTED));

	//	The self-tests are developer instruments. They stay compiled - flip
	//	PIEFX_SHOW_SELFTESTS in the header to get them back - but a user's
	//	Window menu is not where they belong.
#if PIEFX_SHOW_SELFTESTS
	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_cmdprobe_cmd));
	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(S_cmdprobe_cmd, PIEFX_CMDPROBE_NAME,
														AEGP_Menu_WINDOW, AEGP_MENU_INSERT_SORTED));

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_selftest_cmd));
	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(S_selftest_cmd, PIEFX_SELFTEST_NAME,
														AEGP_Menu_WINDOW, AEGP_MENU_INSERT_SORTED));
#endif

	ERR(suites.RegisterSuite5()->AEGP_RegisterCommandHook(S_my_id, AEGP_HP_BeforeAE,
														AEGP_Command_ALL, CommandHook, 0));
	ERR(suites.RegisterSuite5()->AEGP_RegisterUpdateMenuHook(S_my_id, UpdateMenuHook, NULL));
	ERR(suites.RegisterSuite5()->AEGP_RegisterIdleHook(S_my_id, IdleHook, NULL));
	ERR(suites.RegisterSuite5()->AEGP_RegisterDeathHook(S_my_id, DeathHook, NULL));

	if (err) {
		ERR2(suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, "pieFX POC failed to register."));
	}
	return err;
}
