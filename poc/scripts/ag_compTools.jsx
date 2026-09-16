/**
 * AG Comp Tools — Un-precompose, and a Duplicate that copies what it nests.
 *
 * Everything lives on the single global _ct, for the same reason ag_masterNull
 * keeps to _mn: After Effects shares ONE ExtendScript namespace across every
 * running script, so a top-level `var comp` here is a name the next script has
 * to lose a fight with.
 *
 * pieFX loads this headlessly (see actions.js) and calls:
 *
 *   _ct.unPrecompose()                      asks, then extracts
 *   _ct.unPrecompose({deep:true, ask:false}) extracts, nested comps and all
 *   _ct.duplicateComp()                     a copy whose precomps are copies too
 *
 * WHY THIS IS A SCRIPT AND NOT A NATIVE BUILT-IN. Both features are pure
 * scripting-DOM work — copyToComp, setParentWithJump, replaceSource — and the
 * DOM is the only place any of it exists. Writing it against AEGP would be a
 * reimplementation of things AE already gets right.
 */
var _ct = (function(thisObj){

var scriptName = "AG Comp Tools";
var version    = "1.0";

// A nested tree deeper than this is a cycle AE should not have allowed, or a
// project pathological enough that a runaway recursion is the worse failure.
var MAX_DEPTH  = 24;

// Float compare for transform values. AE stores a "default" position as the
// comp centre, which for an odd-sized comp is a .5 — exact equality is fine
// for those, but a value that has been through a UI round-trip is not
// guaranteed to come back bit-identical.
var EPS = 0.0001;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
function isComp(it){
    return !!(it && it instanceof CompItem);
    }

// The comp the user means: the one in the viewer, else the one selected in the
// Project panel. Both are worth honouring — Duplicate is a project-panel verb
// as often as a viewer one.
function targetComp(){
    var c = app.project.activeItem;
    if(isComp(c)) return c;

    var sel = app.project.selection;
    for(var i = 0; i < sel.length; i++){
        if(isComp(sel[i])) return sel[i];
        }
    return null;
    }

function isPrecompLayer(layer){
    return !!(layer && layer.source && layer.source instanceof CompItem);
    }

function near(a, b){
    return Math.abs(a - b) < EPS;
    }

function prop(layer, matchName){
    // A layer type without the property (a camera has no opacity) returns null
    // rather than throwing its way out of the caller's loop.
    try { return layer.property("ADBE Transform Group").property(matchName); }
    catch(e){ return null; }
    }

function isStatic(p){
    return !!p && p.numKeys === 0 && !p.expressionEnabled;
    }

// ---------------------------------------------------------------------------
// copying a property, keyframes and all
//
// The carrier null (below) has to hold the precomp layer's transform EXACTLY,
// which means the keyframes and not just the values: a precomp layer that is
// animated is the case where getting this wrong is invisible on frame 0 and
// wrong everywhere else.
// ---------------------------------------------------------------------------
function copyKeys(src, dst){
    if(!src || !dst) return;

    if(src.expressionEnabled){
        try { dst.expression = src.expression; } catch(e){}
        }

    if(src.numKeys === 0){
        try { dst.setValue(src.value); } catch(e){}
        return;
        }

    var i;
    for(i = 1; i <= src.numKeys; i++){
        try { dst.setValueAtTime(src.keyTime(i), src.keyValue(i)); } catch(e){}
        }

    // Second pass: the interpolation of a key can only be set once the key
    // exists, and setting a temporal ease on key 3 while keys 4..n are still
    // missing is not the same curve.
    for(i = 1; i <= src.numKeys; i++){
        try {
            dst.setInterpolationTypeAtKey(i,
                                          src.keyInInterpolationType(i),
                                          src.keyOutInterpolationType(i));
            } catch(e){}
        try {
            dst.setTemporalEaseAtKey(i,
                                     src.keyInTemporalEase(i),
                                     src.keyOutTemporalEase(i));
            } catch(e){}
        try { dst.setTemporalContinuousAtKey(i, src.keyTemporalContinuous(i)); } catch(e){}
        try { dst.setTemporalAutoBezierAtKey(i, src.keyTemporalAutoBezier(i)); } catch(e){}

        if(src.isSpatial){
            try {
                dst.setSpatialTangentsAtKey(i,
                                            src.keyInSpatialTangent(i),
                                            src.keyOutSpatialTangent(i));
                } catch(e){}
            try { dst.setSpatialContinuousAtKey(i, src.keySpatialContinuous(i)); } catch(e){}
            try { dst.setSpatialAutoBezierAtKey(i, src.keySpatialAutoBezier(i)); } catch(e){}
            // Roving throws on the first and last key, which are never rovable.
            try { dst.setRovingAtKey(i, src.keyRoving(i)); } catch(e){}
            }
        }
    }

// Position may be separated into X/Y/Z, in which case the grouped property
// cannot be read or written at all and the three children are the real ones.
function copyPosition(src, dst){
    var sp = prop(src, "ADBE Position"),
        dp = prop(dst, "ADBE Position");

    if(!sp || !dp) return;

    if(sp.dimensionsSeparated){
        try { dp.dimensionsSeparated = true; } catch(e){}
        var names = ["ADBE Position_0" , "ADBE Position_1" , "ADBE Position_2"];
        for(var i = 0; i < names.length; i++){
            var s = prop(src, names[i]), d = prop(dst, names[i]);
            if(s && d) copyKeys(s, d);
            }
        return;
        }
    copyKeys(sp, dp);
    }

// ---------------------------------------------------------------------------
// the carrier null
//
// THE WHOLE ARGUMENT OF UN-PRECOMPOSE, in one paragraph. A layer inside the
// precomp has coordinates in the INNER comp's space. Its position in the outer
// comp is P + R*S*(p - A), where P, R, S and A are the precomp layer's
// position, rotation, scale and anchor point. That is also, exactly, what AE
// computes for a layer PARENTED to something whose transform is P, R, S, A. So
// a null carrying the precomp layer's transform, with the extracted layers
// parented to it and their values left alone, reproduces the picture — without
// baking anything, and with the animation still animated.
//
// Which is why the parenting below uses setParentWithJump: assigning .parent
// preserves where a layer currently IS by rewriting its values, and its values
// are the one thing here that must not be touched.
// ---------------------------------------------------------------------------

// The case where no null is needed: the mapping above is the identity, so the
// inner layers can simply move up. This is the ordinary "pre-comp with move all
// attributes, then change your mind" and it should not leave a null behind.
function transformIsIdentity(layer){
    var names = ["ADBE Anchor Point" , "ADBE Position" , "ADBE Scale" ,
                 "ADBE Rotate X" , "ADBE Rotate Y" , "ADBE Rotate Z" ,
                 "ADBE Orientation"];

    for(var i = 0; i < names.length; i++){
        var p = prop(layer, names[i]);
        if(p && !isStatic(p)) return false;
        }

    var a = prop(layer, "ADBE Anchor Point"),
        p = prop(layer, "ADBE Position"),
        s = prop(layer, "ADBE Scale"),
        r = prop(layer, "ADBE Rotate Z");

    if(!a || !p || !s) return false;
    if(p.dimensionsSeparated) return false;
    if(layer.threeDLayer) return false;
    if(r && !near(r.value, 0)) return false;
    if(!near(s.value[0], 100) || !near(s.value[1], 100)) return false;

    var av = a.value, pv = p.value;
    return near(av[0], pv[0]) && near(av[1], pv[1]) &&
           near(av.length > 2 ? av[2] : 0, pv.length > 2 ? pv[2] : 0);
    }

// WITH JUMP. The values being parented are already correct in the space they
// are about to be interpreted in; assigning .parent would "helpfully" rewrite
// them to keep the layer where it is sitting right now, which is the wrong
// place — it is the whole picture this feature is trying to preserve.
function setParent(layer, to, warn){
    if(!layer || !to) return;
    try {
        if(typeof layer.setParentWithJump === "function") layer.setParentWithJump(to);
        else layer.parent = to;
        } catch(e){
        if(warn) warn.push('Could not re-parent "' + layer.name + '": ' + e.toString());
        }
    }

function makeCarrier(comp, layer){
    var n = comp.layers.addNull(comp.duration);

    n.name        = layer.name + " XF";
    n.threeDLayer = layer.threeDLayer;
    n.label       = layer.label;
    n.moveBefore(layer);

    // A null's default anchor is [50,50] for its 100x100 box; the precomp
    // layer's anchor is in INNER comp coordinates and is the origin the whole
    // mapping is written around, so it is copied like everything else.
    copyKeys(prop(layer, "ADBE Anchor Point") , prop(n, "ADBE Anchor Point"));
    copyPosition(layer, n);
    copyKeys(prop(layer, "ADBE Scale")        , prop(n, "ADBE Scale"));
    copyKeys(prop(layer, "ADBE Rotate Z")     , prop(n, "ADBE Rotate Z"));

    if(layer.threeDLayer){
        copyKeys(prop(layer, "ADBE Rotate X")    , prop(n, "ADBE Rotate X"));
        copyKeys(prop(layer, "ADBE Rotate Y")    , prop(n, "ADBE Rotate Y"));
        copyKeys(prop(layer, "ADBE Orientation") , prop(n, "ADBE Orientation"));
        }

    // Not the opacity: parenting does not inherit it, and a null that silently
    // ate a 40% precomp layer would be a wrong picture reported as a success.
    // extractOne warns about it instead.

    n.inPoint  = layer.inPoint;
    n.outPoint = layer.outPoint;
    n.shy      = false;

    // A precomp that was ITSELF parented — to another null, or to the carrier
    // of an outer level during a deep pass — hands that parent to the carrier,
    // which is now the thing standing in for it. Without this, the second
    // level of a deep extract loses the first level's transform.
    setParent(n, layer.parent, null);

    return n;
    }

// ---------------------------------------------------------------------------
// what un-precompose cannot carry
//
// Said out loud rather than dropped. Every one of these is a property of the
// PRECOMP LAYER that has no counterpart on the layers inside it: there is no
// arrangement of extracted layers that reproduces a blur applied to the comp
// as a whole, and pretending otherwise is the failure mode this list exists to
// avoid.
// ---------------------------------------------------------------------------
function collectWarnings(layer, warn){
    var name = layer.name;

    var fx = null, masks = null;
    try { fx    = layer.property("ADBE Effect Parade"); } catch(e){}
    try { masks = layer.property("ADBE Mask Parade");   } catch(e){}

    if(fx && fx.numProperties > 0){
        warn.push('"' + name + '" had ' + fx.numProperties +
                  ' effect(s) on the precomp layer — not carried down.');
        }

    if(masks && masks.numProperties > 0){
        warn.push('"' + name + '" had ' + masks.numProperties +
                  ' mask(s) on the precomp layer — not carried down.');
        }

    var op = prop(layer, "ADBE Opacity");
    if(op && (!isStatic(op) || !near(op.value, 100))){
        warn.push('"' + name + '" was not at 100% opacity — parenting does not ' +
                  'inherit opacity, so that is lost.');
        }

    try {
        if(layer.blendingMode !== BlendingMode.NORMAL){
            warn.push('"' + name + '" had a blending mode — it applied to the ' +
                      'flattened comp and cannot be per-layer.');
            }
        } catch(e){}

    try {
        if(layer.timeRemapEnabled){
            warn.push('"' + name + '" had time remapping — the extracted layers ' +
                      'keep their own timing, not the remapped timing.');
            }
        } catch(e){}

    if(!near(layer.stretch, 100)){
        warn.push('"' + name + '" was time-stretched to ' + layer.stretch +
                  '% — the extracted layers are not.');
        }

    try {
        if(layer.collapseTransformation){
            warn.push('"' + name + '" had collapse transformations on — ' +
                      'extracting changes how it rasterises.');
            }
        } catch(e){}

    try {
        if(layer.trackMatteType && layer.trackMatteType !== TrackMatteType.NO_TRACK_MATTE){
            warn.push('"' + name + '" was using a track matte — that binding is ' +
                      'to the precomp layer and does not survive.');
            }
        } catch(e){}

    try {
        if(layer.motionBlur){
            warn.push('"' + name + '" had motion blur on the precomp layer.');
            }
        } catch(e){}
    }

// ---------------------------------------------------------------------------
// extract one precomp layer
//
// Returns the layers it created in the outer comp, so the deep pass can look
// at them and go round again.
// ---------------------------------------------------------------------------
function extractOne(comp, layer, opts, warn){
    var inner = layer.source,
        n     = inner.numLayers,
        made  = [],
        i;

    if(n === 0){
        warn.push('"' + layer.name + '" is an empty comp — nothing to extract.');
        layer.remove();
        if(opts.removeSource) maybeRemoveSource(inner, warn);
        return made;
        }

    collectWarnings(layer, warn);

    // The carrier, first, so the copies can be parented to it as they arrive
    // and so it sits above them in the stack.
    var identity = transformIsIdentity(layer),
        carrier  = identity ? null : makeCarrier(comp, layer);

    // LAST TO FIRST. copyToComp puts its copy at the TOP of the target, so
    // copying in stacking order would arrive reversed — which for a comp built
    // out of mattes and adjustment layers is not a cosmetic difference.
    var copies = [];        // indexed by the INNER layer index
    for(i = n; i >= 1; i--){
        inner.layer(i).copyToComp(comp);
        copies[i] = comp.layer(1);
        }

    // Put the block back where the precomp layer was, in order. Landing at the
    // top of the comp would put the extracted layers above everything that was
    // meant to be over them.
    for(i = 1; i <= n; i++){
        copies[i].moveBefore(layer);
        made.push(copies[i]);
        }

    // Parenting, in two parts: inner parenting is restored by index, and the
    // roots hang off the carrier. copyToComp does not bring a parent across —
    // the parent it pointed at is a layer in another comp.
    for(i = 1; i <= n; i++){
        var src = inner.layer(i),
            dst = copies[i],
            par = null;

        try { par = src.parent; } catch(e){ par = null; }

        // A root layer hangs off the carrier — or, when the transform was the
        // identity and there is no carrier, off whatever the precomp layer
        // was parented to.
        var to = par ? copies[par.index] : (carrier || layer.parent);
        setParent(dst, to, warn);
        }

    // Time. Inner time t sits at outer time startTime + t, so every copy moves
    // by the precomp layer's startTime, and then gets clipped to the precomp
    // layer's own bar — a layer that was scrolled off the end of the precomp
    // was not visible before and must not become visible now.
    for(i = 1; i <= n; i++){
        var c = copies[i];
        try { c.startTime = c.startTime + layer.startTime; } catch(e){}

        try {
            var lo = Math.max(c.inPoint , layer.inPoint),
                hi = Math.min(c.outPoint, layer.outPoint);
            if(hi > lo){
                // Out first: AE rejects an in-point past the current out-point.
                c.outPoint = hi;
                c.inPoint  = lo;
                }else{
                c.enabled = false;
                warn.push('"' + c.name + '" fell entirely outside "' + layer.name +
                          '" in time — extracted, but switched off.');
                }
            } catch(e){}
        }

    if(carrier){
        carrier.inPoint  = layer.inPoint;
        carrier.outPoint = layer.outPoint;
        }

    layer.remove();
    if(opts.removeSource) maybeRemoveSource(inner, warn);

    return made;
    }

// Only when nothing else points at it. Called AFTER the precomp layer is gone,
// so the comp we just extracted out of no longer counts as a use — and
// removing an item that is still in a comp would take that layer with it,
// which is not a thing to do by accident.
function maybeRemoveSource(inner, warn){
    try {
        var uses = inner.usedIn.length;
        if(uses === 0) inner.remove();
        else warn.push('"' + inner.name + '" is still used by ' + uses +
                       ' comp(s), so it stays in the project.');
        } catch(e){
        warn.push('Could not remove "' + inner.name + '" from the project: ' + e.toString());
        }
    }

// ---------------------------------------------------------------------------
// un-precompose
// ---------------------------------------------------------------------------
function countNested(layers){
    var n = 0;
    for(var i = 0; i < layers.length; i++){
        var inner = layers[i].source;
        for(var j = 1; j <= inner.numLayers; j++){
            if(isPrecompLayer(inner.layer(j))) n++;
            }
        }
    return n;
    }

function selectedPrecomps(comp, warn){
    var sel = comp.selectedLayers,
        out = [],
        skipped = 0;

    for(var i = 0; i < sel.length; i++){
        if(isPrecompLayer(sel[i])) out.push(sel[i]);
        else skipped++;
        }
    if(skipped){
        warn.push(skipped + ' selected layer(s) were not precomps, and were left alone.');
        }
    return out;
    }

/**
 * opts:
 *   deep         extract precomps found inside the extracted layers too
 *   ask          show the dialog (default true when `deep` was not given)
 *   removeSource delete an emptied comp from the project when nothing else uses it
 */
function unPrecompose(opts){
    opts = opts || {};

    var comp = targetComp();
    if(!comp) return "pieFX: no comp is active";

    var warn    = [],
        targets = selectedPrecomps(comp, warn);

    if(!targets.length) return "pieFX: select a precomp layer first";

    var ask = ("ask" in opts) ? opts.ask : !("deep" in opts);
    if(ask){
        var answer = askDialog(countNested(targets), opts);
        if(!answer) return "cancelled";
        opts = answer;
        }

    var deep = !!opts.deep, made = 0;

    app.beginUndoGroup("pieFX: Un-precompose");
    try {
        var queue = [], i;
        for(i = 0; i < targets.length; i++) queue.push({ layer: targets[i], depth: 0 });

        while(queue.length){
            var job = queue.shift();
            var out = extractOne(comp, job.layer, opts, warn);
            made += out.length;

            if(deep){
                for(i = 0; i < out.length; i++){
                    if(!isPrecompLayer(out[i])) continue;
                    if(job.depth < MAX_DEPTH){
                        queue.push({ layer: out[i], depth: job.depth + 1 });
                        }else{
                        warn.push('Stopped at ' + MAX_DEPTH + ' levels deep, at "' +
                                  out[i].name + '".');
                        }
                    }
                }
            }
        } catch(e){
        app.endUndoGroup();
        alert(scriptName + "\n\nUn-precompose failed:\n" + e.toString(), scriptName);
        return "pieFX: un-precompose failed: " + e.toString();
        }
    app.endUndoGroup();

    report("Un-precompose", warn);
    return "un-precomposed " + targets.length + " layer(s) into " + made + " layer(s)";
    }

// ---------------------------------------------------------------------------
// duplicate, including the precomps
//
// comp.duplicate() gives a copy that SHARES its nested comps: edit a precomp in
// the copy and you have edited the original. That is exactly the surprise this
// exists to remove — so every nested comp is duplicated too, and the copy is
// re-pointed at the duplicate.
//
// A comp used twice inside the tree is duplicated ONCE and both layers point at
// that one duplicate, because the sharing WITHIN the original is part of what
// is being copied.
// ---------------------------------------------------------------------------
function deepDuplicate(src, map, warn, depth){
    var key = "c" + src.id;
    if(map[key]) return map[key];

    if(depth > MAX_DEPTH){
        warn.push("Stopped at " + MAX_DEPTH + ' levels deep at "' + src.name + '".');
        return src;
        }

    var dup = src.duplicate();
    map[key] = dup;

    // duplicate() drops the copy next to the original in the Project panel on
    // some versions and at the root on others; pin it to the original's folder
    // so a deep tree does not scatter itself across the project.
    try { dup.parentFolder = src.parentFolder; } catch(e){}

    for(var i = 1; i <= dup.numLayers; i++){
        var L = dup.layer(i);
        if(!isPrecompLayer(L)) continue;

        var nested = deepDuplicate(L.source, map, warn, depth + 1);
        if(nested === L.source) continue;

        try { L.replaceSource(nested, true); }
        catch(e){ warn.push('Could not re-point "' + L.name + '": ' + e.toString()); }
        }
    return dup;
    }

// An expression that names a comp by string is bound to the ORIGINAL's name,
// and the duplicate's nested comps have new names. Nothing can fix that
// automatically — but it can be said.
function countCompExpressions(comp, seen){
    seen = seen || {};
    var key = "c" + comp.id;
    if(seen[key]) return 0;
    seen[key] = true;

    var n = 0;
    for(var i = 1; i <= comp.numLayers; i++){
        var L = comp.layer(i);
        n += walkForCompExpr(L, 0);
        if(isPrecompLayer(L)) n += countCompExpressions(L.source, seen);
        }
    return n;
    }

function walkForCompExpr(group, depth){
    if(depth > 6) return 0;      // deep enough for any real property tree
    var n = 0, count = 0;
    try { count = group.numProperties; } catch(e){ return 0; }

    for(var i = 1; i <= count; i++){
        var p;
        try { p = group.property(i); } catch(e){ continue; }
        if(!p) continue;

        if(p.propertyType === PropertyType.PROPERTY){
            try {
                if(p.expressionEnabled && p.expression.indexOf("comp(") >= 0) n++;
                } catch(e){}
            }else{
            n += walkForCompExpr(p, depth + 1);
            }
        }
    return n;
    }

/**
 * opts:
 *   deep  duplicate the nested comps too (default true — the point of this)
 */
function duplicateComp(opts){
    opts = opts || {};
    var deep = ("deep" in opts) ? !!opts.deep : true;

    var comp = targetComp();
    if(!comp) return "pieFX: no comp is active or selected";

    var warn = [], map = {}, dup = null;
    var before = app.project.numItems;

    app.beginUndoGroup("pieFX: Duplicate comp");
    try {
        if(deep){
            var expr = countCompExpressions(comp, {});
            if(expr){
                warn.push(expr + ' expression(s) refer to a comp by name — those still ' +
                          'point at the ORIGINAL comps, not at the copies.');
                }
            dup = deepDuplicate(comp, map, warn, 0);
            }else{
            dup = comp.duplicate();
            try { dup.parentFolder = comp.parentFolder; } catch(e){}
            }
        } catch(e){
        app.endUndoGroup();
        alert(scriptName + "\n\nDuplicate failed:\n" + e.toString(), scriptName);
        return "pieFX: duplicate failed: " + e.toString();
        }
    app.endUndoGroup();

    try {
        dup.selected = true;
        dup.openInViewer();
        } catch(e){}

    report("Duplicate", warn);
    return "duplicated \"" + comp.name + "\" as \"" + dup.name + "\" (" +
           (app.project.numItems - before) + " new item(s))";
    }

// ---------------------------------------------------------------------------
// reporting
//
// Silence is only right when there is nothing to say. A warning list is shown,
// once, after the work is done and committed — never as a question in the
// middle of an undo group.
// ---------------------------------------------------------------------------
function report(what, warn){
    if(!warn.length) return;
    alert(scriptName + " — " + what + "\n\n• " + warn.join("\n• "), scriptName);
    }

// ---------------------------------------------------------------------------
// the question
//
// Un-precompose has exactly one decision in it, and it is not one that can be
// guessed: extracting a precomp that itself contains precomps either stops at
// the first level or goes all the way down, and both are things people mean.
// ---------------------------------------------------------------------------
function askDialog(nested, opts){
    var w = new Window("dialog" , "Un-precompose");

    w.orientation  = "column";
    w.alignChildren = "left";
    w.spacing      = 10;
    w.margins      = 16;

    w.add("statictext", undefined,
          nested ? ("This selection contains " + nested + " nested precomp" +
                    (nested === 1 ? "" : "s") + ".")
                 : "No nested precomps in this selection.");

    var deep = w.add("checkbox", undefined, "Deep — extract nested precomps too");
    deep.value   = !!opts.deep;
    deep.enabled = nested > 0;

    var rm = w.add("checkbox", undefined, "Remove emptied comps from the project");
    rm.value = !!opts.removeSource;

    var row = w.add("group");
    row.alignment = "right";
    var cancel = row.add("button", undefined, "Cancel", { name: "cancel" });
    var ok     = row.add("button", undefined, "Un-precompose", { name: "ok" });

    var out = null;
    ok.onClick     = function(){ out = { deep: deep.value && nested > 0,
                                         removeSource: rm.value,
                                         ask: false }; w.close(); };
    cancel.onClick = function(){ out = null; w.close(); };

    w.show();
    return out;
    }

// ---------------------------------------------------------------------------
// panel, for running this the ordinary way
// ---------------------------------------------------------------------------
function showUI(thisObj){
    var pal = (thisObj instanceof Panel)
            ? thisObj
            : new Window("palette" , scriptName + " " + version , undefined , { resizeable: true });

    pal.orientation   = "column";
    pal.alignChildren = "fill";
    pal.spacing       = 6;
    pal.margins       = 10;

    pal.add("button", undefined, "Un-precompose…").onClick = function(){ unPrecompose(); };
    pal.add("button", undefined, "Duplicate (deep)").onClick = function(){ duplicateComp(); };

    if(pal instanceof Window){
        pal.center();
        pal.show();
        }else{
        pal.layout.layout(true);
        }
    return pal;
    }

// -- boot -------------------------------------------------------------------
// pieFX loads this file headlessly to reach the functions below and sets
// __pieFXHeadless while it does; without the guard the palette would open in
// the middle of a gesture, once per session. Running it from the Scripts menu
// is unaffected.
if(!$.global.__pieFXHeadless){ showUI(thisObj); }

return {
    scriptName    : scriptName,
    version       : version,
    unPrecompose  : unPrecompose,
    duplicateComp : duplicateComp
    };

})(this);
