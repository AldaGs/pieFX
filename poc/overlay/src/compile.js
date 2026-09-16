// pieFX — compiling a list of steps into ONE ExtendScript snippet.
//
// WHY THIS EXISTS. The search window can already apply an effect (AEGP, by
// match name), a preset (the scripting DOM, by path) and run a menu command
// (app.executeCommand, by id) — three fire kinds, one action each. What it
// could not do is anything that needs MORE THAN ONE THING TO HAPPEN AS A UNIT:
// apply Gaussian Blur *and set its blurriness to 40*, or run three steps that
// one Ctrl+Z takes back together. Both are one problem — a sequence — and
// ExtendScript is the only one of the three paths that can express a sequence,
// because it can do all three things itself:
//
//   effect    layer.property("ADBE Effect Parade").addProperty(matchName)
//   preset    layer.applyPreset(file)
//   command   app.executeCommand(id)
//
// So a parameterised effect is a one-step sequence, a macro is an n-step one,
// and both compile to a single `script-snippet` — a fire kind that already
// exists and is the one this project has watched working longest. No new pipe
// message, no new executor, no fourth path to keep correct.
//
// THE ONE THING OUTSIDE THIS: pieFX's own builtins (the anchor grid, the frame
// on the clipboard) are native, not ExtendScript, and cannot go in a snippet.
// A sequence containing one is rejected here rather than half-compiled.
//
// WHY ERRORS THROW RATHER THAN RETURN. `RunScript` on the native side already
// toasts whatever a script throws — that is how a snippet whose script has not
// been loaded reports itself today. Returning a string instead would need a
// new branch in the executor to notice it. Throwing means a parameter that did
// not land says so on screen, in the toast the user already recognises, with
// no C++ change at all.

// Every string that goes into generated source goes through here. ExtendScript
// accepts JSON's string escaping, and hand-rolling quoting for a Windows path
// with a user's name in it is how this project would find out about someone
// called O'Brien.
// The generated source is then forced to ASCII, `\uXXXX` for everything above
// 127. It does not have to be: the snippet crosses the pipe base64-encoded as
// UTF-8 and is decoded into a char buffer. But what AE's ExecuteScript does
// with UTF-8 BYTES in source is not something this project has measured, and it
// has already paid for one encoding assumption - the macOS port found preset
// names stored DECOMPOSED and a search that could not match them. A `\u` escape
// is the one spelling that cannot be misread, and a user preset folder under a
// name with an accent in it is not an edge case.
function ascii(s) {
  return String(s).replace(/[^\x20-\x7e]/g, (ch) =>
    "\\u" + ch.charCodeAt(0).toString(16).padStart(4, "0")
  );
}

function q(s) {
  return ascii(JSON.stringify(String(s)));
}

// Forward slashes: ExtendScript takes them on Windows too, and it removes
// backslash-escaping from the generated source entirely. The native preset
// executor does the same thing for the same reason.
function fwd(s) {
  return String(s).replace(/\\/g, "/");
}

// The parameter setter, emitted once and shared by every effect step.
//
// It looks the property up by NAME on the effect that was just added, never by
// index, and that is the whole design. `effects.json` carries a name, a match
// name and a category — nothing about properties — so an index would be a
// guess, and an effect's first property is very often a group or a checkbox
// rather than the thing anyone means by "40". Looking it up against the LIVE
// effect also means this works for a third-party plug-in nobody has ever
// enumerated.
//
// ES3, deliberately: ExtendScript has no `forEach`, no `let`, no arrow, and
// finding that out from a user's toast is the expensive way.
const HELPERS =
  "function __nums(fx){var a=[],k,p;" +
  "for(k=1;k<=fx.numProperties;k++){p=fx.property(k);" +
  "if(p&&p.propertyValueType===PropertyValueType.OneD){a[a.length]=p;}}" +
  "return a;}" +
  "function __set(fx,ps,notes){var nums=__nums(fx),pos=0,i,j,t,w;" +
  "for(i=0;i<ps.length;i++){w=ps[i];t=null;" +
  // A named parameter is matched as a case-insensitive SUBSTRING, so "blur"
  // finds "Blurriness" and nobody has to know AE's exact wording.
  "if(w.name){for(j=0;j<nums.length;j++){" +
  "if(String(nums[j].name).toLowerCase().indexOf(w.name)>=0){t=nums[j];break;}}}" +
  "else{t=nums[pos];pos++;}" +
  "if(!t){notes[notes.length]=fx.name+': no '+(w.name?'\"'+w.name+'\"':'settable')+' parameter';continue;}" +
  "try{t.setValue(w.value);}catch(e){notes[notes.length]=fx.name+' - '+t.name+': '+e.toString();}}}";

// A step is one of:
//   {t:"effect",  id:matchName, params:[{name?, value}]}
//   {t:"preset",  id:path}
//   {t:"command", cmdId:number}
//
// Effects and presets run ONCE PER SELECTED LAYER; a command runs once, full
// stop, because a menu command acts on the selection itself and running
// "Precompose" once per layer would precompose the same selection N times.
function step(s) {
  // Every per-layer body is wrapped, and the note carries the LAYER NAME. Not
  // every selected layer can take every step: a camera and a light have no
  // effect parade at all, and an unwrapped throw on layer 2 of 5 would abandon
  // layers 3 to 5 inside an undo group that had already half-run. Wrapped, a
  // mixed selection does what it can and names what it could not.
  const perLayer = (body) =>
    "for(li=0;li<sel.length;li++){try{" +
    body +
    "}catch(e){notes[notes.length]=sel[li].name+': '+e.toString();}}";

  if (s.t === "effect") {
    return perLayer(
      "fx=sel[li].property(" + q("ADBE Effect Parade") + ").addProperty(" + q(s.id) + ");" +
        (s.params && s.params.length
          ? "__set(fx," + ascii(JSON.stringify(s.params)) + ",notes);"
          : "")
    );
  }
  if (s.t === "preset") {
    return (
      "f=new File(" + q(fwd(s.id)) + ");" +
      "if(!f.exists){notes[notes.length]='preset not found: '+f.fsName;}" +
      "else{" + perLayer("sel[li].applyPreset(f);") + "}"
    );
  }
  if (s.t === "command") {
    return "app.executeCommand(" + Number(s.cmdId) + ");";
  }
  throw new Error("pieFX cannot put a " + s.t + " in a script: " + JSON.stringify(s));
}

// The native side decodes a snippet into a 4096-byte buffer and toasts "script
// too long for one action" when it does not fit — so overflow is already
// caught and already visible. This is the same limit said EARLIER and in terms
// of what the user did, because "your macro has too many steps" is actionable
// and "script too long" is not.
const MAX_SOURCE = 3900;

export function compile(steps, label) {
  if (!steps || !steps.length) throw new Error("nothing to compile");

  const body = steps.map(step).join("");
  const code =
    "(function(){" +
    HELPERS +
    "var c=app.project.activeItem;" +
    "if(!(c&&c instanceof CompItem)){throw new Error('pieFX: no comp is active');}" +
    "var sel=c.selectedLayers;" +
    "if(!sel||sel.length===0){throw new Error('pieFX: select a layer first');}" +
    "var notes=[],li,fx,f;" +
    // ONE undo group around the whole sequence, which is the point: one Ctrl+Z
    // takes back everything the step list did, exactly as one takes back an
    // applied preset today.
    "app.beginUndoGroup(" + q(label || "pieFX") + ");" +
    "try{" + body + "}finally{app.endUndoGroup();}" +
    // Thrown AFTER the undo group closes, so a parameter that would not set
    // still leaves the effect applied and undoable. Half-done and said out
    // loud beats half-done and silent — a parameter is a refinement, and
    // throwing the effect away because its number did not land would be a
    // worse answer than applying it at its defaults.
    "if(notes.length){throw new Error('pieFX: '+notes.join('; '));}" +
    "return 'ok';" +
    "})()";

  if (code.length > MAX_SOURCE) {
    throw new Error(
      "too much for one action (" + code.length + " characters, limit " + MAX_SOURCE + ")"
    );
  }
  return code;
}

// What the caller fires. Kept here so nothing outside has to know that a
// parameterised effect is "really" a snippet.
export function compileAction(steps, label) {
  return { kind: "script-snippet", code: compile(steps, label) };
}
