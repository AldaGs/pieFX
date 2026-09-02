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

use tauri::{Emitter, Manager, PhysicalPosition, PhysicalSize};

// One pipe per direction. A single duplex pipe opened synchronously has its I/O
// serialised by Windows, so a thread parked in read blocks any write issued from
// another thread — and on the plug-in's side that write comes from AE's UI
// thread, which froze AE on the first summon. Separate handles, separate
// directions, no serialisation.
const PIPE_RX: &str = r"\\.\pipe\pieFX"; // plug-in -> us (events)
const PIPE_TX: &str = r"\\.\pipe\pieFX-cmd"; // us -> plug-in (actions)

// The plug-in passes the names it actually managed to create (`--rx` / `--tx`),
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
    (find("--rx", PIPE_RX), find("--tx", PIPE_TX))
}

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

#[tauri::command]
fn dbg(msg: String) {
    dlog(&format!("  js: {}", msg));
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

fn settings_path() -> Option<PathBuf> {
    let base = std::env::var_os("APPDATA")?;
    Some(PathBuf::from(base).join("pieFX").join("settings.json"))
}

// Returns the settings file's contents, or "" when there is none — the frontend
// falls back to its built-in DEFAULTS in that case.
#[tauri::command]
fn load_settings() -> String {
    settings_path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .unwrap_or_default()
}

#[tauri::command]
fn save_settings(json: String) -> Result<(), String> {
    let p = settings_path().ok_or("no APPDATA")?;
    if let Some(dir) = p.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    std::fs::write(p, json).map_err(|e| e.to_string())
}

fn pipe_client(app: tauri::AppHandle) {
    // Wait for the webview before touching the pipes.
    loop {
        if *app.state::<Ready>().0.lock().unwrap() {
            break;
        }
        thread::sleep(Duration::from_millis(25));
    }

    let (rx_name, tx_name) = pipe_names();
    dlog(&format!("  pipes: rx={} tx={}", rx_name, tx_name));

    loop {
        // Open the inbound pipe first — the plug-in accepts them in this order.
        match std::fs::OpenOptions::new().read(true).open(&rx_name) {
            Ok(file) => {
                // Then the outbound one. A separate handle, so writes from the
                // UI never queue behind the read parked below.
                match std::fs::OpenOptions::new().write(true).open(&tx_name) {
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
            fire_action,
            frontend_ready,
            dbg,
            load_settings,
            save_settings
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
            let handle = app.handle().clone();
            thread::spawn(move || pipe_client(handle));

            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
