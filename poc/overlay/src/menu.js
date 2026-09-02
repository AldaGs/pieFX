// pieFX — the slot tree: defaults, and the compile step from the on-disk shape
// to the runtime one.
//
// Extracted from hexwheel.js so the SETTINGS WINDOW and the LIVE WHEEL read
// exactly the same model. Two copies of this would drift, and the copy that
// drifted would be the one the user configured against.
//
// See SETTINGS.md for the format this file is the reference implementation of.

// --- the menu -------------------------------------------------------------
// kind: "verb"   fires on release
//       "ring"   opens six more hexagons
//       "widget" opens tool-specific UI (NOT hexagons)
//
// `def` on a ring/widget is its DEFAULT action: flick to it and release without
// drilling in, and this fires. So a category costs one flick for the common
// case and two only when you want a variant — the 80% case stays as fast as a
// verb. Slots are positional (0..5 = N,NE,SE,S,SW,NW); null is a deliberate
// hole so adding an item later does not move the others.
// An `action` is one of six kinds — see SETTINGS.md. A slot with `slots` is a
// ring; it may ALSO carry an action, which is its default (fired when you flick
// to it and release without drilling in).
//
//   builtin        { name: "anchor-grid" | "effect-search" }
//   ae-command     { name: "Add to Render Queue", id: 2161 }  name first, id is fallback
//   script-snippet { code: "_mn.addMasterNull(false)" }
//   script-file    { path: "D:/…/ag_masterNull.jsx" }
//   effect         { matchName: "ADBE Gaussian Blur 2" }
//
// `requires` says what a slot needs before it can fire: "selection" (one or
// more layers selected) or "comp" (a comp is the active item). The plug-in
// sends both facts with every summon, so a slot that cannot apply is drawn
// dead and refuses to fire, instead of firing into nothing and reporting
// success. Omit it for actions that need nothing at all.
// Every Master Null variant is a call into the same script, so the load
// declaration is written once. Three forms of `file`, in order of how portable
// they are:
//
//   "scripts/ag_masterNull.jsx"  RELATIVE — resolved against pieFX's install
//                                directory, so the script travels with the
//                                product. What anything shipped should use.
//   "ag_masterNull.jsx"          bare — searched in AE's own script folders
//                                (ScriptUI Panels first), for a script the user
//                                already has.
//   "D:/…/ag_masterNull.jsx"     absolute — one machine's layout, and the thing
//                                the other two exist to avoid.
const MN = { global: "_mn", file: "scripts/ag_masterNull.jsx" };

const DEFAULTS = {
  version: 1,
  // armOnLaunch is read by the NATIVE plug-in, not by the overlay: it is the
  // one setting that has to be honoured before the overlay exists at all. It
  // ships on, so an install is "restart AE and flick" rather than "remember to
  // arm it first" — Window > pieFX is still the manual toggle and the way out.
  gesture: { holdMs: 200, armMode: "distance", armOnLaunch: true },
  appearance: { accent: "#C74FD6" },
  wheel: {
    slots: [
      // N
      {
        label: "Effects",
        requires: "selection",
        action: { kind: "builtin", name: "effect-search" },
        widget: "search",
      },
      // NE — from ag_masterNull.jsx. Its two orthogonal axes (placement and
      // parenting) are modifier chords there; a ring replaces the chord with a
      // direction. The script exposes `_mn`, so every variant is one snippet.
      {
        label: "Master Null",
        requires: "selection",
        action: { kind: "script-snippet", code: "_mn.addMasterNull(false)", needs: MN },
        slots: [
          {
            label: "Comp Center",
            action: { kind: "script-snippet", code: "_mn.addMasterNull(true)", needs: MN },
          },
          {
            label: "Area Center",
            action: {
              kind: "script-snippet",
              code: "_mn.addMasterNull(false,{useArea:true,skipParenting:false,forceReparent:false})",
              needs: MN,
            },
          },
          null,
          {
            label: "No Parenting",
            action: {
              kind: "script-snippet",
              code: "_mn.addMasterNull(false,{useArea:false,skipParenting:true,forceReparent:false})",
              needs: MN,
            },
          },
          {
            label: "Force Reparent",
            action: {
              kind: "script-snippet",
              code: "_mn.addMasterNull(false,{useArea:false,skipParenting:false,forceReparent:true})",
              needs: MN,
            },
          },
          null,
        ],
      },
      // SE
      {
        label: "Create",
        requires: "comp",
        slots: [
          // Every name here was resolved by app.findMenuCommandId on AE 2026;
          // the ids are what it returned, kept only as a locale fallback.
          //
          // SOLID IS THE EXCEPTION, and it is the counter-example to the
          // resolve-by-name rule this file argues for. "Solid..." resolves —
          // to 3000 — and 3000 makes a solid in the PROJECT and never puts it
          // in the comp. Resolution is not correctness: it proves a menu item
          // by that name exists, not that it is the one you meant, and AE has
          // more than one. So this binding carries NO name, because the name
          // that resolves is the wrong command; the id has to win.
          //
          // 2038 is the command map's "Solid" and is the remaining candidate
          // for Layer > New > Solid. Unconfirmed. If it is wrong too, the
          // dialog-free fallback is a snippet:
          //   comp.layers.addSolid([1,1,1], 'Solid', comp.width, comp.height, 1)
          { label: "Solid", action: { kind: "ae-command", id: 2038 } },
          { label: "Null", action: { kind: "ae-command", name: "Null Object", id: 2767 } },
          {
            label: "Adjustment Layer",
            action: { kind: "ae-command", name: "Adjustment Layer", id: 2279 },
          },
          // Replaced Text (7034, which does resolve if it is ever wanted
          // back). The one Create item that needs nothing: it MAKES the comp,
          // so an explicit null overrides the category's inherited "comp".
          // Measured: "New Composition..." resolves at 2000, and the bare
          // "New Composition" does not.
          {
            label: "Comp",
            requires: null,
            action: { kind: "ae-command", name: "New Composition...", id: 2000 },
          },
          { label: "Light", action: { kind: "ae-command", name: "Light...", id: 2563 } },
          { label: "Camera", action: { kind: "ae-command", name: "Camera...", id: 2564 } },
        ],
      },
      // S — a ring now, but its DEFAULT is still Add to Render Queue, so the
      // flick that has always meant "queue this" still means it. That is the
      // whole point of a category carrying a default: new items can arrive
      // without moving what is already in the hands.
      //
      // Comp Settings does not belong under `Layer` — it acts on the comp,
      // not on what is selected inside it. Save Frame as PNG moved here for
      // the same reason.
      {
        label: "Comp",
        requires: "comp",
        action: { kind: "ae-command", name: "Add to Render Queue", id: 2161 },
        slots: [
          {
            label: "Comp Settings",
            action: { kind: "ae-command", name: "Composition Settings...", id: 2007 },
          },
          null,
          {
            // NOT the menu command. "Save Frame As" (2233, and the spelling
            // really does drop the ellipsis the menu shows) only queues the
            // frame in the Render Queue, where it waits for a second gesture
            // that has nothing to do with this one. This writes the PNG.
            //
            // resolutionFactor is forced to 1:1 and restored, because
            // saveFrameToPng honours it: exporting at Half because that is how
            // you happened to be previewing is a silent wrong answer, and the
            // slot says "full res".
            label: "Save Frame as PNG",
            action: {
              kind: "script-snippet",
              code:
                "(function(){" +
                "var c=app.project.activeItem;" +
                "if(!(c&&c instanceof CompItem))return'pieFX: no comp is active';" +
                "if(typeof c.saveFrameToPng!=='function')return'pieFX: saveFrameToPng unavailable in this AE';" +
                "var f=File.saveDialog('Save frame as PNG','PNG:*.png');" +
                "if(!f)return'cancelled';" +
                "if(!/\\.png$/i.test(f.fsName))f=new File(f.fsName+'.png');" +
                "var res=c.resolutionFactor;" +
                "try{c.resolutionFactor=[1,1];c.saveFrameToPng(c.time,f);}" +
                "finally{c.resolutionFactor=res;}" +
                "return'saved '+f.fsName;" +
                "})()",
            },
          },
          // The default, spelled out in the direction you already flicked: S
          // then S is the same command as S alone. Discoverable without
          // costing the one-flick case anything.
          {
            label: "Queue to Render",
            action: { kind: "ae-command", name: "Add to Render Queue", id: 2161 },
          },
          null,
          null,
        ],
      },
      // SW — was "More Actions" (a pager). Same content as a NAMED category, so
      // positions stay stable; paging would break the muscle memory the whole
      // design depends on.
      {
        label: "Layer",
        requires: "selection",
        slots: [
          { label: "Pre-comp", action: { kind: "ae-command", name: "Pre-compose...", id: 2071 } },
          // "Split + Dup" is back. The command map labels 2158
          // "DuplicatePreserveFile", which is why it was written off as having
          // no menu id — but findMenuCommandId("Split Layer") returns 2158.
          // The map's names are not to be trusted; AE's own lookup is.
          { label: "Split + Dup", action: { kind: "ae-command", name: "Split Layer", id: 2158 } },
          // A hole where Save Frame as PNG was: it moved to `Comp`, by the
          // same argument that kept Comp Settings out of here. It acts on the
          // comp, and it was the only thing in this ring that survived with
          // nothing selected — which made `Layer` look live when it was not.
          null,
          {
            // Resolved, and the capital I is the whole story: "Center in View"
            // returns 0, "Center In View" returns 3819. AE's menu strings are
            // not case-normalised and findMenuCommandId does not forgive.
            label: "Center in Comp",
            action: { kind: "ae-command", name: "Center In View", id: 3819 },
          },
          null,
          null,
        ],
      },
      // NW
      {
        label: "Anchor Master",
        requires: "selection",
        action: { kind: "builtin", name: "anchor-grid", cell: 4 },
        widget: "anchor",
      },
    ],
  },
};

// The renderer wants `kind`/`children`; the settings file speaks
// `action`/`slots`. Compile one into the other so the on-disk format stays the
// document and the runtime shape stays convenient.
// A child with no `requires` of its own inherits the category's — every item
// under `Layer` needs a selection for the same reason the category does. An
// explicit `requires: null` overrides that back to "needs nothing".
function compile(slot, inherited) {
  if (!slot) return null;
  const node = {
    label: slot.label,
    action: slot.action || null,
    // Per-hexagon highlight colour, or null to use the wheel's.
    accent: slot.accent || null,
    requires: "requires" in slot ? slot.requires : inherited || null,
    widget: slot.widget || (slot.action && slot.action.kind === "builtin" ? slot.action.name : null),
  };
  if (slot.slots) {
    node.kind = "ring";
    node.children = slot.slots.map((c) => compile(c, node.requires));
    node.def = slot.action ? slot.label : null;
  } else if (slot.widget) {
    node.kind = "widget";
    node.def = slot.action ? slot.label : null;
  } else {
    node.kind = "verb";
  }
  return node;
}

// Deep-clone, so an edit in the settings window can never reach into DEFAULTS
// and change what a "reset to defaults" restores.
export function cloneSettings(s) {
  return JSON.parse(JSON.stringify(s));
}

// Compile a whole settings object into the renderer's ring.
export function compileMenu(settings) {
  const slots = (settings && settings.wheel && settings.wheel.slots) || [];
  return { kind: "ring", label: "", children: slots.map((c) => compile(c, null)) };
}

// Why the last parse fell back, or "" if it did not. A settings file that is
// ignored SILENTLY is undiagnosable from the user's side — the wheel simply
// keeps doing what it did, and nothing anywhere says the file was read and
// rejected. Callers report this.
let lastError = "";

export function settingsError() {
  return lastError;
}

// Parse whatever load_settings returned. An unreadable file falls back to
// DEFAULTS rather than to an empty wheel: a broken settings file must not cost
// the user their menu.
//
// The BOM strip is not defensive programming for its own sake. PowerShell's
// Set-Content -Encoding utf8 writes one, JSON.parse rejects the string outright,
// and the failure was invisible — worse, the NATIVE side scans the same file
// with strstr and is unbothered by a BOM, so holdMs and armOnLaunch would be
// honoured from a file the wheel had thrown away. Two halves of the product
// disagreeing about whether a file exists is the bug; this is the fix.
export function parseSettings(text) {
  lastError = "";
  if (!text) return cloneSettings(DEFAULTS);
  const clean = text.replace(/^\uFEFF/, "").trim();
  if (!clean) return cloneSettings(DEFAULTS);
  try {
    const s = JSON.parse(clean);
    if (!s || !s.wheel || !Array.isArray(s.wheel.slots)) throw new Error("no wheel.slots");
    // Fill in whatever a file written by an older build is missing, rather
    // than treating an incomplete file as a broken one.
    s.gesture = Object.assign(cloneSettings(DEFAULTS.gesture), s.gesture || {});
    s.appearance = Object.assign(cloneSettings(DEFAULTS.appearance), s.appearance || {});
    return s;
  } catch (e) {
    lastError = String((e && e.message) || e);
    return cloneSettings(DEFAULTS);
  }
}

export { MN, DEFAULTS, compile };
