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

const PIPE_NAME: &str = r"\\.\pipe\pieFX";

// Virtual-desktop origin in physical px; the frontend converts the plug-in's
// screen coordinates to window-local with it.
struct Origin(Mutex<(i32, i32)>);

// Write half of the connected pipe. Cloned from the reader's handle on connect.
struct Pipe(Mutex<Option<File>>);

#[tauri::command]
fn overlay_origin(state: tauri::State<Origin>) -> (i32, i32) {
    *state.0.lock().unwrap()
}

// Send one already-serialised message to the plug-in. The frontend builds the
// JSON because it owns the slot tree; this only frames and writes it.
#[tauri::command]
fn fire_action(json: String, state: tauri::State<Pipe>) -> Result<(), String> {
    let mut guard = state.0.lock().map_err(|e| e.to_string())?;
    match guard.as_mut() {
        Some(f) => {
            let mut line = json;
            line.push('\n');
            f.write_all(line.as_bytes()).map_err(|e| e.to_string())?;
            f.flush().map_err(|e| e.to_string())
        }
        None => Err("not connected to the plug-in".into()),
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
    loop {
        match std::fs::OpenOptions::new().read(true).write(true).open(PIPE_NAME) {
            Ok(file) => {
                // Keep a write handle so fire_action can reply on the same pipe.
                if let Ok(w) = file.try_clone() {
                    *app.state::<Pipe>().0.lock().unwrap() = Some(w);
                }
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
        .invoke_handler(tauri::generate_handler![
            overlay_origin,
            fire_action,
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

            let handle = app.handle().clone();
            thread::spawn(move || pipe_client(handle));

            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
