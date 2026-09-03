// pieFX — turning a slot's action into a message the plug-in can execute.
//
// Extracted from hexwheel.js so the settings window's TEST-FIRE button goes
// down the exact same path the gesture does. A test that fired through a
// second code path would be testing the wrong thing — and test-fire exists
// precisely because AE menu ids cannot be trusted without watching one work.


// Free text crosses the pipe base64-encoded: the native side hand-rolls its JSON
// parsing, and a correct unescaper for arbitrary user script is exactly the kind
// of thing that works until someone puts a quote in a string literal.
function b64(s) {
  return btoa(unescape(encodeURIComponent(s)));
}

// --- script bootstrap -----------------------------------------------------
// A snippet needs its script already loaded: `_mn.addMasterNull(...)` only
// works once ag_masterNull.jsx has been run, because AE shares one ExtendScript
// namespace and pieFX is not the thing that populated it. Before this, the
// first Master Null of every session toasted "_mn is undefined" and the second
// one worked — which reads as a flaky wheel rather than as a missing load.
//
// So an action may declare what it needs:
//
//   needs: { global: "_mn", file: "C:/…/ag_masterNull.jsx" }
//
// and the snippet is wrapped in a loader that runs the file ONLY when the
// global is absent. The wrapping happens here, in the overlay, because the
// whole thing crosses the pipe base64-encoded either way and the native side
// stays a dumb executor that knows nothing about scripts.
//
// $.global.__pieFXHeadless is set for the duration. That is the flag a script
// author guards their UI with —
//
//   if (!$.global.__pieFXHeadless) showUI(thisObj);
//
// — so that loading a panel script to reach its functions does not also pop its
// palette in the middle of a gesture. Scripts without the guard still work;
// they just show their window the first time.
// Set once at startup from the Rust side. Empty in a browser preview, where
// there is no install directory and a relative path just stays relative.
let INSTALL_DIR = "";

// Absolute stays absolute; a bare name is left for the script-side search of
// AE's folders; anything with a directory in it is ours, and belongs to the
// install.
function resolveScript(path) {
  const c0 = path.charAt(0);
  if (c0 === "/" || c0 === "~" || c0 === String.fromCharCode(92)) return path;
  if (path.charAt(1) === ":") return path;
  if (path.indexOf("/") < 0) return path;
  return INSTALL_DIR ? INSTALL_DIR + "/" + path : path;
}

function bootstrapped(action) {
  const n = action.needs;
  if (!n || !n.global || !n.file) return action.code;
  const g = JSON.stringify(n.global);
  const f = JSON.stringify(resolveScript(n.file));
  return (
    "$.global.__pieFXHeadless = true;" +
    "try {" +
    "  if ($.global[" + g + "] === undefined) {" +
    "    var __p = " + f + ", __f = new File(__p);" +
    // A bare filename is searched in AE's own script folders instead of being
    // a broken absolute path in someone else's settings file. The version
    // directory is whatever is there, so this works across AE versions and
    // across machines — which a hardcoded path never does.
        "    if (!__f.exists && __p.indexOf('/') < 0 && __p.indexOf(String.fromCharCode(92)) < 0) {" +
    "      var __ud = new Folder(Folder.userData.absoluteURI + '/Adobe/After Effects');" +
    "      var __vs = __ud.exists ? __ud.getFiles() : [];" +
    "      for (var __i = 0; __i < __vs.length && !__f.exists; __i++) {" +
    "        if (!(__vs[__i] instanceof Folder)) { continue; }" +
    "        var __c = [__vs[__i].absoluteURI + '/Scripts/ScriptUI Panels/' + __p," +
    "                   __vs[__i].absoluteURI + '/Scripts/' + __p];" +
    "        for (var __j = 0; __j < __c.length; __j++) {" +
    "          var __t = new File(__c[__j]);" +
    "          if (__t.exists) { __f = __t; break; }" +
    "        }" +
    "      }" +
    "    }" +
    "    if (!__f.exists) { throw new Error('pieFX: script not found: ' + __p); }" +
    "    $.evalFile(__f);" +
    "  }" +
    "  if ($.global[" + g + "] === undefined) {" +
    "    throw new Error('pieFX: loaded ' + " + f + " + ' but ' + " + g + " + ' is still undefined');" +
    "  }" +
    "  " + action.code + ";" +
    "} finally { $.global.__pieFXHeadless = false; }"
  );
}

// Returns a promise that settles when the plug-in has ACCEPTED the message —
// which is not the same as the action having worked, but is the difference
// between "the pipe is down" and "AE did nothing". The settings window's
// test-fire reports on it; the gesture ignores it.
function sendFire(action, cell) {
  if (!action) return Promise.reject(new Error("no action bound"));
  let m = null;
  switch (action.kind) {
    case "ae-command":
      // Name first, id as fallback. findMenuCommandId resolves the name against
      // the running AE, so a binding is checkable at the moment it is made — an
      // id can only be trusted, and the hand-tested map has already produced
      // three wrong entries and two duplicate names.
      m = { type: "fire", kind: "ae-command" };
      if (action.name) m.b64 = b64(action.name);
      if (action.id) m.id = action.id;
      break;
    case "script-snippet":
      m = { type: "fire", kind: "script-snippet", b64: b64(bootstrapped(action)) };
      break;
    case "script-file":
      m = { type: "fire", kind: "script-file", b64: b64(action.path) };
      break;
    case "effect":
      m = { type: "fire", kind: "effect", b64: b64(action.matchName) };
      break;
    // An animation preset is a FILE, applied through the scripting DOM's
    // layer.applyPreset — there is no AEGP call for it, which is also why
    // presets have to be found by scanning folders instead of enumerated. The
    // path crosses base64-encoded like every other free text.
    case "preset":
      m = { type: "fire", kind: "preset", b64: b64(action.path) };
      break;
    case "builtin":
      // The effect search never reaches the plug-in as an action: it is a
      // WINDOW this process opens, and the effect it eventually applies is
      // fired from there as an ordinary `effect`. Intercepted here rather than
      // in the wheel so that a test-fire from the settings window opens it too
      // — a binding you cannot try is a binding you cannot trust.
      if (action.name === "effect-search") {
        if (!(window.__TAURI__ && window.__TAURI__.core)) {
          return Promise.reject(new Error("not running under Tauri"));
        }
        return window.__TAURI__.core.invoke("open_search").then(
          () => ({ type: "open", name: "effect-search" }),
          (e) => {
            // Said out loud on the way past. A window that fails to open is
            // indistinguishable from a slot that fires nothing, which is
            // exactly what this slot used to be.
            if (window.__PIEFX_SAY__) window.__PIEFX_SAY__("open_search FAILED " + e);
            throw e;
          }
        );
      }
      m = {
        type: "fire",
        kind: "builtin",
        name: action.name,
        cell: cell === undefined || cell === null ? (action.cell ?? -1) : cell,
      };
      break;
    default:
      return Promise.reject(new Error("unknown action kind: " + action.kind));
  }
  if (window.__PIEFX_LOCAL__) console.log("FIRE:", m);
  if (!(window.__TAURI__ && window.__TAURI__.core)) {
    // Browser preview: there is no plug-in to fire into.
    return Promise.reject(new Error("not running under Tauri"));
  }
  return window.__TAURI__.core.invoke("fire_action", { json: JSON.stringify(m) }).then(
    () => m,
    (e) => {
      if (window.__PIEFX_SAY__) window.__PIEFX_SAY__("fire FAILED " + e);
      throw e;
    }
  );
}

// Set once at startup from the Rust side (`overlay_dir`), by whichever window
// gets there first.
export function setInstallDir(d) {
  INSTALL_DIR = d || "";
}

export { b64, resolveScript, bootstrapped, sendFire };
