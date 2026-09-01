// pieFX overlay — the wheel UI (Tauri v2).
//
// A transparent, click-through, always-on-top window spanning the WHOLE virtual
// desktop (all monitors). The native AEGP plug-in owns all input and drives this
// over a named pipe (`\\.\pipe\pieFX`): the plug-in is the SERVER, we are the
// client.
//
// This side is deliberately dumb: it connects (retrying until the pipe exists),
// reads newline-delimited JSON messages, and forwards each one verbatim to the
// webview as a `piefx` event. All wheel logic lives in ../src/main.js.
//
// The plug-in sends cursor positions in physical screen pixels (raw mouse-hook
// coordinates). The window is positioned at the virtual-desktop origin, which can
// be negative or nonzero, so the frontend needs that origin to convert screen
// coords to window-local. We expose it via the `overlay_origin` command.

use std::io::{BufRead, BufReader};
use std::sync::Mutex;
use std::thread;
use std::time::Duration;

use tauri::{Emitter, Manager, PhysicalPosition, PhysicalSize};

const PIPE_NAME: &str = r"\\.\pipe\pieFX";

// Virtual-desktop origin in physical px, set at startup, read by the frontend.
struct Origin(Mutex<(i32, i32)>);

#[tauri::command]
fn overlay_origin(state: tauri::State<Origin>) -> (i32, i32) {
    *state.0.lock().unwrap()
}

fn pipe_client(app: tauri::AppHandle) {
    loop {
        match std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(PIPE_NAME)
        {
            Ok(file) => {
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
        .invoke_handler(tauri::generate_handler![overlay_origin])
        .setup(|app| {
            let win = app
                .get_webview_window("main")
                .expect("main window is declared in tauri.conf.json");

            // Span the union of all monitors so a summon on ANY display is inside
            // the canvas. Coordinates are physical px.
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
                let _ = win.set_size(PhysicalSize::new(
                    (maxx - minx) as u32,
                    (maxy - miny) as u32,
                ));
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
