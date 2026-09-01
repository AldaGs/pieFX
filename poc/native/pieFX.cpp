/*
	pieFX.cpp - POC: the Anchor wheel (Windows).

	See pieFX.h for the shape. The flow, all on AE's UI thread except the pipe
	accept:

	  right-button DOWN ....... swallowed (AE opens its menu on DOWN; S2D)
	  held past 200ms ......... summon: query selection, tell the overlay to show
	                            the 3x3 grid at the cursor
	  mouse moves ............. stream the highlighted cell to the overlay
	  right-button UP ......... if it was a hold: swallow the UP too, hide the
	                            overlay, and queue the anchor move for the chosen
	                            cell (run from IdleHook, never from inside the
	                            mouse hook)
	                            if it was a short click: replay it so AE's normal
	                            context menu still appears

	Modelled on Persisto (five-param entry point) and on the frozen Phase-0
	spike (../../pieFX.cpp), whose gesture engine this reuses almost verbatim.
*/

#include "pieFX.h"

// ---------------------------------------------------------------------------
//	globals
// ---------------------------------------------------------------------------
static AEGP_Command		S_toggle_cmd	= 0L;
static AEGP_PluginID	S_my_id			= 0L;
static SPBasicSuite		*sP				= NULL;

static A_Boolean		S_active		= FALSE;	// POC armed?
static char				S_log_path[MAX_PATH] = { 0 };

//	gesture state (from the S2D spike)
static HHOOK			S_mouse_hook	= NULL;
static UINT_PTR			S_hold_timer	= 0;
static DWORD			S_rdown_tick	= 0;
static POINT			S_rdown_pt		= { 0, 0 };
static A_Boolean		S_rdownB		= FALSE;
static A_Boolean		S_hold_firedB	= FALSE;
static int				S_replay_pending = 0;

//	summon session
static LONG				S_summon_cx		= 0;
static LONG				S_summon_cy		= 0;
static int				S_cur_cell		= -1;

//	selection context, refreshed in IdleHook so the summon (which runs inside the
//	mouse hook, where AEGP calls would be reentrant) can read a cached value.
static A_Boolean		S_has_selection	= FALSE;
static A_long			S_layer_count	= 0;

//	deferred action: the mouse hook must not run a script. It parks the chosen
//	cell here and IdleHook fires it once the modal press loop has ended.
static A_Boolean		S_action_pending = FALSE;
static int				S_action_cell	= -1;

//	pipe (native = server). Writes happen on the UI thread; a background thread
//	owns accept/re-accept. The critical section guards the handle + flags.
static HANDLE			S_pipe			= INVALID_HANDLE_VALUE;
static HANDLE			S_pipe_thread	= NULL;
static HANDLE			S_pipe_dead_evt	= NULL;	// UI -> bg: this connection broke
static HANDLE			S_pipe_stop_evt	= NULL;	// UI -> bg: shut down
static A_Boolean		S_pipe_connected = FALSE;
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

// ---------------------------------------------------------------------------
//	the generalised anchor script (S1, from centre to any grid fraction)
//
//	fx, fy in {0, 0.5, 1} pick the anchor's position on the layer's source rect.
//	The position compensation (delta pushed through Scale and Z-Rotation so the
//	pixels stay put) is exactly the S1 spike's; only cx/cy are parameterised.
// ---------------------------------------------------------------------------
static const char *S_anchor_script_fmt =
	"(function(){"
	"  var c = app.project.activeItem;"
	"  if (!(c instanceof CompItem)) { return 'no active comp'; }"
	"  var sel = c.selectedLayers;"
	"  if (sel.length === 0) { return 'no layer selected'; }"
	"  app.beginUndoGroup('pieFX: Anchor');"
	"  var moved = 0;"
	"  for (var i = 0; i < sel.length; i++) {"
	"    var L = sel[i];"
	"    var ap = L.property('Anchor Point');"
	"    if (!ap) { continue; }"
	"    var r;"
	"    try { r = L.sourceRectAtTime(c.time, false); } catch (e) { continue; }"
	"    var oldA = ap.value;"
	"    var cx = r.left + r.width * (%f);"
	"    var cy = r.top + r.height * (%f);"
	"    var dx = cx - oldA[0];"
	"    var dy = cy - oldA[1];"
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

// ---------------------------------------------------------------------------
//	wheel geometry - MUST mirror ../overlay/src/main.js
// ---------------------------------------------------------------------------
static int
CellForPoint(LONG cx, LONG cy, LONG px, LONG py)
{
	const int span = 3 * PIEFX_CELL + 2 * PIEFX_GAP;
	const int half = span / 2;
	for (int i = 0; i < 9; i++) {
		int row = i / 3, col = i % 3;
		LONG gx = cx - half + col * (PIEFX_CELL + PIEFX_GAP);
		LONG gy = cy - half + row * (PIEFX_CELL + PIEFX_GAP);
		if (px >= gx && px <= gx + PIEFX_CELL &&
			py >= gy && py <= gy + PIEFX_CELL) {
			return i;
		}
	}
	return -1;
}

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
		DWORD written = 0;
		size_t len = strlen(jsonZ);
		BOOL ok = WriteFile(S_pipe, jsonZ, (DWORD)len, &written, NULL);
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

static void SendSummon(LONG x, LONG y, A_Boolean hasSel, A_long layers)
{
	char m[192];
	sprintf_s(m, sizeof(m),
		"{\"type\":\"summon\",\"x\":%ld,\"y\":%ld,\"hasSelection\":%s,\"layerCount\":%ld}\n",
		x, y, hasSel ? "true" : "false", layers);
	PipeWrite(m);
}
static void SendCursor(LONG x, LONG y, int cell)
{
	char m[160];
	sprintf_s(m, sizeof(m),
		"{\"type\":\"cursor\",\"x\":%ld,\"y\":%ld,\"cell\":%d}\n", x, y, cell);
	PipeWrite(m);
}
static void SendRelease(void) { PipeWrite("{\"type\":\"release\"}\n"); }
static void SendCancel(void)  { PipeWrite("{\"type\":\"cancel\"}\n"); }

// ---------------------------------------------------------------------------
//	pipe: server thread (accept + re-accept)
// ---------------------------------------------------------------------------
static DWORD WINAPI
PipeServerThread(LPVOID)
{
	for (;;) {
		HANDLE inst = CreateNamedPipeA(
			PIEFX_PIPE_NAME,
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1,				// one instance - one overlay
			4096, 4096,
			0, NULL);

		if (inst == INVALID_HANDLE_VALUE) {
			Log("  pipe: CreateNamedPipe failed, err=%lu\n", (unsigned long)GetLastError());
			return 1;
		}

		//	ConnectNamedPipe blocks until the overlay connects. We poll the stop
		//	event around it by overlapping would be cleaner, but for the POC a
		//	blocking accept plus a CancelSynchronousIo-free shutdown (close the
		//	handle from stop path) is enough: on stop we just exit the process.
		BOOL connected = ConnectNamedPipe(inst, NULL)
			? TRUE
			: (GetLastError() == ERROR_PIPE_CONNECTED);

		if (WaitForSingleObject(S_pipe_stop_evt, 0) == WAIT_OBJECT_0) {
			CloseHandle(inst);
			return 0;
		}

		if (!connected) {
			CloseHandle(inst);
			continue;
		}

		EnterCriticalSection(&S_pipe_cs);
		S_pipe = inst;
		S_pipe_connected = TRUE;
		LeaveCriticalSection(&S_pipe_cs);
		Log("  pipe: overlay connected\n");

		//	Park until this connection dies or we're told to stop.
		HANDLE evs[2] = { S_pipe_dead_evt, S_pipe_stop_evt };
		DWORD w = WaitForMultipleObjects(2, evs, FALSE, INFINITE);

		EnterCriticalSection(&S_pipe_cs);
		S_pipe_connected = FALSE;
		S_pipe = INVALID_HANDLE_VALUE;
		LeaveCriticalSection(&S_pipe_cs);
		DisconnectNamedPipe(inst);
		CloseHandle(inst);
		Log("  pipe: overlay disconnected\n");

		if (w == WAIT_OBJECT_0 + 1) {	// stop
			return 0;
		}
		// else: loop and re-accept (overlay restarted)
	}
}

static void
StartPipeServer(void)
{
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
	//	Nudge a blocking ConnectNamedPipe to return by opening+closing the pipe.
	HANDLE poke = CreateFileA(PIEFX_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
							  0, NULL, OPEN_EXISTING, 0, NULL);
	if (poke != INVALID_HANDLE_VALUE) {
		CloseHandle(poke);
	}
	if (S_pipe_thread) {
		WaitForSingleObject(S_pipe_thread, 2000);
		CloseHandle(S_pipe_thread);
		S_pipe_thread = NULL;
	}
	if (S_pipe_dead_evt) { CloseHandle(S_pipe_dead_evt); S_pipe_dead_evt = NULL; }
	if (S_pipe_stop_evt) { CloseHandle(S_pipe_stop_evt); S_pipe_stop_evt = NULL; }
	S_pipe_connected = FALSE;
	S_pipe = INVALID_HANDLE_VALUE;
}

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

	STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
	PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
	if (CreateProcessA(exe, NULL, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		Log("  overlay: launched %s\n", exe);
	} else {
		Log("  overlay: CreateProcess failed, err=%lu\n", (unsigned long)GetLastError());
	}
}

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
	S_cur_cell  = -1;

	if (S_has_selection) {
		SendSummon(S_summon_cx, S_summon_cy, TRUE, S_layer_count);
	} else {
		//	Still show the wheel, greyed - it teaches the gesture even with
		//	nothing selected. hasSelection=false makes the overlay grey it.
		SendSummon(S_summon_cx, S_summon_cy, FALSE, 0);
	}
	Log("  HOLD -> summon at (%ld,%ld) hasSel=%d layers=%ld\n",
		S_summon_cx, S_summon_cy, S_has_selection, S_layer_count);
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
				S_hold_timer = SetTimer(NULL, 0, PIEFX_HOLD_MS, HoldTimerProc);
				//	AE opens its context menu on DOWN, so the DOWN must go.
				return 1;
			}

			case WM_MOUSEMOVE: {
				if (S_rdownB && !S_hold_firedB &&
					(GetTickCount() - S_rdown_tick) >= PIEFX_HOLD_MS) {
					OnHoldDetected();
				}
				if (S_rdownB && S_hold_firedB && mhs) {
					int cell = S_has_selection
						? CellForPoint(S_summon_cx, S_summon_cy, mhs->pt.x, mhs->pt.y)
						: -1;
					if (cell != S_cur_cell) {
						S_cur_cell = cell;
						SendCursor(mhs->pt.x, mhs->pt.y, cell);
					}
				}
			} break;

			case WM_RBUTTONUP: {
				CancelHoldTimer();
				if (S_rdownB) {
					if (S_hold_firedB) {
						//	Ours. AE saw neither DOWN nor UP.
						S_rdownB = FALSE;
						SendRelease();
						if (S_has_selection && S_cur_cell >= 0) {
							//	Never run a script from inside the hook - defer.
							S_action_cell = S_cur_cell;
							S_action_pending = TRUE;
							Log("  UP -> fire cell %d (deferred to idle)\n", S_cur_cell);
						} else {
							Log("  UP -> no fire (hasSel=%d cell=%d)\n",
								S_has_selection, S_cur_cell);
						}
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

// ---------------------------------------------------------------------------
//	selection context (safe here in idle), and the deferred anchor action
// ---------------------------------------------------------------------------
static void
RefreshSelectionContext(AEGP_SuiteHandler &suites)
{
	A_Boolean	hasSel	= FALSE;
	A_long		count	= 0;
	AEGP_ItemH	itemH	= NULL;
	AEGP_ItemType type	= AEGP_ItemType_NONE;
	A_Err		err		= A_Err_NONE, err2 = A_Err_NONE;

	ERR(suites.ItemSuite6()->AEGP_GetActiveItem(&itemH));
	if (!err && itemH) {
		ERR(suites.ItemSuite6()->AEGP_GetItemType(itemH, &type));
	}
	if (!err && itemH && type == AEGP_ItemType_COMP) {
		AEGP_CompH		compH = NULL;
		AEGP_Collection2H collH = NULL;
		ERR(suites.CompSuite11()->AEGP_GetCompFromItem(itemH, &compH));
		if (!err && compH) {
			ERR(suites.CompSuite11()->AEGP_GetNewCollectionFromCompSelection(S_my_id, compH, &collH));
			if (!err && collH) {
				A_u_long n = 0;
				ERR2(suites.CollectionSuite2()->AEGP_GetCollectionNumItems(collH, &n));
				count = (A_long)n;
				hasSel = (n > 0);
				ERR2(suites.CollectionSuite2()->AEGP_DisposeCollection(collH));
			}
		}
	}
	S_has_selection = hasSel;
	S_layer_count	= count;
}

static void
RunAnchorAction(AEGP_SuiteHandler &suites, int cell)
{
	int row = cell / 3, col = cell % 3;
	double fx = col * 0.5;	// 0, 0.5, 1
	double fy = row * 0.5;

	//	Assemble the script (fmt has two %f for cx/cy fractions).
	char script[2048];
	sprintf_s(script, sizeof(script), S_anchor_script_fmt, fx, fy);

	A_Boolean		avail = FALSE;
	AEGP_MemHandle	resultH = NULL, errorH = NULL;
	A_Err			err = A_Err_NONE, err2 = A_Err_NONE;

	ERR(suites.UtilitySuite6()->AEGP_IsScriptingAvailable(&avail));
	if (!err && avail) {
		ERR(suites.UtilitySuite6()->AEGP_ExecuteScript(S_my_id, script, FALSE, &resultH, &errorH));
		//	Product: stay silent on success; only log. Errors go to the log too.
		if (errorH) {
			A_char *t = NULL;
			if (!suites.MemorySuite1()->AEGP_LockMemHandle(errorH, reinterpret_cast<void**>(&t)) && t && t[0]) {
				Log("  anchor script error: %s\n", t);
			}
			ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(errorH));
			ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(errorH));
		}
		if (resultH) {
			A_char *t = NULL;
			if (!suites.MemorySuite1()->AEGP_LockMemHandle(resultH, reinterpret_cast<void**>(&t)) && t) {
				Log("  anchor cell %d (fx=%.1f fy=%.1f): %s\n", cell, fx, fy, t);
			}
			ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(resultH));
			ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(resultH));
		}
	}
}

// ---------------------------------------------------------------------------
//	AEGP hooks
// ---------------------------------------------------------------------------
static A_Err
IdleHook(AEGP_GlobalRefcon, AEGP_IdleRefcon, A_long *max_sleepPL)
{
	AEGP_SuiteHandler suites(sP);

	//	Backstop for a press whose UP we never saw. The thread-local WH_MOUSE hook
	//	only sees AE's thread, so a right-release over a NON-AE window is invisible
	//	to us: S_rdownB stays set and the wheel is left showing. Idle is starved
	//	during the modal press loop, so this can only evaluate once the press has
	//	actually ended - at which point, if we still think the button is down but
	//	it is physically up, the release happened off-AE. Treat it as a cancel.
	if (S_active && S_rdownB && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {
		CancelHoldTimer();
		if (S_hold_firedB) {
			SendCancel();
		}
		S_rdownB	  = FALSE;
		S_hold_firedB = FALSE;
		S_cur_cell	  = -1;
		Log("  backstop: right-up unseen (released off-AE) -> cancel, wheel hidden\n");
	}

	//	Keep selection context warm while armed (cheap; only when not dragging).
	if (S_active && !S_rdownB) {
		RefreshSelectionContext(suites);
	}

	//	Run any queued anchor move here, where AEGP calls are safe.
	if (S_action_pending) {
		S_action_pending = FALSE;
		int cell = S_action_cell;
		S_action_cell = -1;
		if (cell >= 0) {
			RunAnchorAction(suites, cell);
		}
	}

	if (max_sleepPL && S_active) {
		*max_sleepPL = 30;	// stay responsive while armed
	}
	return A_Err_NONE;
}

static A_Err
DeathHook(AEGP_GlobalRefcon, AEGP_DeathRefcon)
{
	CancelHoldTimer();
	if (S_mouse_hook) { UnhookWindowsHookEx(S_mouse_hook); S_mouse_hook = NULL; }
	S_active = FALSE;
	StopPipeServer();
	return A_Err_NONE;
}

static A_Err
UpdateMenuHook(AEGP_GlobalRefcon, AEGP_UpdateMenuRefcon, AEGP_WindowType)
{
	AEGP_SuiteHandler suites(sP);
	//	Always available - arming is a global mode, not a per-comp action.
	suites.CommandSuite1()->AEGP_EnableCommand(S_toggle_cmd);
	return A_Err_NONE;
}

static void
ResolveLogPath(void)
{
	DWORD len = GetTempPathA(MAX_PATH, S_log_path);
	if (!len || len > MAX_PATH - 24) { S_log_path[0] = 0; return; }
	strcat_s(S_log_path, MAX_PATH, "pieFX_poc.txt");
}

static void
ToggleActive(AEGP_SuiteHandler &suites)
{
	if (S_active) {
		//	Disarm.
		S_active = FALSE;
		CancelHoldTimer();
		if (S_mouse_hook) { UnhookWindowsHookEx(S_mouse_hook); S_mouse_hook = NULL; }
		SendCancel();
		StopPipeServer();
		Log("=== pieFX POC disarmed ===\n");
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, "pieFX POC: OFF.");
		return;
	}

	//	Arm.
	ResolveLogPath();
	FILE *fp = NULL;
	if (!fopen_s(&fp, S_log_path, "w") && fp) {
		fprintf(fp, "pieFX POC log. Hold %dms. Thread %lu.\n\n",
				PIEFX_HOLD_MS, (unsigned long)GetCurrentThreadId());
		fclose(fp);
	}

	S_mouse_hook = SetWindowsHookEx(WH_MOUSE, MouseProc, NULL, GetCurrentThreadId());
	if (!S_mouse_hook) {
		char m[160];
		sprintf_s(m, sizeof(m), "pieFX POC: SetWindowsHookEx failed, err=%lu",
				  (unsigned long)GetLastError());
		suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, m);
		return;
	}

	StartPipeServer();
	LaunchOverlay();

	S_active = TRUE;
	S_rdownB = FALSE;
	S_hold_firedB = FALSE;
	S_replay_pending = 0;
	Log("=== pieFX POC armed ===\n");

	char msg[MAX_PATH + 256];
	sprintf_s(msg, sizeof(msg),
		"pieFX POC: ON.\n\n"
		"Select a layer, then right-press-and-hold past %dms: the 3x3 anchor wheel\n"
		"appears under the cursor. Flick to a cell and release to move the anchor there.\n"
		"A quick right-click still opens AE's normal menu.\n\n"
		"If the wheel does not appear, run the overlay by hand (npm run tauri dev in\n"
		"poc/overlay) - it retries connecting.\n\nLog: %s",
		PIEFX_HOLD_MS, S_log_path);
	suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg);
}

static A_Err
CommandHook(AEGP_GlobalRefcon, AEGP_CommandRefcon, AEGP_Command command,
			AEGP_HookPriority, A_Boolean, A_Boolean *handledPB)
{
	AEGP_SuiteHandler suites(sP);
	if (command == S_toggle_cmd) {
		ToggleActive(suites);
		*handledPB = TRUE;
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
