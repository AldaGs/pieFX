// pieFX overlay — the wheel UI (Tauri v2).
//
// A transparent, click-through, always-on-top window spanning the WHOLE virtual
// desktop. The native AEGP plug-in owns all input and drives this over a named
// pipe (`\\.\pipe\pieFX`): the plug-in is the SERVER, we are the client.
//
// Traffic is bidirectional now:
//   native → us   summon / cursor / release / cancel
//   us → native   fire { action descriptor }      (see SETTINGS.md)
//
// The overlay owns the wheel geometry, hit-testing and the slot tree, so the
// native side never needs to know the layout — it is a dumb executor.

use std::fs::File;
use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;
use std::sync::Mutex;
use std::thread;
use std::time::Duration;

use tauri::{Emitter, Manager, PhysicalPosition, PhysicalSize, WebviewUrl};
use tauri::webview::WebviewWindowBuilder;

// One pipe per direction. A single duplex pipe opened synchronously has its I/O
// serialised by Windows, so a thread parked in read blocks any write issued from
// another thread — and on the plug-in's side that write comes from AE's UI
// thread, which froze AE on the first summon. Separate handles, separate
// directions, no serialisation.
const PIPE_EVENTS: &str = r"\\.\pipe\pieFX"; // plug-in -> us (summon/cursor/release)
const PIPE_ACTIONS: &str = r"\\.\pipe\pieFX-cmd"; // us -> plug-in (fire)

// The plug-in passes the names it actually managed to create (`--events` / `--actions`),
// because a second AE instance finds the base names taken and falls back to a
// pid-suffixed pair. Defaults are kept so an overlay started by hand — the dev
// flow — still connects to the first instance.
fn pipe_names() -> (String, String) {
    let args: Vec<String> = std::env::args().collect();
    let find = |flag: &str, fallback: &str| {
        args.iter()
            .position(|a| a == flag)
            .and_then(|i| args.get(i + 1))
            .cloned()
            .unwrap_or_else(|| fallback.to_string())
    };
    // Named after what FLOWS, not after whose end it is: tx/rx invert
    // between the two processes, and they duly disagreed.
    (find("--events", PIPE_EVENTS), find("--actions", PIPE_ACTIONS))
}

// The pid the plug-in passes with `--owner-pid`: the After Effects that
// launched us.
fn owner_pid() -> Option<u32> {
    let args: Vec<String> = std::env::args().collect();
    args.iter()
        .position(|a| a == "--owner-pid")
        .and_then(|i| args.get(i + 1))
        .and_then(|v| v.parse::<u32>().ok())
}

// Outlive nothing. The overlay is a windowed process with no UI of its own, so
// an orphan is invisible except as a name in Task Manager holding a pipe open —
// and AE would not finish quitting until it was killed by hand.
//
// The pipes cannot be the signal: they close on every DISARM, and surviving
// arm/disarm/arm is the whole reason the plug-in refuses to launch a second
// overlay. What the lifetime is actually tied to is the process that launched
// us, so that is what this waits on. It also covers an AE that crashes without
// ever reaching its death hook.
#[cfg(windows)]
fn watch_owner(pid: u32) {
    use std::os::windows::io::RawHandle;
    const SYNCHRONIZE: u32 = 0x0010_0000;
    const INFINITE: u32 = 0xFFFF_FFFF;
    extern "system" {
        fn OpenProcess(access: u32, inherit: i32, pid: u32) -> RawHandle;
        fn WaitForSingleObject(h: RawHandle, ms: u32) -> u32;
        fn CloseHandle(h: RawHandle) -> i32;
    }
    thread::spawn(move || unsafe {
        let h = OpenProcess(SYNCHRONIZE, 0, pid);
        if h.is_null() {
            dlog(&format!("  owner {} not open-able; no watchdog", pid));
            return;
        }
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        dlog(&format!("  owner {} exited -> quitting", pid));
        std::process::exit(0);
    });
}

#[cfg(not(windows))]
fn watch_owner(_pid: u32) {}

// Virtual-desktop origin in physical px; the frontend converts the plug-in's
// screen coordinates to window-local with it.
struct Origin(Mutex<(i32, i32)>);

// Write half of the connected pipe.
struct Pipe(Mutex<Option<File>>);

// Set once the webview has registered its event listener. The pipes are not
// opened until then: the plug-in's ConnectNamedPipe completing is what tells it
// the overlay can be driven, so connecting before the UI can receive would drop
// any summon sent in that window.
struct Ready(Mutex<bool>);

// Diagnostics. The overlay is a windowed app with no console, so without this
// there is no way to tell "the page never loaded" from "the page loaded but the
// action never fired" — and guessing between those is what costs an AE session.
fn dlog(s: &str) {
    if let Some(t) = std::env::var_os("TEMP") {
        let p = PathBuf::from(t).join("piefx_overlay.log");
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(p) {
            let _ = writeln!(f, "{}", s);
        }
    }
}

#[tauri::command]
fn frontend_ready(state: tauri::State<Ready>) {
    *state.0.lock().unwrap() = true;
    dlog("  frontend ready; opening pipes");
}

// Asked to go by the plug-in, which does it BEFORE tearing the pipes down so
// this process is not sitting in a read with no server on the other end. Exit
// is immediate and deliberate: there is nothing here worth saving, and a tidy
// unwind would have to wait on the very pipe thread we are trying not to be
// stuck in.
#[tauri::command]
fn quit_overlay() {
    dlog("  quit requested by the plug-in -> exiting");
    std::process::exit(0);
}

#[tauri::command]
fn dbg(msg: String) {
    dlog(&format!("  js: {}", msg));
}

// Where we are installed, which is beside the .aex — the plug-in launches us
// from there. Scripts that SHIP with pieFX are resolved against this, so a
// binding can say "scripts/ag_masterNull.jsx" and travel with the product
// instead of naming one machine's Dropbox.
#[tauri::command]
fn overlay_dir() -> String {
    let d = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .map(|d| d.to_string_lossy().replace(std::path::MAIN_SEPARATOR, "/"))
        .unwrap_or_default();
    dlog(&format!("  overlay_dir -> {}", d));
    d
}

#[tauri::command]
fn overlay_origin(state: tauri::State<Origin>) -> (i32, i32) {
    let o = *state.0.lock().unwrap();
    dlog(&format!("  overlay_origin -> {:?}", o));
    o
}

// Send one already-serialised message to the plug-in. The frontend builds the
// JSON because it owns the slot tree; this only frames and writes it.
#[tauri::command]
fn fire_action(json: String, state: tauri::State<Pipe>) -> Result<(), String> {
    let mut guard = state.0.lock().map_err(|e| e.to_string())?;
    dlog(&format!("  fire_action <- {}", json));
    match guard.as_mut() {
        Some(f) => {
            let mut line = json;
            line.push('\n');
            f.write_all(line.as_bytes()).map_err(|e| e.to_string())?;
            f.flush().map_err(|e| e.to_string())
        }
        None => {
            dlog("  fire_action FAILED: no write handle");
            Err("not connected to the plug-in".into())
        }
    }
}

// The settings window. A SECOND window in this same process, not a second
// process: it must read and write the same settings file the wheel is using,
// and two processes racing on one file is a bug waiting for the first time
// someone leaves the configurator open.
//
// Everything the overlay window is, this one is not — decorated, focusable,
// opaque, not always-on-top, and it takes the mouse.
//
// It also has to be RAISED by hand, which is the part that is not obvious.
// Windows refuses SetForegroundWindow from a process that does not own the
// foreground, and the process that owns it is After Effects - the user's click
// went to AE's menu, not to us. So the window was created, correctly, BEHIND
// the app that asked for it, and reads as "the settings never opened".
//
// Two steps, because they fix two different things:
//
//   1. A bounce through always-on-top. SetWindowPos changing Z-ORDER needs no
//      foreground rights at all, so this is what actually puts the window in
//      front of AE. It is dropped again immediately: a settings window that
//      floated over everything forever would be its own bug.
//   2. force_foreground, for the input FOCUS, which z-order does not give.
//      Without it the window is visible but the first keystroke still goes to
//      After Effects.
fn raise(w: &tauri::WebviewWindow) {
    let _ = w.show();
    let _ = w.unminimize();
    let _ = w.set_always_on_top(true);
    let _ = w.set_always_on_top(false);
    let _ = w.set_focus();
    #[cfg(windows)]
    {
        match w.hwnd() {
            Ok(h) => force_foreground(h.0 as isize),
            Err(e) => dlog(&format!("  no hwnd for settings window: {}", e)),
        }
    }
}

// The AttachThreadInput dance. Windows grants SetForegroundWindow only to a
// process that already has the foreground, or shares an input queue with the
// one that does - so we borrow the foreground thread's input queue for exactly
// as long as it takes to make the call, and give it straight back.
//
// This must run on the thread that OWNS the window, which is why show_settings
// is always reached on the main thread (the pipe reader hops via
// run_on_main_thread; a sync #[tauri::command] is already there).
#[cfg(windows)]
fn force_foreground(hwnd: isize) {
    extern "system" {
        fn GetForegroundWindow() -> isize;
        fn GetWindowThreadProcessId(hwnd: isize, pid: *mut u32) -> u32;
        fn GetCurrentThreadId() -> u32;
        fn AttachThreadInput(attach: u32, attach_to: u32, do_attach: i32) -> i32;
        fn BringWindowToTop(hwnd: isize) -> i32;
        fn SetForegroundWindow(hwnd: isize) -> i32;
    }
    unsafe {
        let fg = GetForegroundWindow();
        let me = GetCurrentThreadId();
        if fg == 0 {
            SetForegroundWindow(hwnd);
            return;
        }
        let fg_thread = GetWindowThreadProcessId(fg, std::ptr::null_mut());
        // Already ours: no borrowing needed, and attaching a thread to itself
        // is an error.
        if fg_thread == 0 || fg_thread == me {
            SetForegroundWindow(hwnd);
            return;
        }
        AttachThreadInput(me, fg_thread, 1);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        AttachThreadInput(me, fg_thread, 0);
    }
}

fn show_settings(app: &tauri::AppHandle) {
    if let Some(w) = app.get_webview_window("settings") {
        raise(&w);
        dlog("  settings window refocused");
        return;
    }
    match WebviewWindowBuilder::new(app, "settings", WebviewUrl::App("settings.html".into()))
        .title("pieFX Settings")
        .inner_size(1180.0, 760.0)
        .min_inner_size(920.0, 620.0)
        .resizable(true)
        .decorations(true)
        .transparent(false)
        .always_on_top(false)
        .skip_taskbar(false)
        .focused(true)
        .center()
        .build()
    {
        // .focused(true) at build time is not enough, for the same reason: it
        // asks Windows for the foreground from a process that is not allowed
        // to have it. The window is raised explicitly after it exists.
        Ok(w) => {
            raise(&w);
            dlog("  settings window opened and raised");
        }
        Err(e) => dlog(&format!("  settings window FAILED: {}", e)),
    }
}

// Asked for by the plug-in's `Window > pieFX Settings` menu item, which arrives
// down the events pipe like any other message.
#[tauri::command]
async fn open_settings(app: tauri::AppHandle) {
    let h = app.clone();
    let _ = app.run_on_main_thread(move || show_settings(&h));
}

// The effect search. A THIRD window, built the same way and for the same
// reason: it needs a keyboard, and nothing else in pieFX can take a keystroke.
// The overlay is click-through and unfocused by construction, the plug-in
// hooks WH_MOUSE only, and the gesture is a press-and-hold — so the search
// field cannot live on the wheel, and this is the one input path the project
// has already watched working in AE. `raise()` is what makes it come to the
// front from a process AE owns the foreground of; without it the window opens
// behind After Effects, which is indistinguishable from not opening at all.
fn show_search(app: &tauri::AppHandle) {
    if let Some(w) = app.get_webview_window("search") {
        // Dismissing the search HIDES it rather than closing it, so a second
        // summon is instant and the field is already alive. A hidden window
        // cannot be raised into view, so show it before asking for the front.
        let _ = w.show();
        raise(&w);
        let _ = app.emit("piefx-search-shown", "");
        dlog("  search window refocused");
        return;
    }
    match WebviewWindowBuilder::new(app, "search", WebviewUrl::App("search.html".into()))
        .title("pieFX — Apply Effect")
        .inner_size(560.0, 560.0)
        .min_inner_size(420.0, 360.0)
        .resizable(true)
        .decorations(true)
        .transparent(false)
        .always_on_top(false)
        .skip_taskbar(false)
        .focused(true)
        .center()
        .build()
    {
        Ok(w) => {
            raise(&w);
            dlog("  search window opened and raised");
        }
        Err(e) => dlog(&format!("  search window FAILED: {}", e)),
    }
}

#[tauri::command]
// ASYNC, and posting to the main thread rather than building the window here.
// Both halves are load-bearing, and the harness found out why by wedging.
//
// A SYNC #[tauri::command] runs ON the main thread, so building the window
// inside it means asking the event loop to create a webview from inside the
// event loop's own IPC handler - and `run_on_main_thread` from the main thread
// waits for a turn that will never come. Either way the process goes deaf: no
// window, no error, no reply to the next `quit`, which is precisely the shape
// of the un-dead-overlay bug this project has already paid for once.
//
// An `async` command runs OFF the main thread, so run_on_main_thread dispatches
// and returns, and the window is built by the loop on its own turn. The pipe
// path had this right all along (it hands show_settings to run_on_main_thread
// from the pipe thread); the JS command path did not, because until the search
// existed nothing had ever invoked one.
async fn open_search(app: tauri::AppHandle) {
    dlog("  open_search requested");
    let h = app.clone();
    match app.run_on_main_thread(move || show_search(&h)) {
        Ok(()) => dlog("  open_search queued on the main thread"),
        Err(e) => dlog(&format!("  open_search could not be queued: {}", e)),
    }
}

// `--settings <path>` overrides where settings come from, and `--settings none`
// means "there are none, use the built-in defaults".
//
// This exists for the offline harness. Once the wheel reads a settings file, a
// harness that asserts on the DEFAULT tree starts failing on any machine whose
// owner has configured anything - and the failure would look exactly like a
// transport bug, which is the one thing that harness is for. A test that
// depends on the developer's own settings is not a test.
fn settings_arg() -> Option<String> {
    let args: Vec<String> = std::env::args().collect();
    args.iter()
        .position(|a| a == "--settings")
        .and_then(|i| args.get(i + 1))
        .cloned()
}

fn settings_path() -> Option<PathBuf> {
    match settings_arg() {
        Some(v) if v == "none" => None,
        Some(v) => Some(PathBuf::from(v)),
        None => {
            let base = std::env::var_os("APPDATA")?;
            Some(PathBuf::from(base).join("pieFX").join("settings.json"))
        }
    }
}

// Returns the settings file's contents, or "" when there is none — the frontend
// falls back to its built-in DEFAULTS in that case.
#[tauri::command]
fn load_settings() -> String {
    if matches!(settings_arg().as_deref(), Some("none")) {
        dlog("  load_settings: --settings none -> built-in defaults");
        return String::new();
    }
    let s = settings_path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .unwrap_or_default();
    dlog(&format!("  load_settings: {} bytes", s.len()));
    s
}

// Written, then BROADCAST. Without the broadcast the wheel would keep serving
// the tree it loaded at startup, and a user who just rebound a hexagon would
// flick at it and get the old action — which reads as "settings do not work"
// rather than as "settings are cached".
#[tauri::command]
fn save_settings(app: tauri::AppHandle, json: String) -> Result<(), String> {
    let p = settings_path().ok_or("settings are disabled (--settings none)")?;
    if let Some(dir) = p.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    std::fs::write(&p, json).map_err(|e| e.to_string())?;
    dlog(&format!("  settings saved -> {}", p.display()));
    let _ = app.emit("piefx-settings", "saved");
    Ok(())
}

// The installed-effects catalogue, written by the plug-in once per session
// (see WriteEffectCatalogue in the native side). Read, never written, here.
//
// `--effects <path>` is the harness's way in, for the same reason `--settings`
// exists: the search UI can then be driven with a KNOWN catalogue instead of
// whatever this machine happens to have installed. `--effects none` means "no
// catalogue", which is also what a machine that has never armed pieFX looks
// like, and the window has to say so rather than showing an empty list that
// reads as "no effects installed".
fn effects_arg() -> Option<String> {
    let args: Vec<String> = std::env::args().collect();
    args.iter()
        .position(|a| a == "--effects")
        .and_then(|i| args.get(i + 1))
        .cloned()
}

fn effects_path() -> Option<PathBuf> {
    match effects_arg() {
        Some(v) if v == "none" => None,
        Some(v) => Some(PathBuf::from(v)),
        None => {
            let base = std::env::var_os("APPDATA")?;
            Some(PathBuf::from(base).join("pieFX").join("effects.json"))
        }
    }
}

#[tauri::command]
fn load_effects() -> String {
    let s = effects_path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .unwrap_or_default();
    dlog(&format!("  load_effects: {} bytes", s.len()));
    s
}

// Recents live in their own file rather than in settings.json, because the
// settings window writes that file WHOLE: a search window saving a recent
// while the settings window is open would be a lost-update race between two
// windows of the same process. A separate file has one writer.
fn recents_path() -> Option<PathBuf> {
    match settings_arg() {
        Some(v) if v == "none" => None,
        Some(v) => Some(PathBuf::from(v).with_file_name("recents.json")),
        None => {
            let base = std::env::var_os("APPDATA")?;
            Some(PathBuf::from(base).join("pieFX").join("recents.json"))
        }
    }
}

#[tauri::command]
fn load_recents() -> String {
    let s = recents_path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .unwrap_or_default();
    dlog(&format!("  load_recents: {} bytes", s.len()));
    s
}

#[tauri::command]
fn save_recents(json: String) -> Result<(), String> {
    let p = recents_path().ok_or("recents are disabled (--settings none)")?;
    if let Some(dir) = p.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    std::fs::write(&p, json).map_err(|e| e.to_string())?;
    dlog(&format!("  recents saved -> {}", p.display()));
    Ok(())
}

fn pipe_client(app: tauri::AppHandle) {
    // Wait for the webview before touching the pipes.
    loop {
        if *app.state::<Ready>().0.lock().unwrap() {
            break;
        }
        thread::sleep(Duration::from_millis(25));
    }

    let (events_name, actions_name) = pipe_names();
    dlog(&format!("  pipes: events={} actions={}", events_name, actions_name));

    loop {
        // Open the inbound pipe first — the plug-in accepts them in this order.
        match std::fs::OpenOptions::new().read(true).open(&events_name) {
            Ok(file) => {
                // Then the outbound one. A separate handle, so writes from the
                // UI never queue behind the read parked below.
                match std::fs::OpenOptions::new().write(true).open(&actions_name) {
                    Ok(w) => *app.state::<Pipe>().0.lock().unwrap() = Some(w),
                    Err(_) => {
                        thread::sleep(Duration::from_millis(500));
                        continue;
                    }
                }
                dlog("  pipe: both handles open");
                let _ = app.emit("piefx-link", "connected");

                let mut reader = BufReader::new(file);
                let mut line = String::new();
                loop {
                    line.clear();
                    match reader.read_line(&mut line) {
                        Ok(0) => break, // server closed the pipe
                        Ok(_) => {
                            let msg = line.trim();
                            if !msg.is_empty() {
                                dlog(&format!("  emit -> {}", msg));
                                // The settings window is opened HERE rather than
                                // from the wheel's JS: the wheel is a
                                // click-through renderer and has no business
                                // owning another window's lifetime.
                                if msg.contains("\"type\":\"settings\"") {
                                    let h = app.clone();
                                    let _ = app.run_on_main_thread(move || show_settings(&h));
                                }
                                let _ = app.emit("piefx", msg.to_string());
                            }
                        }
                        Err(_) => break,
                    }
                }

                *app.state::<Pipe>().0.lock().unwrap() = None;
                let _ = app.emit("piefx-link", "disconnected");
            }
            Err(_) => { /* pipe not up yet (plug-in not armed) — retry */ }
        }
        thread::sleep(Duration::from_millis(500));
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .manage(Origin(Mutex::new((0, 0))))
        .manage(Pipe(Mutex::new(None)))
        .manage(Ready(Mutex::new(false)))
        .invoke_handler(tauri::generate_handler![
            overlay_origin,
            overlay_dir,
            fire_action,
            frontend_ready,
            dbg,
            load_settings,
            save_settings,
            open_settings,
            open_search,
            load_effects,
            load_recents,
            save_recents,
            quit_overlay
        ])
        .setup(|app| {
            let win = app
                .get_webview_window("main")
                .expect("main window is declared in tauri.conf.json");

            // Span the union of all monitors so a summon on ANY display is
            // inside the canvas. Physical px.
            let (mut minx, mut miny, mut maxx, mut maxy) =
                (i32::MAX, i32::MAX, i32::MIN, i32::MIN);
            if let Ok(monitors) = win.available_monitors() {
                for m in &monitors {
                    let p = m.position();
                    let s = m.size();
                    minx = minx.min(p.x);
                    miny = miny.min(p.y);
                    maxx = maxx.max(p.x + s.width as i32);
                    maxy = maxy.max(p.y + s.height as i32);
                }
            }
            if minx != i32::MAX {
                let _ = win.set_position(PhysicalPosition::new(minx, miny));
                let _ = win.set_size(PhysicalSize::new((maxx - minx) as u32, (maxy - miny) as u32));
                *app.state::<Origin>().0.lock().unwrap() = (minx, miny);
            }

            // The plug-in owns the mouse; the overlay must never steal it while
            // the right button is held. It is a pure renderer.
            let _ = win.set_ignore_cursor_events(true);

            dlog("=== overlay started ===");
            match owner_pid() {
                Some(pid) => {
                    dlog(&format!("  owner pid {}", pid));
                    watch_owner(pid);
                }
                // Hand-started for development: nothing to outlive, so nothing
                // to watch. Closing it is the developer's job.
                None => dlog("  no --owner-pid; running unowned (dev)"),
            }
            let handle = app.handle().clone();
            thread::spawn(move || pipe_client(handle));

            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
