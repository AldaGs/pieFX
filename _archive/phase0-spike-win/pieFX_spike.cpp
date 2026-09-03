/*
	pieFX.cpp - Phase 0 capability spikes.

	S1  "AEGP hello world" - PASSED. One menu command that runs ExtendScript to
	    snap the selected layers' anchor points to their source-rect centre.
	    Proves the entry point, the command/menu hooks, and AEGP_ExecuteScript
	    as a dispatch path.

	S2A "which window?" - the measurement half of S2. See the block comment
	    above the probe.

	Deliberately NOT here: any overlay window (S3), the effect catalogue (S5).

	Note on the SDK samples: Commando registers its hooks with S_my_id, which
	it never assigns - so it registers everything under plugin id 0. We store
	aegp_plugin_id first and use it everywhere, including ExecuteScript, which
	needs a real id.
*/

#include "pieFX_spike.h"

static AEGP_Command		S_anchor_cmd	= 0L;
static AEGP_Command		S_probe_cmd		= 0L;
static AEGP_Command		S_watch_cmd		= 0L;
static AEGP_Command		S_swallow_cmd	= 0L;
static AEGP_Command		S_overlay_cmd	= 0L;
static AEGP_Command		S_overlay2_cmd	= 0L;
static AEGP_Command		S_fxdump_cmd	= 0L;
static AEGP_Command		S_fxapply_cmd	= 0L;
static AEGP_PluginID	S_my_id			= 0L;
static SPBasicSuite		*sP				= NULL;

//	ES3 only: no let, no arrow functions, no JSON.
static const A_char *S_anchor_script =
	"(function(){"
	"  var c = app.project.activeItem;"
	"  if (!(c instanceof CompItem)) { return 'no active comp'; }"
	"  var sel = c.selectedLayers;"
	"  if (sel.length === 0) { return 'no layer selected'; }"
	"  app.beginUndoGroup('Radial Menu S1: Anchor to Center');"
	"  var moved = 0;"
	"  for (var i = 0; i < sel.length; i++) {"
	"    var L = sel[i];"
	"    var ap = L.property('Anchor Point');"
	"    if (!ap) { continue; }"
	"    var r;"
	"    try { r = L.sourceRectAtTime(c.time, false); } catch (e) { continue; }"
	"    var oldA = ap.value;"
	"    var cx = r.left + r.width / 2;"
	"    var cy = r.top + r.height / 2;"
	"    var dx = cx - oldA[0];"
	"    var dy = cy - oldA[1];"
	// Moving the anchor alone makes the layer jump, because position is
	// measured to the anchor. Push position by the same delta, taken through
	// the layer's own scale and rotation, so the pixels stay put.
	"    var s = L.property('Scale').value;"
	"    var sx = dx * s[0] / 100;"
	"    var sy = dy * s[1] / 100;"
	"    var rot = L.threeDLayer ? L.property('Z Rotation').value : L.property('Rotation').value;"
	"    var th = rot * Math.PI / 180;"
	"    var ct = Math.cos(th), st = Math.sin(th);"
	"    var pdx = sx * ct - sy * st;"
	"    var pdy = sx * st + sy * ct;"
	"    var pos = L.property('Position');"
	"    var p = pos.value;"
	"    if (L.threeDLayer) {"
	"      ap.setValue([cx, cy, oldA[2]]);"
	"      pos.setValue([p[0] + pdx, p[1] + pdy, p[2]]);"
	"    } else {"
	"      ap.setValue([cx, cy]);"
	"      pos.setValue([p[0] + pdx, p[1] + pdy]);"
	"    }"
	"    moved++;"
	"  }"
	"  app.endUndoGroup();"
	"  return 'moved ' + moved + ' layer(s)';"
	"})()";

/* ---------------------------------------------------------------------------
	S2A - "which window is under the cursor?"

	We cannot subclass AE's comp viewer until we know how to FIND it, and the
	roadmap is explicit that hardcoding a class name is what pins competing
	products to one AE version. So step A of S2 is measurement, not code we
	intend to keep: arm a probe, wave the cursor over the viewer, and write down
	every distinct window it passes through together with its full ancestor
	chain. The resolver in S2B gets written against that, not against a guess.

	Sampling happens in the idle hook. That is also a free datum: if the idle
	hook does not fire while the mouse moves over the viewer, we learn something
	important about AE's message pumping before we depend on it.
--------------------------------------------------------------------------- */

#define RM_MAX_SEEN		64

static A_Boolean	S_probe_armedB					= FALSE;
static DWORD		S_probe_end_tick				= 0;
static HWND			S_seen[RM_MAX_SEEN]				= { 0 };
static int			S_seen_count					= 0;
static char			S_probe_path[MAX_PATH]			= { 0 };
static HWND			S_main_hwnd						= NULL;

static A_Boolean
AlreadySeen(HWND hwnd)
{
	for (int i = 0; i < S_seen_count; i++) {
		if (S_seen[i] == hwnd) {
			return TRUE;
		}
	}
	return FALSE;
}

//	One line per window: class, text, screen rect, and how it relates to the
//	windows we already know about.
static void
DescribeWindow(FILE *fp, HWND hwnd, const char *labelZ, int indent)
{
	char	cls[256]	= { 0 };
	char	txt[256]	= { 0 };
	RECT	r			= { 0, 0, 0, 0 };

	GetClassNameA(hwnd, cls, sizeof(cls));
	GetWindowTextA(hwnd, txt, sizeof(txt));
	GetWindowRect(hwnd, &r);

	fprintf(fp, "%*s%-10s hwnd=%p  class=\"%s\"  text=\"%s\"  rect=(%ld,%ld)-(%ld,%ld)  %ldx%ld  %s%s\n",
			indent * 2, "",
			labelZ,
			(void*)hwnd,
			cls,
			txt,
			r.left, r.top, r.right, r.bottom,
			r.right - r.left, r.bottom - r.top,
			IsWindowVisible(hwnd) ? "visible" : "hidden",
			(hwnd == S_main_hwnd) ? "  <-- AE main hwnd" : "");
}

//	The ancestor chain is the part that matters: it tells us whether a stable
//	walk from the main window down to the viewer exists at all.
static void
LogHitWindow(HWND hwnd, POINT pt)
{
	FILE	*fp = NULL;

	if (fopen_s(&fp, S_probe_path, "a") || !fp) {
		return;
	}

	fprintf(fp, "\n=== cursor at (%ld,%ld) ===\n", pt.x, pt.y);
	DescribeWindow(fp, hwnd, "HIT", 0);

	//	Walk up to the top, recording depth as we go.
	HWND	chain[32]	= { 0 };
	int		depth		= 0;
	for (HWND w = hwnd; w && depth < 32; w = GetParent(w)) {
		chain[depth++] = w;
	}

	fprintf(fp, "  ancestors (top-level first):\n");
	for (int i = depth - 1; i >= 0; i--) {
		DescribeWindow(fp, chain[i], "", (depth - i) + 1);
	}

	//	Depth from the main window is what a resolver would have to traverse.
	fprintf(fp, "  depth below AE main: %s\n",
			(depth > 0 && chain[depth - 1] == S_main_hwnd) ? "rooted at main" : "NOT rooted at main (separate top-level)");

	fclose(fp);
}

static void
ProbeSample(void)
{
	POINT	pt = { 0, 0 };

	if (!GetCursorPos(&pt)) {
		return;
	}

	//	WindowFromPoint gives the top-level-ish hit; ChildWindowFromPointEx
	//	drills into the child that actually owns those pixels.
	HWND	hit = WindowFromPoint(pt);

	if (!hit || AlreadySeen(hit)) {
		return;
	}

	if (S_seen_count < RM_MAX_SEEN) {
		S_seen[S_seen_count++] = hit;
	}

	LogHitWindow(hit, pt);
}

static void
ProbeBegin(AEGP_SuiteHandler &suites)
{
	DWORD	len = GetTempPathA(MAX_PATH, S_probe_path);

	if (!len || len > MAX_PATH - 32) {
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, "S2A: could not resolve a temp path.");
		return;
	}
	strcat_s(S_probe_path, MAX_PATH, "pieFX_S2_probe.txt");

	S_seen_count		= 0;
	S_probe_end_tick	= GetTickCount() + (RM_PROBE_SECONDS * 1000);
	S_probe_armedB		= TRUE;

	//	Truncate and write a header, so a stale file is never mistaken for a run.
	FILE *fp = NULL;
	if (!fopen_s(&fp, S_probe_path, "w") && fp) {
		S_main_hwnd = NULL;
		suites.UtilitySuite3()->AEGP_GetMainHWND(&S_main_hwnd);

		fprintf(fp, "Radial Menu - S2A window probe\n");
		fprintf(fp, "Sampling for %d seconds. Move the cursor over the COMP VIEWER now.\n", RM_PROBE_SECONDS);
		fprintf(fp, "Also worth sweeping: timeline, project panel, effect controls, a floating\n");
		fprintf(fp, "panel, and a second monitor - the resolver has to tell them apart.\n\n");
		if (S_main_hwnd) {
			DescribeWindow(fp, S_main_hwnd, "MAIN", 0);
		} else {
			fprintf(fp, "AEGP_GetMainHWND returned nothing.\n");
		}
		fclose(fp);
	}

	char msg[MAX_PATH + 160];
	sprintf_s(msg, sizeof(msg),
		"S2A armed for %d seconds.\n\nDismiss this alert, then sweep the cursor over the comp viewer "
		"and the other panels.\n\nLog: %s",
		RM_PROBE_SECONDS, S_probe_path);
	suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
}

/* ---------------------------------------------------------------------------
	S2B - can we see right-button-held-past-200ms, and does normal right-click
	still work?

	S2A killed the roadmap's plan of subclassing a resolved comp-viewer window:
	every AE panel shares the class "DroverLord - Window Class" and carries only
	a structural role in its window text, so there is nothing to resolve BY.

	A thread-local WH_MOUSE hook sidesteps the whole question. It is installed on
	AE's own UI thread - which we are already running on, being a DLL inside AE -
	so it needs no OS permission, touches no other process, sees mouse messages
	for every AE window regardless of class, and mentions no version-stamped
	class name anywhere. It is also the same argument the roadmap makes for
	macOS local event monitors, which makes S2 and S4 the same shape rather than
	two unrelated problems.

	This spike deliberately NEVER swallows a message: every path ends in
	CallNextHookEx. Detecting the gesture and stealing the gesture are separate
	claims, and "never break normal right-click" is the bug the roadmap says
	would make people uninstall in anger. Prove we can see it first.

	Open question this also measures: AEGP_WindowType is only ever *delivered*
	to the UpdateMenuHook - there is no getter - so we cache it there and log it
	on every press to find out whether that cache actually tracks panel focus.
--------------------------------------------------------------------------- */

static HHOOK			S_mouse_hook			= NULL;
static A_Boolean		S_watch_onB				= FALSE;
static char				S_gesture_path[MAX_PATH]= { 0 };

static UINT_PTR			S_hold_timer			= 0;
static DWORD			S_rdown_tick			= 0;
static POINT			S_rdown_pt				= { 0, 0 };
static A_Boolean		S_rdownB				= FALSE;
static A_Boolean		S_hold_firedB			= FALSE;
static AEGP_WindowType	S_last_wintype			= AEGP_WindType_NONE;
static int				S_hold_count			= 0;
static int				S_click_count			= 0;

static const char *
WinTypeName(AEGP_WindowType t)
{
	switch (t) {
		case AEGP_WindType_NONE:			return "NONE";
		case AEGP_WindType_PROJECT:			return "PROJECT";
		case AEGP_WindType_COMP:			return "COMP";
		case AEGP_WindType_TIME_LAYOUT:		return "TIME_LAYOUT";
		case AEGP_WindType_LAYER:			return "LAYER";
		case AEGP_WindType_FOOTAGE:			return "FOOTAGE";
		case AEGP_WindType_RENDER_QUEUE:	return "RENDER_QUEUE";
		case AEGP_WindType_QT:				return "QT";
		case AEGP_WindType_DIALOG:			return "DIALOG";
		case AEGP_WindType_FLOWCHART:		return "FLOWCHART";
		case AEGP_WindType_EFFECT:			return "EFFECT";
		case AEGP_WindType_OTHER:			return "OTHER";
	}
	return "?";
}

static void
GestureLog(const char *fmtZ, ...)
{
	FILE *fp = NULL;

	if (!S_gesture_path[0] || fopen_s(&fp, S_gesture_path, "a") || !fp) {
		return;
	}

	va_list args;
	va_start(args, fmtZ);
	vfprintf(fp, fmtZ, args);
	va_end(args);

	fclose(fp);
}

//	Describe where a press landed, without pretending we can name the panel.
static void
LogPressTarget(const char *labelZ, POINT pt)
{
	HWND	hit		= WindowFromPoint(pt);
	char	cls[128]= { 0 };
	char	txt[128]= { 0 };

	if (hit) {
		GetClassNameA(hit, cls, sizeof(cls));
		GetWindowTextA(hit, txt, sizeof(txt));
	}

	GestureLog("%s at (%ld,%ld)  hwnd=%p  class=\"%s\"  text=\"%s\"  AE says active panel = %s\n",
				labelZ, pt.x, pt.y, (void*)hit, cls, txt, WinTypeName(S_last_wintype));
}

//	Fires once per press, the moment the hold threshold is crossed. sourceZ
//	records WHICH clock got there first, which is the point of this revision.
static void
OnHoldDetected(const char *sourceZ)
{
	S_hold_firedB = TRUE;
	S_hold_count++;

	//	Audible, non-focus-stealing, and it cannot itself disturb the very input
	//	we are trying to measure - which an alert or a window would.
	MessageBeep(MB_OK);

	GestureLog("  HOLD detected after %lums (hold #%d, via %s)\n",
				(unsigned long)(GetTickCount() - S_rdown_tick), S_hold_count, sourceZ);
}

//	The clock the gesture actually runs on.
//
//	The first cut timed the hold off WM_MOUSEMOVE with the AEGP idle hook as the
//	backstop for a motionless press. The S2B log showed that backstop does not
//	work: two presses held for 1.7 SECONDS were recorded as short clicks,
//	because the mouse never moved and the idle hook never ran while the button
//	was down. AE is in a modal drag loop for the duration of a press, and that
//	loop does not pump AEGP idle time.
//
//	A thread timer does survive it: modal loops still call DispatchMessage, and
//	DispatchMessage invokes the callback of a WM_TIMER posted with a NULL hwnd.
//	No window of our own is needed.
static VOID CALLBACK
HoldTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick)
{
	if (S_hold_timer) {
		KillTimer(NULL, S_hold_timer);
		S_hold_timer = 0;
	}

	if (S_rdownB && !S_hold_firedB) {
		OnHoldDetected("timer");
	}
}

static void
CancelHoldTimer(void)
{
	if (S_hold_timer) {
		KillTimer(NULL, S_hold_timer);
		S_hold_timer = 0;
	}
}

/* ---------------------------------------------------------------------------
	S2C-1 - when does AE actually raise its context menu?

	Everything about taking the gesture turns on this. If the menu comes from
	WM_RBUTTONUP we can let DOWN through untouched and swallow only the UP of a
	press that already crossed the threshold - a short click is then never even
	interfered with. If AE raises it on DOWN, that route is closed and the only
	remaining one is swallow-everything-and-resynthesise, which is much riskier.

	WM_CONTEXTMENU is SENT, not posted, so a WH_GETMESSAGE hook would never see
	it. WH_CALLWNDPROC does. It cannot modify or swallow anything - it is pure
	observation, which is all this step needs.
--------------------------------------------------------------------------- */

static HHOOK	S_wndproc_hook	= NULL;
static A_Boolean S_swallow_onB	= FALSE;
static int		S_swallowed		= 0;
static int		S_replayed		= 0;

//	How many hook events belong to a click WE injected. SendInput is
//	asynchronous, so a boolean cleared right after the call would already be
//	stale by the time our own hook sees the pair - it has to be a count that the
//	hook itself draws down.
static int		S_replay_pending = 0;

//	Put back a right-click we swallowed but did not want.
//
//	SendInput with no MOUSEEVENTF_ABSOLUTE uses the live cursor position, which
//	is where the press happened - the user has not had time to move far in under
//	RM_HOLD_MS. Injecting real input rather than posting WM_RBUTTON* means AE
//	sees exactly what it would have seen, through the same path, with no
//	assumptions about which window should receive it. That matters here: S2A
//	proved we cannot identify AE's panels, so we are in no position to address a
//	message to the right one.
static void
ReplayRightClick(void)
{
	INPUT in[2];

	ZeroMemory(in, sizeof(in));
	in[0].type			= INPUT_MOUSE;
	in[0].mi.dwFlags	= MOUSEEVENTF_RIGHTDOWN;
	in[1].type			= INPUT_MOUSE;
	in[1].mi.dwFlags	= MOUSEEVENTF_RIGHTUP;

	S_replay_pending = 2;

	UINT sent = SendInput(2, in, sizeof(INPUT));

	if (sent != 2) {
		S_replay_pending = 0;
		GestureLog("  !! REPLAY FAILED: SendInput sent %u of 2, GetLastError = %lu\n",
					sent, (unsigned long)GetLastError());
	} else {
		S_replayed++;
		GestureLog("  << replayed the click (#%d) - AE's menu should appear now\n", S_replayed);
	}
}

static const char *
MenuMsgName(UINT msg)
{
	switch (msg) {
		case WM_CONTEXTMENU:	return "WM_CONTEXTMENU";
		case WM_ENTERMENULOOP:	return "WM_ENTERMENULOOP";
		case WM_EXITMENULOOP:	return "WM_EXITMENULOOP";
		case WM_INITMENUPOPUP:	return "WM_INITMENUPOPUP";
		case WM_CAPTURECHANGED:	return "WM_CAPTURECHANGED";
	}
	return NULL;
}

static LRESULT CALLBACK
WndProcTrace(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HC_ACTION && S_watch_onB) {
		CWPSTRUCT	*cwp = reinterpret_cast<CWPSTRUCT*>(lParam);
		const char	*name = cwp ? MenuMsgName(cwp->message) : NULL;

		//	Only the menu-relevant traffic, and only in the window of time where
		//	it can be attributed to a press. AE sends thousands of messages a
		//	second; logging all of them would drown the signal.
		if (name && S_rdown_tick) {
			GestureLog("    [+%4lums] %s  hwnd=%p%s\n",
						(unsigned long)(GetTickCount() - S_rdown_tick),
						name,
						(void*)(cwp ? cwp->hwnd : NULL),
						S_rdownB ? "   (button still DOWN)" : "");
		}
	}
	return CallNextHookEx(S_wndproc_hook, code, wParam, lParam);
}

static LRESULT CALLBACK
MouseProc(int code, WPARAM wParam, LPARAM lParam)
{
	//	HC_ACTION is the only code carrying a message worth reading; anything
	//	less than zero must be passed straight on, per the Win32 contract.
	if (code == HC_ACTION && S_watch_onB) {
		MOUSEHOOKSTRUCT *mhs = reinterpret_cast<MOUSEHOOKSTRUCT*>(lParam);

		//	Our own injected click coming back around. Let it through untouched
		//	and do not treat it as a new gesture, or we would swallow the very
		//	thing we just replayed.
		if (S_replay_pending > 0 &&
			(wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP)) {
			S_replay_pending--;
			return CallNextHookEx(S_mouse_hook, code, wParam, lParam);
		}

		switch (wParam) {
			case WM_RBUTTONDOWN: {
				S_rdownB		= TRUE;
				S_hold_firedB	= FALSE;
				S_rdown_tick	= GetTickCount();
				S_rdown_pt		= mhs ? mhs->pt : S_rdown_pt;

				GestureLog("\n--- right button DOWN ---\n");
				LogPressTarget("  press", S_rdown_pt);

				//	Arm the real clock. A motionless press depends on this
				//	entirely - see HoldTimerProc.
				CancelHoldTimer();
				S_hold_timer = SetTimer(NULL, 0, RM_HOLD_MS, HoldTimerProc);

				if (!S_hold_timer) {
					GestureLog("  !! SetTimer FAILED, GetLastError = %lu\n",
								(unsigned long)GetLastError());
				}

				//	S2D: AE opens its menu on DOWN, so the DOWN is what has to
				//	go. We do not yet know whether this is a gesture or a click
				//	- that is decided at release, and a click gets replayed.
				if (S_swallow_onB) {
					GestureLog("  >> swallowed the DOWN\n");
					return 1;
				}
			} break;

			case WM_MOUSEMOVE: {
				//	Kept as a second path so the log can show which clock wins.
				//	If "via move" never appears, the timer is carrying the
				//	gesture on its own and this can go.
				if (S_rdownB && !S_hold_firedB &&
					(GetTickCount() - S_rdown_tick) >= RM_HOLD_MS) {
					OnHoldDetected("move");
				}
			} break;

			case WM_RBUTTONUP: {
				CancelHoldTimer();

				if (S_rdownB) {
					DWORD held = GetTickCount() - S_rdown_tick;

					if (S_hold_firedB) {
						GestureLog("  right button UP after %lums - was a HOLD\n", (unsigned long)held);

						//	Ours. AE saw neither the DOWN nor the UP, so there
						//	is nothing to undo and no menu to suppress.
						if (S_swallow_onB) {
							S_rdownB = FALSE;
							S_swallowed++;
							GestureLog("  >> SWALLOWED the UP too (#%d). AE never saw this press at all.\n",
										S_swallowed);
							return 1;
						}
					} else {
						S_click_count++;
						GestureLog("  right button UP after %lums - short CLICK #%d\n",
									(unsigned long)held, S_click_count);

						//	Not ours after all. Give AE back the click we took,
						//	then eat the real UP - its DOWN was already eaten,
						//	so letting it through would leave AE with an
						//	unmatched release.
						if (S_swallow_onB) {
							S_rdownB = FALSE;
							ReplayRightClick();
							return 1;
						}
					}
					S_rdownB = FALSE;
				}
			} break;

			default:
				break;
		}
	}

	//	Default is still passthrough. The ONE early return above is the only
	//	message this plug-in ever eats, and only when S2C swallowing is armed.
	return CallNextHookEx(S_mouse_hook, code, wParam, lParam);
}

static void
WatchToggle(AEGP_SuiteHandler &suites)
{
	char msg[MAX_PATH + 320];

	if (S_watch_onB) {
		S_watch_onB = FALSE;
		CancelHoldTimer();

		if (S_mouse_hook) {
			UnhookWindowsHookEx(S_mouse_hook);
			S_mouse_hook = NULL;
		}
		if (S_wndproc_hook) {
			UnhookWindowsHookEx(S_wndproc_hook);
			S_wndproc_hook = NULL;
		}

		GestureLog("\n=== watch OFF: %d hold(s), %d short click(s), %d swallowed ===\n",
					S_hold_count, S_click_count, S_swallowed);

		sprintf_s(msg, sizeof(msg),
			"Watch OFF.\n\n%d hold(s) taken, %d short click(s), %d replayed.\n\nLog: %s",
			S_hold_count, S_click_count, S_replayed, S_gesture_path);
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
		return;
	}

	DWORD len = GetTempPathA(MAX_PATH, S_gesture_path);

	if (!len || len > MAX_PATH - 32) {
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, "S2B: could not resolve a temp path.");
		S_gesture_path[0] = 0;
		return;
	}
	strcat_s(S_gesture_path, MAX_PATH, "pieFX_S2_gesture.txt");

	//	Thread-local: last argument is OUR thread, so this never leaves AE. The
	//	module handle must be NULL for a thread-local hook.
	S_mouse_hook = SetWindowsHookEx(WH_MOUSE, MouseProc, NULL, GetCurrentThreadId());

	if (!S_mouse_hook) {
		sprintf_s(msg, sizeof(msg),
			"S2B: SetWindowsHookEx(WH_MOUSE) FAILED, GetLastError = %lu.\n\n"
			"That is the spike failing, not a bug to work around - "
			"the fallback is hotkey-only summon.",
			(unsigned long)GetLastError());
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
		return;
	}

	//	Pure observation, cannot swallow anything - this is the only way to see
	//	WM_CONTEXTMENU, which is sent rather than posted.
	S_wndproc_hook = SetWindowsHookEx(WH_CALLWNDPROC, WndProcTrace, NULL, GetCurrentThreadId());

	S_watch_onB		= TRUE;
	S_hold_count	= 0;
	S_click_count	= 0;
	S_swallowed		= 0;
	S_replayed		= 0;
	S_replay_pending= 0;
	S_rdownB		= FALSE;

	FILE *fp = NULL;
	if (!fopen_s(&fp, S_gesture_path, "w") && fp) {
		fprintf(fp, "Radial Menu - S2B right-hold watch\n");
		fprintf(fp, "Hold threshold: %dms. This spike NEVER swallows a message.\n", RM_HOLD_MS);
		fprintf(fp, "Thread-local WH_MOUSE + WH_CALLWNDPROC hooks on AE's UI thread (id %lu).\n",
				(unsigned long)GetCurrentThreadId());
		fprintf(fp, "Swallow-on-hold is currently %s.\n\n", S_swallow_onB ? "ARMED" : "off");
		fclose(fp);
	}

	sprintf_s(msg, sizeof(msg),
		"S2B watch ON (threshold %dms).\n\n"
		"1. Right-click normally everywhere - the context menu MUST still appear every time.\n"
		"2. Right-press and hold PERFECTLY STILL: the beep must arrive at ~%dms.\n"
		"3. LEFT-click into the timeline first, then right-hold there, so the panel-type\n"
		"   readout is tested against a panel AE has actually focused.\n\n"
		"Run the command again to stop and see the tally.\n\nLog: %s",
		RM_HOLD_MS, RM_HOLD_MS, S_gesture_path);
	suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
}

/* ---------------------------------------------------------------------------
	S3 - the overlay window.

	Two questions, and only one of them is about drawing:

	1. Can a transparent, borderless, always-on-top window sit over AE and WIN
	   the z-order fight? The roadmap notes this is exactly where FX Console is
	   weakest - its ScriptUI palette can lose and render behind panels on
	   multi-monitor setups - and that losing it is the reason not to use
	   ScriptUI at all.
	2. Does a focus round-trip cost us the layer selection? The roadmap's
	   assumption is that selection is DOCUMENT state, not FOCUS state, and that
	   the overlay can therefore take focus like any normal window - skipping a
	   pile of NOACTIVATE tricks. It says to test that explicitly rather than
	   assume it, so this window deliberately DOES take focus.

	Note on toolchain: the roadmap puts the real overlay in Tauri, but it also
	says to lock the architecture BEFORE the POC and AFTER the spikes. A raw
	Win32 layered window answers both questions in a day and confounds nothing -
	if the OS itself cannot hold a window above AE, no toolkit can. What this
	does NOT cover is that Tauri's window would be OUT of process; see S3B.

	Per-pixel alpha via UpdateLayeredWindow, filled by hand. No GDI+, no
	dependencies - the point is the window, not the picture.
--------------------------------------------------------------------------- */

static HWND		S_overlay_hwnd		= NULL;
static UINT_PTR	S_overlay_timer		= 0;
static UINT_PTR	S_arm_timer			= 0;
static A_u_long	S_sel_before		= 0;
static HWND		S_focus_before		= NULL;

//	Selection is the thing we are afraid of losing, so measure it rather than
//	eyeball it. Returns the number of selected items in the active comp.
static A_u_long
CountSelection(AEGP_SuiteHandler &suites)
{
	A_Err				err				= A_Err_NONE;
	AEGP_ItemH			active_itemH	= NULL;
	AEGP_ItemType		item_type		= AEGP_ItemType_NONE;
	AEGP_CompH			compH			= NULL;
	AEGP_Collection2H	collectionH		= NULL;
	A_u_long			count			= 0;

	ERR(suites.ItemSuite6()->AEGP_GetActiveItem(&active_itemH));

	if (err || !active_itemH) {
		return 0;
	}

	ERR(suites.ItemSuite6()->AEGP_GetItemType(active_itemH, &item_type));

	if (err || AEGP_ItemType_COMP != item_type) {
		return 0;
	}

	ERR(suites.CompSuite11()->AEGP_GetCompFromItem(active_itemH, &compH));
	ERR(suites.CompSuite11()->AEGP_GetNewCollectionFromCompSelection(S_my_id, compH, &collectionH));

	if (!err && collectionH) {
		ERR(suites.CollectionSuite2()->AEGP_GetCollectionNumItems(collectionH, &count));
		suites.CollectionSuite2()->AEGP_DisposeCollection(collectionH);
	}

	return err ? 0 : count;
}

//	A soft-edged ring, drawn straight into a premultiplied BGRA DIB. Antialiased
//	by construction, which also proves the per-pixel alpha path really works -
//	a hard-edged shape would look the same with a colour key.
static void
PaintOverlayBits(void *bitsPV, int size)
{
	A_u_long	*px		= reinterpret_cast<A_u_long*>(bitsPV);
	const double centre	= (size - 1) / 2.0;
	const double outer	= centre - 6.0;
	const double inner	= outer - 26.0;

	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			double	dx	= x - centre;
			double	dy	= y - centre;
			double	d	= sqrt(dx * dx + dy * dy);

			//	1px feather on both edges of the ring.
			double	a	= 1.0;
			if (d > outer)		a = outer + 1.0 - d;
			else if (d < inner)	a = d - (inner - 1.0);

			if (a < 0.0) a = 0.0;
			if (a > 1.0) a = 1.0;
			a *= 0.85;

			//	Premultiplied, as UpdateLayeredWindow requires.
			A_u_char	A = static_cast<A_u_char>(a * 255.0 + 0.5);
			A_u_char	R = static_cast<A_u_char>(a * 255.0 + 0.5);
			A_u_char	G = static_cast<A_u_char>(a * 140.0 + 0.5);
			A_u_char	B = static_cast<A_u_char>(a *  20.0 + 0.5);

			px[y * size + x] = (A << 24) | (R << 16) | (G << 8) | B;
		}
	}
}

static void	CloseOverlay(void);

static LRESULT CALLBACK
OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_DESTROY) {
		S_overlay_hwnd = NULL;
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void CALLBACK
OverlayTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick)
{
	CloseOverlay();
}

//	Report what happened AFTER the window is gone and focus has settled - the
//	whole point is the state on the far side of the round trip.
static void
CloseOverlay(void)
{
	if (S_overlay_timer) {
		KillTimer(NULL, S_overlay_timer);
		S_overlay_timer = 0;
	}

	if (S_overlay_hwnd) {
		DestroyWindow(S_overlay_hwnd);
		S_overlay_hwnd = NULL;
	}

	//	Hand focus back where we found it. If selection survives WITHOUT this,
	//	so much the better - but not restoring focus would make AE unusable
	//	after every summon, so the real product has to do it anyway.
	if (S_focus_before && IsWindow(S_focus_before)) {
		SetForegroundWindow(S_focus_before);
	}

	AEGP_SuiteHandler	suites(sP);
	A_u_long			after	= CountSelection(suites);
	HWND				fg		= GetForegroundWindow();
	char				msg[512];

	sprintf_s(msg, sizeof(msg),
		"S3 overlay closed.\n\n"
		"Selected items BEFORE: %lu\n"
		"Selected items AFTER:  %lu\n"
		"%s\n\n"
		"Foreground window is now %s AE.\n\n"
		"By eye: did the ring sit ABOVE whatever was under it, and were its edges\n"
		"soft? That is the half no counter can answer.",
		(unsigned long)S_sel_before,
		(unsigned long)after,
		(S_sel_before == after)
			? "==> SELECTION SURVIVED the focus round-trip."
			: "==> SELECTION CHANGED. The roadmap's assumption is wrong.",
		(fg == S_main_hwnd) ? "back on" : "NOT");

	suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
}

static void
CreateOverlayNow(void)
{
	AEGP_SuiteHandler	suites(sP);

	S_sel_before = CountSelection(suites);

	S_main_hwnd = NULL;
	suites.UtilitySuite3()->AEGP_GetMainHWND(&S_main_hwnd);
	S_focus_before = GetForegroundWindow();

	HINSTANCE	inst = reinterpret_cast<HINSTANCE>(GetModuleHandle(NULL));
	static A_Boolean registeredB = FALSE;

	if (!registeredB) {
		WNDCLASSEX	wc;
		ZeroMemory(&wc, sizeof(wc));
		wc.cbSize			= sizeof(wc);
		wc.lpfnWndProc		= OverlayWndProc;
		wc.hInstance		= inst;
		wc.hCursor			= LoadCursor(NULL, IDC_ARROW);
		wc.lpszClassName	= "pieFXOverlay";

		if (!RegisterClassEx(&wc)) {
			char m[128];
			sprintf_s(m, sizeof(m), "S3: RegisterClassEx failed, GetLastError = %lu",
					(unsigned long)GetLastError());
			suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, m);
			return;
		}
		registeredB = TRUE;
	}

	POINT	pt = { 0, 0 };
	GetCursorPos(&pt);

	const int	size = RM_OVERLAY_SIZE;
	const int	x	 = pt.x - size / 2;
	const int	y	 = pt.y - size / 2;

	//	WS_EX_TOOLWINDOW keeps it out of the taskbar and Alt-Tab. TOPMOST is the
	//	z-order claim under test. Deliberately NOT WS_EX_NOACTIVATE - the point
	//	is to let it take focus and see whether selection survives.
	S_overlay_hwnd = CreateWindowEx(
		WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
		"pieFXOverlay", "", WS_POPUP,
		x, y, size, size,
		NULL, NULL, inst, NULL);

	if (!S_overlay_hwnd) {
		char m[128];
		sprintf_s(m, sizeof(m), "S3: CreateWindowEx failed, GetLastError = %lu",
				(unsigned long)GetLastError());
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, m);
		return;
	}

	//	Per-pixel alpha: build a 32bpp top-down DIB, paint it, blit it in.
	BITMAPINFO	bmi;
	ZeroMemory(&bmi, sizeof(bmi));
	bmi.bmiHeader.biSize		= sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth		= size;
	bmi.bmiHeader.biHeight		= -size;		// negative = top-down
	bmi.bmiHeader.biPlanes		= 1;
	bmi.bmiHeader.biBitCount	= 32;
	bmi.bmiHeader.biCompression	= BI_RGB;

	void	*bitsPV	= NULL;
	HDC		screen	= GetDC(NULL);
	HDC		mem		= CreateCompatibleDC(screen);
	HBITMAP	dib		= CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bitsPV, NULL, 0);

	if (dib && bitsPV) {
		PaintOverlayBits(bitsPV, size);

		HGDIOBJ	old		= SelectObject(mem, dib);
		POINT	dstPt	= { x, y };
		POINT	srcPt	= { 0, 0 };
		SIZE	wndSize	= { size, size };
		BLENDFUNCTION blend;
		blend.BlendOp				= AC_SRC_OVER;
		blend.BlendFlags			= 0;
		blend.SourceConstantAlpha	= 255;
		blend.AlphaFormat			= AC_SRC_ALPHA;

		UpdateLayeredWindow(S_overlay_hwnd, screen, &dstPt, &wndSize,
							mem, &srcPt, 0, &blend, ULW_ALPHA);

		SelectObject(mem, old);
	}

	if (dib)	DeleteObject(dib);
	if (mem)	DeleteDC(mem);
	ReleaseDC(NULL, screen);

	ShowWindow(S_overlay_hwnd, SW_SHOW);
	SetForegroundWindow(S_overlay_hwnd);		// take focus ON PURPOSE

	MessageBeep(MB_OK);							// "look now"

	//	Same NULL-hwnd thread timer as the hold clock, for the same reason.
	S_overlay_timer = SetTimer(NULL, 0, RM_OVERLAY_MS, OverlayTimerProc);
}

/* ---------------------------------------------------------------------------
	S3B - the same ring, from another process.

	S3A passing does not settle this. A TOPMOST window owned by a BACKGROUND
	process is a different z-order proposition, and launching it deactivates AE
	as an application (WM_ACTIVATEAPP) rather than merely moving focus within
	it - a stronger event than the one S3A proved selection survives. Since the
	planned overlay is Tauri, and Tauri is another process, this is the
	arrangement that actually has to work.

	The plug-in stays the instrument: count selection, launch, wait for exit,
	count again. Blocking on the child is fine here - it lives for ~3 seconds
	and this is a spike, not a gesture path.
--------------------------------------------------------------------------- */

//	The .exe is expected beside the .aex, so resolve OUR module - not
//	GetModuleHandle(NULL), which would give AfterFX.exe.
static A_Boolean
ResolveS3BPath(char *bufZ, size_t buf_size)
{
	HMODULE	self = NULL;

	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
							GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							reinterpret_cast<LPCSTR>(&ResolveS3BPath),
							&self) || !self) {
		return FALSE;
	}

	if (!GetModuleFileNameA(self, bufZ, static_cast<DWORD>(buf_size))) {
		return FALSE;
	}

	char *slash = strrchr(bufZ, '\\');
	if (!slash) {
		return FALSE;
	}
	slash[1] = 0;

	return (strcat_s(bufZ, buf_size, RM_S3B_EXE_NAME) == 0);
}

static void
RunOverlayOutOfProcess(void)
{
	AEGP_SuiteHandler	suites(sP);
	char				exe[MAX_PATH]	= { 0 };
	char				msg[1024];

	if (!ResolveS3BPath(exe, sizeof(exe))) {
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, "S3B: could not resolve the plug-in's own path.");
		return;
	}

	if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
		sprintf_s(msg, sizeof(msg),
			"S3B: helper not found.\n\nCopy pieFX_S3B.exe to sit beside the .aex:\n%s", exe);
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
		return;
	}

	POINT pt = { 0, 0 };
	GetCursorPos(&pt);

	A_u_long	before	= CountSelection(suites);
	HWND		fg_before = GetForegroundWindow();

	S_main_hwnd = NULL;
	suites.UtilitySuite3()->AEGP_GetMainHWND(&S_main_hwnd);

	char cmd[MAX_PATH + 96];
	sprintf_s(cmd, sizeof(cmd), "\"%s\" %ld %ld %d %d",
			exe, pt.x, pt.y, RM_OVERLAY_SIZE, RM_OVERLAY_MS);

	STARTUPINFOA		si;
	PROCESS_INFORMATION	pi;
	ZeroMemory(&si, sizeof(si));
	ZeroMemory(&pi, sizeof(pi));
	si.cb = sizeof(si);

	if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		sprintf_s(msg, sizeof(msg), "S3B: CreateProcess failed, GetLastError = %lu\n\n%s",
				(unsigned long)GetLastError(), cmd);
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
		return;
	}

	WaitForSingleObject(pi.hProcess, RM_OVERLAY_MS + 5000);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	//	Give focus back, exactly as the real product would have to.
	if (fg_before && IsWindow(fg_before)) {
		SetForegroundWindow(fg_before);
	}

	A_u_long	after	= CountSelection(suites);
	HWND		fg		= GetForegroundWindow();

	sprintf_s(msg, sizeof(msg),
		"S3B overlay (separate process) closed.\n\n"
		"Selected items BEFORE: %lu\n"
		"Selected items AFTER:  %lu\n"
		"%s\n\n"
		"Foreground window is now %s AE.\n\n"
		"By eye: did the CYAN ring sit above everything, the same as S3A's orange one?\n"
		"That is the z-order claim, and it is the one a background process might lose.",
		(unsigned long)before,
		(unsigned long)after,
		(before == after)
			? "==> SELECTION SURVIVED, even across a process switch."
			: "==> SELECTION CHANGED. Out-of-process focus costs us the selection.",
		(fg == S_main_hwnd) ? "back on" : "NOT");

	suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
}

static void CALLBACK
S3BArmTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick)
{
	if (S_arm_timer) {
		KillTimer(NULL, S_arm_timer);
		S_arm_timer = 0;
	}
	RunOverlayOutOfProcess();
}

static void CALLBACK
ArmTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick)
{
	if (S_arm_timer) {
		KillTimer(NULL, S_arm_timer);
		S_arm_timer = 0;
	}
	CreateOverlayNow();
}

//	Arm, do not show. The overlay appears wherever the cursor is when the grace
//	period ends, so each run can test a different panel.
static void
ShowOverlay(AEGP_SuiteHandler &suites)
{
	if (S_overlay_hwnd) {
		CloseOverlay();
		return;
	}

	if (S_arm_timer) {
		KillTimer(NULL, S_arm_timer);
		S_arm_timer = 0;
	}

	char msg[384];
	sprintf_s(msg, sizeof(msg),
		"S3 armed.\n\n"
		"Dismiss this, then move the cursor over whatever you want to test.\n"
		"In %d seconds the ring appears there (you will hear a beep), stays %d seconds,\n"
		"and closes itself.\n\n"
		"Worth testing separately: comp viewer, timeline, a CEP panel, second monitor.",
		RM_OVERLAY_ARM_MS / 1000, RM_OVERLAY_MS / 1000);

	//	ReportInfo is modal, so the clock must start AFTER it returns - otherwise
	//	the grace period burns down behind the alert.
	suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);

	S_arm_timer = SetTimer(NULL, 0, RM_OVERLAY_ARM_MS, ArmTimerProc);
}

//	Same arming dance, different payload.
static void
ShowOverlayOutOfProcess(AEGP_SuiteHandler &suites)
{
	if (S_arm_timer) {
		KillTimer(NULL, S_arm_timer);
		S_arm_timer = 0;
	}

	char msg[384];
	sprintf_s(msg, sizeof(msg),
		"S3B armed (separate process).\n\n"
		"Dismiss this, then move the cursor over whatever you want to test.\n"
		"In %d seconds a CYAN ring appears there - drawn by another process, not by AE.\n\n"
		"Same targets as S3A: comp viewer, timeline, a CEP panel, second monitor.",
		RM_OVERLAY_ARM_MS / 1000);

	suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);

	S_arm_timer = SetTimer(NULL, 0, RM_OVERLAY_ARM_MS, S3BArmTimerProc);
}

/* ---------------------------------------------------------------------------
	S5 - the installed effects catalogue.

	The roadmap's reason for this being native-side territory: there is no clean
	ExtendScript API for the full installed-effects list, which is a large part
	of why the AEGP layer is load-bearing rather than optional. It is also the
	only thing the search segment needs - fuzzy-match display names, apply by
	match name.

	Its stated failure mode is "you can only get a partial or unstable list", and
	a COUNT would hide exactly that. So S5A dumps every entry to a file with its
	category, display name and match name, where it can be checked against what
	is actually installed - third-party effects especially, since those are the
	ones a partial list would drop.

	S5B closes the loop the search bar actually needs: look a key up BY MATCH
	NAME and apply it. Match name rather than display name because display names
	are localised and change between versions; match names do not.
--------------------------------------------------------------------------- */

static A_Boolean
FindEffectKeyByMatchName(
	AEGP_SuiteHandler		&suites,
	const char				*wantedZ,
	AEGP_InstalledEffectKey	*keyP)
{
	A_Err					err		= A_Err_NONE;
	AEGP_InstalledEffectKey	key		= AEGP_InstalledEffectKey_NONE;
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
DumpEffectCatalogue(AEGP_SuiteHandler &suites)
{
	A_Err		err		= A_Err_NONE;
	A_long		claimed	= 0;
	char		path[MAX_PATH] = { 0 };
	DWORD		len		= GetTempPathA(MAX_PATH, path);

	if (!len || len > MAX_PATH - 32) {
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, "S5A: could not resolve a temp path.");
		return;
	}
	strcat_s(path, MAX_PATH, "pieFX_S5_effects.txt");

	ERR(suites.EffectSuite4()->AEGP_GetNumInstalledEffects(&claimed));

	FILE *fp = NULL;
	if (fopen_s(&fp, path, "w") || !fp) {
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, "S5A: could not open the log for writing.");
		return;
	}

	fprintf(fp, "Radial Menu - S5A installed effects catalogue\n");
	fprintf(fp, "AEGP_GetNumInstalledEffects reports: %ld\n\n", claimed);
	fprintf(fp, "%-5s  %-28s  %-44s  %s\n", "#", "CATEGORY", "DISPLAY NAME", "MATCH NAME");
	fprintf(fp, "%-5s  %-28s  %-44s  %s\n", "-----", "----------------------------",
			"--------------------------------------------", "----------");

	AEGP_InstalledEffectKey	key		= AEGP_InstalledEffectKey_NONE;
	A_long					walked	= 0;

	while (!err) {
		ERR(suites.EffectSuite4()->AEGP_GetNextInstalledEffect(key, &key));

		if (err || AEGP_InstalledEffectKey_NONE == key) {
			break;
		}

		A_char	name[AEGP_MAX_EFFECT_NAME_SIZE]				= { 0 };
		A_char	match[AEGP_MAX_EFFECT_MATCH_NAME_SIZE]		= { 0 };
		A_char	cat[AEGP_MAX_EFFECT_CATEGORY_NAME_SIZE]		= { 0 };

		ERR(suites.EffectSuite4()->AEGP_GetEffectName(key, name));
		ERR(suites.EffectSuite4()->AEGP_GetEffectMatchName(key, match));
		ERR(suites.EffectSuite4()->AEGP_GetEffectCategory(key, cat));

		walked++;
		fprintf(fp, "%-5ld  %-28s  %-44s  %s\n", walked, cat, name, match);
	}

	//	The interesting number is not the count, it is whether WALKING the list
	//	agrees with what AE claims is in it. A mismatch is the "partial or
	//	unstable" failure the roadmap warns about, showing itself.
	fprintf(fp, "\nWalked %ld entries; AE claimed %ld. %s\n",
			walked, claimed,
			(walked == claimed) ? "MATCH." : "*** MISMATCH - the enumeration is not complete. ***");

	if (err) {
		fprintf(fp, "Enumeration stopped early with AEGP error %d.\n", static_cast<int>(err));
	}

	fclose(fp);

	char msg[MAX_PATH + 320];
	sprintf_s(msg, sizeof(msg),
		"S5A: walked %ld effects; AE claims %ld. %s\n\n"
		"Check the file against effects you KNOW are installed - third-party ones\n"
		"especially, since a partial list would drop those first.\n\n%s",
		walked, claimed,
		(walked == claimed) ? "Counts match." : "COUNTS DISAGREE.",
		path);
	suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
}

static void
ApplyEffectByMatchName(AEGP_SuiteHandler &suites)
{
	A_Err					err		= A_Err_NONE;
	AEGP_LayerH				layerH	= NULL;
	AEGP_InstalledEffectKey	key		= AEGP_InstalledEffectKey_NONE;
	AEGP_EffectRefH			refH	= NULL;
	char					msg[512];

	ERR(suites.LayerSuite9()->AEGP_GetActiveLayer(&layerH));

	if (err || !layerH) {
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id,
			"S5B: select exactly one layer first (AEGP_GetActiveLayer returns a layer only when one is selected).");
		return;
	}

	if (!FindEffectKeyByMatchName(suites, RM_S5B_MATCH_NAME, &key)) {
		sprintf_s(msg, sizeof(msg),
			"S5B: no installed effect with match name \"%s\".\n\n"
			"If S5A listed it, the lookup is broken; if S5A did not, the catalogue is.",
			RM_S5B_MATCH_NAME);
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
		return;
	}

	ERR(suites.UtilitySuite3()->AEGP_StartUndoGroup("Radial Menu S5B: Apply Effect"));
	ERR(suites.EffectSuite4()->AEGP_ApplyEffect(S_my_id, layerH, key, &refH));

	if (refH) {
		suites.EffectSuite4()->AEGP_DisposeEffect(refH);
	}
	suites.UtilitySuite3()->AEGP_EndUndoGroup();

	if (err) {
		sprintf_s(msg, sizeof(msg), "S5B: AEGP_ApplyEffect failed with error %d.", static_cast<int>(err));
	} else {
		sprintf_s(msg, sizeof(msg),
			"S5B: applied \"%s\" to the selected layer.\n\n"
			"That is the whole search path proven: catalogue -> match name -> key -> apply.\n"
			"Still to do at MVP: open Effect Controls afterwards.",
			RM_S5B_MATCH_NAME);
	}
	suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
}

//	S2C-2 arm/disarm. Separate from the watch so one AE session can run the
//	trace first, read the timing, and only then arm the experiment.
static void
SwallowToggle(AEGP_SuiteHandler &suites)
{
	S_swallow_onB = !S_swallow_onB;

	GestureLog("\n=== swallow-on-hold %s ===\n", S_swallow_onB ? "ARMED" : "disarmed");

	if (S_swallow_onB) {
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id,
			"S2D swallow ARMED.\n\n"
			"Every right-button DOWN is now eaten. A press past 200ms is ours - AE never\n"
			"sees it. A shorter press is replayed with SendInput so AE's menu still opens.\n\n"
			"Judge these BY EYE, not by the log - the log was wrong last time:\n"
			"(1) a hold shows NO menu, anywhere.\n"
			"(2) a short right-click still shows the SAME menu, in the right place -\n"
			"    but now on release rather than on press. Does that feel wrong?\n"
			"(3) no double menus, no stuck menus, no lost clicks.");
	} else {
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, "S2D swallow disarmed.");
	}
}

//	Read an AEGP_MemHandle of A_char back into a std::string-ish buffer and
//	dispose of it. Returns "" for a NULL handle.
static A_Err
ReportAndDisposeHandle(
	AEGP_SuiteHandler	&suites,
	AEGP_MemHandle		handleH,
	const A_char		*prefixZ)
{
	A_Err	err = A_Err_NONE;

	if (!handleH) {
		return err;
	}

	A_char	*textP = NULL;

	ERR(suites.MemorySuite1()->AEGP_LockMemHandle(handleH, reinterpret_cast<void**>(&textP)));

	//	AE hands back a non-NULL but EMPTY error handle on success, so an
	//	unconditional report fires a blank alert every time. Test the string,
	//	not the handle.
	if (!err && textP && textP[0] != 0) {
		A_char	msg[512];
		suites.ANSICallbacksSuite1()->sprintf(msg, "%s%s", prefixZ, textP);
		ERR(suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg));
	}

	ERR(suites.MemorySuite1()->AEGP_UnlockMemHandle(handleH));
	ERR(suites.MemorySuite1()->AEGP_FreeMemHandle(handleH));

	return err;
}

//	The probe samples here. An idle hook that never fires while the mouse moves
//	is itself an S2 finding, so this stays deliberately dumb.
static A_Err
IdleHook(
	AEGP_GlobalRefcon	plugin_refconP,
	AEGP_IdleRefcon		refconP,
	A_long				*max_sleepPL)
{
	//	Kept only to prove the negative. The first S2B run showed the idle hook
	//	does NOT run while a mouse button is held - two 1.7-second presses were
	//	logged as short clicks - so this must never be the winning path. If
	//	"via idle" shows up in the log, that assumption needs revisiting.
	if (S_rdownB && !S_hold_firedB && (GetTickCount() - S_rdown_tick) >= RM_HOLD_MS) {
		OnHoldDetected("idle");
	}

	if (S_probe_armedB) {
		if (GetTickCount() > S_probe_end_tick) {
			S_probe_armedB = FALSE;

			AEGP_SuiteHandler	suites(sP);
			char				msg[MAX_PATH + 96];
			sprintf_s(msg, sizeof(msg), "S2A done. %d distinct window(s) logged to:\n%s",
					S_seen_count, S_probe_path);
			suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
		} else {
			ProbeSample();
			if (max_sleepPL) {
				*max_sleepPL = 1;	// ask to be woken often while sampling
			}
		}
	}
	return A_Err_NONE;
}

static A_Err
DeathHook(
	AEGP_GlobalRefcon	plugin_refconP,
	AEGP_DeathRefcon	refconP)
{
	//	Leaving a hook installed as the DLL unloads is how you take the host down
	//	with you.
	CancelHoldTimer();

	if (S_mouse_hook) {
		UnhookWindowsHookEx(S_mouse_hook);
		S_mouse_hook = NULL;
	}
	if (S_wndproc_hook) {
		UnhookWindowsHookEx(S_wndproc_hook);
		S_wndproc_hook = NULL;
	}
	S_watch_onB = FALSE;
	return A_Err_NONE;
}

//	Only offer the command when a comp is frontmost. Same shape as Commando.
static A_Err
UpdateMenuHook(
	AEGP_GlobalRefcon		plugin_refconPV,
	AEGP_UpdateMenuRefcon	refconPV,
	AEGP_WindowType			active_window)
{
	A_Err				err				= A_Err_NONE,
						err2			= A_Err_NONE;
	AEGP_ItemH			active_itemH	= NULL;
	AEGP_ItemType		item_type		= AEGP_ItemType_NONE;

	AEGP_SuiteHandler	suites(sP);

	ERR(suites.ItemSuite6()->AEGP_GetActiveItem(&active_itemH));

	if (!err && active_itemH) {
		ERR(suites.ItemSuite6()->AEGP_GetItemType(active_itemH, &item_type));
	}

	if (!err && active_itemH && AEGP_ItemType_COMP == item_type) {
		ERR(suites.CommandSuite1()->AEGP_EnableCommand(S_anchor_cmd));
	} else {
		ERR2(suites.CommandSuite1()->AEGP_DisableCommand(S_anchor_cmd));
	}

	//	The only place AE ever tells us which panel is active. There is no
	//	getter, so cache it and let S2B's log show whether the cache is current
	//	at the moment of a press.
	S_last_wintype = active_window;

	//	Neither spike command cares what is selected.
	ERR2(suites.CommandSuite1()->AEGP_EnableCommand(S_probe_cmd));
	ERR2(suites.CommandSuite1()->AEGP_EnableCommand(S_watch_cmd));
	ERR2(suites.CommandSuite1()->AEGP_EnableCommand(S_swallow_cmd));
	ERR2(suites.CommandSuite1()->AEGP_EnableCommand(S_overlay_cmd));
	ERR2(suites.CommandSuite1()->AEGP_EnableCommand(S_overlay2_cmd));
	ERR2(suites.CommandSuite1()->AEGP_EnableCommand(S_fxdump_cmd));
	ERR2(suites.CommandSuite1()->AEGP_EnableCommand(S_fxapply_cmd));
	return err;
}

static A_Err
CommandHook(
	AEGP_GlobalRefcon	plugin_refconPV,
	AEGP_CommandRefcon	refconPV,
	AEGP_Command		command,
	AEGP_HookPriority	hook_priority,
	A_Boolean			already_handledB,
	A_Boolean			*handledPB)
{
	A_Err				err = A_Err_NONE;
	AEGP_SuiteHandler	suites(sP);

	if (S_anchor_cmd == command) {
		A_Boolean		scripting_availableB	= FALSE;
		AEGP_MemHandle	resultH					= NULL,
						errorH					= NULL;

		ERR(suites.UtilitySuite6()->AEGP_IsScriptingAvailable(&scripting_availableB));

		if (!err && !scripting_availableB) {
			ERR(suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id,
				"S1: scripting is unavailable (check Preferences > Scripting & Expressions)."));
		} else {
			ERR(suites.UtilitySuite6()->AEGP_ExecuteScript(	S_my_id,
															S_anchor_script,
															FALSE,	// UTF-8, not platform encoding
															&resultH,
															&errorH));

			ERR(ReportAndDisposeHandle(suites, errorH,  "S1 script error: "));
			ERR(ReportAndDisposeHandle(suites, resultH, "S1: "));
		}

		*handledPB = TRUE;
	} else if (S_probe_cmd == command) {
		ProbeBegin(suites);
		*handledPB = TRUE;
	} else if (S_watch_cmd == command) {
		WatchToggle(suites);
		*handledPB = TRUE;
	} else if (S_swallow_cmd == command) {
		SwallowToggle(suites);
		*handledPB = TRUE;
	} else if (S_overlay_cmd == command) {
		ShowOverlay(suites);
		*handledPB = TRUE;
	} else if (S_overlay2_cmd == command) {
		ShowOverlayOutOfProcess(suites);
		*handledPB = TRUE;
	} else if (S_fxdump_cmd == command) {
		DumpEffectCatalogue(suites);
		*handledPB = TRUE;
	} else if (S_fxapply_cmd == command) {
		ApplyEffectByMatchName(suites);
		*handledPB = TRUE;
	}
	return err;
}

//	NB: this signature must match AEGP_PluginInitFuncPrototype EXACTLY (five
//	params). The Commando sample still shows a stale seven-param form with
//	file_pathZ/res_pathZ; using it makes this an *overload* of the extern "C"
//	declaration in the header, so it exports C++-mangled and AE reports
//	"Couldn't find main entry point".
DllExport A_Err
EntryPointFunc(
	struct SPBasicSuite		*pica_basicP,			/* >> */
	A_long					major_versionL,			/* >> */
	A_long					minor_versionL,			/* >> */
	AEGP_PluginID			aegp_plugin_id,			/* >> */
	AEGP_GlobalRefcon		*global_refconP)		/* << */
{
	A_Err				err		= A_Err_NONE,
						err2	= A_Err_NONE;

	sP		= pica_basicP;
	S_my_id	= aegp_plugin_id;		// the sample forgets this; everything below needs it

	AEGP_SuiteHandler	suites(pica_basicP);

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_anchor_cmd));

	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(	S_anchor_cmd,
														RM_S1_MENU_NAME,
														AEGP_Menu_WINDOW,
														AEGP_MENU_INSERT_SORTED));

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_probe_cmd));

	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(	S_probe_cmd,
														RM_S2A_MENU_NAME,
														AEGP_Menu_WINDOW,
														AEGP_MENU_INSERT_SORTED));

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_watch_cmd));

	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(	S_watch_cmd,
														RM_S2B_MENU_NAME,
														AEGP_Menu_WINDOW,
														AEGP_MENU_INSERT_SORTED));

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_swallow_cmd));

	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(	S_swallow_cmd,
														RM_S2C_MENU_NAME,
														AEGP_Menu_WINDOW,
														AEGP_MENU_INSERT_SORTED));

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_overlay_cmd));

	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(	S_overlay_cmd,
														RM_S3_MENU_NAME,
														AEGP_Menu_WINDOW,
														AEGP_MENU_INSERT_SORTED));

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_overlay2_cmd));

	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(	S_overlay2_cmd,
														RM_S3B_MENU_NAME,
														AEGP_Menu_WINDOW,
														AEGP_MENU_INSERT_SORTED));

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_fxdump_cmd));

	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(	S_fxdump_cmd,
														RM_S5A_MENU_NAME,
														AEGP_Menu_WINDOW,
														AEGP_MENU_INSERT_SORTED));

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_fxapply_cmd));

	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(	S_fxapply_cmd,
														RM_S5B_MENU_NAME,
														AEGP_Menu_WINDOW,
														AEGP_MENU_INSERT_SORTED));

	ERR(suites.RegisterSuite5()->AEGP_RegisterCommandHook(	S_my_id,
															AEGP_HP_BeforeAE,
															AEGP_Command_ALL,
															CommandHook,
															0));
	ERR(suites.RegisterSuite5()->AEGP_RegisterUpdateMenuHook(S_my_id, UpdateMenuHook, NULL));
	ERR(suites.RegisterSuite5()->AEGP_RegisterIdleHook(	S_my_id, IdleHook, NULL));
	ERR(suites.RegisterSuite5()->AEGP_RegisterDeathHook(	S_my_id, DeathHook, NULL));

	if (err) {
		ERR2(suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, "Radial Menu S1 failed to register."));
	}
	return err;
}
