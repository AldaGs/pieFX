/**
 * AG Master Null
 *
 * Everything lives on the single global _mn. After Effects shares one
 * ExtendScript namespace across every running script, so anything declared
 * at the top level here (aeobj, scriptName, the helper functions) would be
 * up for grabs by the next script that uses the same name.
 */
var _mn = (function(thisObj){

/**
 * aeobj - small After Effects helper library.
 * matchName constants, project / comp / layer getters, geometry helpers.
 */
var aeobj = {

    // ---------------------------------------------------------------------
    // matchName constants
    // ---------------------------------------------------------------------
    match : {
        AVLayer     : "ADBE AV Layer",
        markers     : "ADBE Marker",
        effects     : "ADBE Effect Parade",
        transform   : "ADBE Transform Group",
        anchor      : "ADBE Anchor Point",
        position    : "ADBE Position",
        positionX   : "ADBE Position_0",
        positionY   : "ADBE Position_1",
        positionZ   : "ADBE Position_2",
        scale       : "ADBE Scale",
        rotation    : "ADBE Rotate Z",
        rotationX   : "ADBE Rotate X",
        rotationY   : "ADBE Rotate Y",
        rotationZ   : "ADBE Rotate Z",
        orientation : "ADBE Orientation",
        opacity     : "ADBE Opacity"
        },

    // ---------------------------------------------------------------------
    // App / project / comp
    // ---------------------------------------------------------------------
    getApp : function(){
        return app;
        },

    doUndo : function(action){
        return this.getApp().beginUndoGroup(action);
        },

    endUndo : function(){
        return this.getApp().endUndoGroup();
        },

    getProject : function(){
        return this.getApp().project;
        },

    // Active comp, or false when the active item is not a comp.
    getComp : function(){
        var comp = this.getProject().activeItem;
        return (comp && comp instanceof CompItem) ? comp : false;
        },

    // Selected layers of the active comp, or false.
    // Guards against getComp() returning false - the old version went
    // straight to .selectedLayers and threw with no comp open.
    getSelectedLayers : function(){
        var comp = this.getComp();
        if(!comp) return false;

        var layers = comp.selectedLayers;
        return (layers.length > 0) ? layers : false;
        },

    getSelectedProperties : function(layer){
        if(!layer) return false;

        var props = layer.selectedProperties;
        return (props.length > 0) ? props : false;
        },

    // ---------------------------------------------------------------------
    // Property predicates
    // Compared against exact matchNames. The old substring tests matched
    // unrelated effect properties, and the rotation test looked for the
    // misspelling "Ratate" so it never fired on the separated dimensions.
    // ---------------------------------------------------------------------
    isAnchorProp : function(prop){
        return (prop.matchName == this.match.anchor);
        },

    isPositionProp : function(prop){
        return (prop.matchName == this.match.position ||
                prop.matchName == this.match.positionX ||
                prop.matchName == this.match.positionY ||
                prop.matchName == this.match.positionZ);
        },

    isRotationProp : function(prop){
        return (prop.matchName == this.match.rotationX ||
                prop.matchName == this.match.rotationY ||
                prop.matchName == this.match.rotationZ);
        },

    isScaleProp : function(prop){
        return (prop.matchName == this.match.scale);
        },

    isOrientationProp : function(prop){
        return (prop.matchName == this.match.orientation);
        },

    isOpacityProp : function(prop){
        return (prop.matchName == this.match.opacity);
        },

    // ---------------------------------------------------------------------
    // Creation helpers
    // All of these validate their arguments; the old versions read
    // params.length off whatever was passed and threw on non arrays.
    // ---------------------------------------------------------------------
    addFolder : function(name){
        return this.getProject().items.addFolder(name);
        },

    // params: [name, width, height, pixelAspect, duration, frameRate]
    addComp : function(params){
        if(!(params instanceof Array) || params.length != 6) return null;
        return this.getProject().items.addComp(params[0] , params[1] , params[2] ,
                                               params[3] , params[4] , params[5]);
        },

    addLayer : function(comp , type , params){
        if(!comp) return null;

        switch(type){
            case "text" :
                return comp.layers.addText(params);

            // params: [name, width, height, pixelAspect, duration, color]
            case "solid" :
                if(!(params instanceof Array) || params.length != 6) return null;
                return comp.layers.addSolid(params[0] , params[1] , params[2] ,
                                            params[3] , params[4] , params[5]);

            // params: optional duration in seconds
            case "null" :
                return comp.layers.addNull(params);

            case "shape" :
                return comp.layers.addShape();

            // params: [item, duration]
            case "item" :
            default :
                if(!(params instanceof Array) || params.length != 2) return null;
                return comp.layers.add(params[0] , params[1]);
            };
        },

    // ---------------------------------------------------------------------
    // Geometry
    // ---------------------------------------------------------------------

    // Layer bounds in SOURCE coordinates.
    // Duck typed rather than "instanceof AVLayer": ExtendScript compares
    // host objects against the exact constructor, so shape and text layers
    // fail that test even though they do have a source rectangle.
    getLayerRect : function(layer){
        if(!layer || typeof layer.sourceRectAtTime != "function") return null;

        var rect = layer.sourceRectAtTime(layer.time , false);
        return {
            left   : rect.left,
            top    : rect.top,
            width  : rect.width,
            height : rect.height,
            right  : rect.left + rect.width,
            bottom : rect.top + rect.height
            };
        },

    // Layer bounds in COMP coordinates.
    //
    // Rewritten: every corner of the source rect is converted individually
    // and the axis aligned box is taken from the results. That makes the
    // box correct under rotation and under any parent chain. The old
    // version scaled a centre point by hand, ignored rotation entirely,
    // and had scaleFactorX and scaleFactorY assigned to the wrong axes.
    getLayerRectInComp : function(layer){
        if(!layer) return null;

        var transform  = layer.property(this.match.transform);
        var anchorProp = transform.property(this.match.anchor);
        var anchor     = anchorProp ? anchorProp.value : [0 , 0 , 0];
        var is3D       = layer.threeDLayer;

        // Converts a source space point to comp space, 2D or 3D aware.
        var toCompPoint = function(p){
            if(is3D) return layer.toComp([p[0] , p[1] , 0]);
            return layer.sourcePointToComp([p[0] , p[1]]);
            };

        var rect = this.getLayerRect(layer);
        if(!rect){
            // Cameras, lights and other non AV layers have no bounds.
            var origin = toCompPoint([anchor[0] , anchor[1]]);
            return {
                anchorX     : origin[0],
                anchorY     : origin[1],
                middlePoint : origin,
                middPoint   : origin,       // kept for backwards compatibility
                middleX     : origin[0],
                middleY     : origin[1],
                left        : origin[0],
                right       : origin[0],
                top         : origin[1],
                bottom      : origin[1]
                };
            };

        var corners = [
            toCompPoint([rect.left  , rect.top]),
            toCompPoint([rect.right , rect.top]),
            toCompPoint([rect.right , rect.bottom]),
            toCompPoint([rect.left  , rect.bottom])
            ];

        var left = corners[0][0], right = corners[0][0];
        var top  = corners[0][1], bottom = corners[0][1];
        for(var i = 1 ; i < corners.length ; i++){
            if(corners[i][0] < left)   left   = corners[i][0];
            if(corners[i][0] > right)  right  = corners[i][0];
            if(corners[i][1] < top)    top    = corners[i][1];
            if(corners[i][1] > bottom) bottom = corners[i][1];
            };

        var anchorInComp = toCompPoint([anchor[0] , anchor[1]]);
        var middlePoint  = toCompPoint([rect.left + rect.width  * 0.5 ,
                                        rect.top  + rect.height * 0.5]);

        return {
            anchorX     : anchorInComp[0],
            anchorY     : anchorInComp[1],
            middlePoint : middlePoint,
            middPoint   : middlePoint,      // kept for backwards compatibility
            middleX     : middlePoint[0],
            middleY     : middlePoint[1],
            left        : left,
            right       : right,
            top         : top,
            bottom      : bottom
            };
        },

    // ---------------------------------------------------------------------
    // Modifier keys
    // ---------------------------------------------------------------------
    getExtraKey : function(key){
        var state = ScriptUI.environment.keyboardState;
        switch(key){
            case "shift" : return state.shiftKey;
            case "ctrl"  : return state.ctrlKey;
            case "alt"   : return state.altKey;
            case "meta"  : return state.metaKey;
            default      : return false;
            };
        }
    };

    var scriptName = "AG Master Null";
    var version = "1.0.0";

    // The modifier keys are the only way to reach three of the four
    // behaviours, so spell them out on hover instead of leaving them hidden.
    var helpTip = [
        "Creates a null at the average position of the selected layers,",
        "sized to the selection's time range, and parents them to it.",
        "",
        "Right click  -  place the null at the COMP CENTRE instead",
        "          (Shift has no effect in this mode)",
        "",
        "Shift  -  place the null at the centre of the total",
        "          selection AREA instead of at the average of",
        "          the anchor points",
        "Alt    -  create the null but do not parent anything",
        "Ctrl (Cmd on Mac)  -  insert the null INTO the existing",
        "          hierarchy: layers that already have a parent are",
        "          attached to the null, and the null is attached to",
        "          that parent, so nothing above is disconnected"
        ].join("\n");

    // Snapshot of the modifier keys, taken ONCE at click time.
    //
    // ScriptUI.environment.keyboardState is a live read of the keyboard, not
    // a record of the click that just happened. The old code polled it four
    // separate times, spread across the whole run - shift at the top of
    // buildMasterNull, alt just before parenting, ctrl and meta inside
    // parentLayers itself. Every one of those reads is a fresh question to
    // the OS about what is being held RIGHT NOW, and the later ones fire
    // after AE has created, moved and positioned a layer. Reading the state
    // once, in the handler, is the only way to be sure all four flags
    // describe the same moment.
    function getModifiers(){
        var state = ScriptUI.environment.keyboardState;
        return {
            useArea       : state.shiftKey,
            skipParenting : state.altKey,
            // Cmd stands in for Ctrl on Mac, where Ctrl+click is a
            // secondary click and never reaches the primary handler.
            forceReparent : (state.ctrlKey || state.metaKey)
            };
        };

    function buildUI(thisObj){
        var mainWindow = (thisObj instanceof Panel) ? thisObj : new Window("palette", [scriptName , version].join(" - ") ,undefined,{resizeable:true});
        mainWindow.margins = 2;
        mainWindow.spacing = 2;
        mainWindow.alignChildren = ["fill" , "fill"];

        var setButton = mainWindow.add("button" , [0 , 0 , 200 , 200] , "Master Null");
        setButton.helpTip = helpTip;

        // addMasterNull owns its own undo group, so that a run that bails out
        // on validation never opens an empty one.
        setButton.onClick = function(){
            addMasterNull(false , getModifiers());
            };

        // Secondary click = place the null at the comp centre.
        // onClick only fires for the primary button, so the secondary button
        // has to be picked up here. mousedown rather than click: right click
        // does not reliably produce a synthetic click event across every
        // AE / OS combination, but it always produces a mousedown.
        setButton.addEventListener("mousedown" , function(e){
            if(e.button != 2) return;   // 0 = primary, 2 = secondary
            addMasterNull(true , getModifiers());
            });

        mainWindow.onResizing = function (){
                this.layout.resize();
            };

        mainWindow.layout.layout(true);
        return mainWindow;
        };

    function showUI(thisObj){
        var palette = buildUI(thisObj);
        if(palette != null && palette instanceof Window){
            palette.center();
            palette.show();
            };
        return palette;
        };


    // =====================================================================
    // Vector helpers
    // ExtendScript arrays do NOT support + or +=. "[0,0] += [10,20]" quietly
    // becomes the STRING "0,010,20", which is how the old averaging code
    // ended up feeding NaN into setValue(). All vector maths below is done
    // component by component on purpose.
    // =====================================================================

    // Returns a 3 component copy of a 2 or 3 component array.
    function toVec3(v){
        return [v[0] , v[1] , (v.length > 2) ? v[2] : 0];
        };

    // Component wise a + b, both padded to 3 components.
    function addVec(a , b){
        var a3 = toVec3(a);
        var b3 = toVec3(b);
        return [a3[0] + b3[0] , a3[1] + b3[1] , a3[2] + b3[2]];
        };

    // Component wise v / d.
    function divVec(v , d){
        if(d == 0) return [0 , 0 , 0];
        var v3 = toVec3(v);
        return [v3[0] / d , v3[1] / d , v3[2] / d];
        };

    // =====================================================================
    // Comp space sampling
    // toComp() / sourcePointToComp() walk the whole parent chain and account
    // for anchor point, scale, rotation and orientation. The old code tried
    // to hand roll this by adding up parent POSITION values, which ignored
    // every other transform property and returned the wrong point as soon as
    // any parent was scaled or rotated.
    // =====================================================================

    // Comp space anchor point of a layer, always returned as [x,y,z].
    function getLayerCompAnchor(layer){
        var transform = layer.property(aeobj.match.transform);
        var anchorProp = transform.property(aeobj.match.anchor);
        // Cameras and lights have no anchor point. Their layer space origin
        // IS their position, so [0,0,0] is the correct point to convert.
        var point = anchorProp ? toVec3(anchorProp.value) : [0 , 0 , 0];

        // 3D layers: toComp gives a real comp space point, including Z.
        if(layer.threeDLayer) return toVec3(layer.toComp(point));

        // 2D layers: project in 2D and keep Z flat at 0.
        var comp2D = layer.sourcePointToComp([point[0] , point[1]]);
        return [comp2D[0] , comp2D[1] , 0];
        };

    // Does this layer actually have a source rectangle?
    //
    // "layer instanceof AVLayer" is NOT reliable in ExtendScript: for host
    // objects it compares against the exact constructor, so ShapeLayer and
    // TextLayer report FALSE even though they are AV layers. That is what
    // broke area mode - shape and text layers fell straight through to the
    // "no bounds" fallback and contributed their anchor point instead of
    // their box. Duck typing on the method is the dependable test.
    function hasSourceRect(layer){
        return (typeof layer.sourceRectAtTime == "function");
        };

    // Converts a point in a layer's SOURCE space to comp space, [x,y,z].
    function layerPointToComp(layer , point){
        if(layer.threeDLayer) return toVec3(layer.toComp([point[0] , point[1] , 0]));

        var comp2D = layer.sourcePointToComp([point[0] , point[1]]);
        return [comp2D[0] , comp2D[1] , 0];
        };

    // The four corners of a layer's bounding box in comp space.
    // Converting each corner individually, rather than scaling a centre
    // point by hand, keeps the box correct under rotation and under any
    // parent chain. Returns null for layers with no bounds.
    function getLayerCompCorners(layer){
        if(!hasSourceRect(layer)) return null;

        var rect   = layer.sourceRectAtTime(layer.time , false);
        var right  = rect.left + rect.width;
        var bottom = rect.top + rect.height;

        return [
            layerPointToComp(layer , [rect.left , rect.top]),
            layerPointToComp(layer , [right     , rect.top]),
            layerPointToComp(layer , [right     , bottom]),
            layerPointToComp(layer , [rect.left , bottom])
            ];
        };

    // Union bounding box of the WHOLE selection in comp space - the actual
    // selection area, i.e. what a rectangle drawn around everything covers.
    //
    // This is deliberately NOT an average of the individual layer centres.
    // With layers of different sizes, or several small layers on one side
    // and one big layer on the other, the average of the centres is dragged
    // toward whichever side holds more layers. The centre of the union box
    // sits in the middle of the area, which is what area mode is for.
    function getSelectionBounds(layers){
        var left = null , right = null , top = null , bottom = null;
        var zSum = 0 , zCount = 0;

        for(var i = 0 ; i < layers.length ; i++){
            // Layers with no bounds (cameras, lights) still contribute
            // their own position as a single point.
            var points = getLayerCompCorners(layers[i]);
            if(!points) points = [getLayerCompAnchor(layers[i])];

            for(var p = 0 ; p < points.length ; p++){
                var pt = points[p];
                if(left   == null || pt[0] < left)   left   = pt[0];
                if(right  == null || pt[0] > right)  right  = pt[0];
                if(top    == null || pt[1] < top)    top    = pt[1];
                if(bottom == null || pt[1] > bottom) bottom = pt[1];
                zSum += pt[2];
                zCount++;
                };
            };

        if(left == null) return null;

        return {
            left    : left,
            right   : right,
            top     : top,
            bottom  : bottom,
            // Z has no meaningful box, so the average depth is used.
            centerZ : (zCount > 0) ? zSum / zCount : 0
            };
        };

    // Comp space point the null should be created at, always [x,y,z].
    // useArea = true  -> centre of the selection's total AREA (Shift)
    // useArea = false -> average of the anchor points (default)
    function getAveragePosition(layers , useArea){
        if(layers.length == 0) return null;

        if(useArea){
            var bounds = getSelectionBounds(layers);
            if(!bounds) return null;
            return [(bounds.left + bounds.right) * 0.5 ,
                    (bounds.top + bounds.bottom) * 0.5 ,
                    bounds.centerZ];
            };

        var sum = [0 , 0 , 0];
        for(var i = 0 ; i < layers.length ; i++){
            sum = addVec(sum , getLayerCompAnchor(layers[i]));
            };
        return divVec(sum , layers.length);
        };

    // Centre of the composition, always [x,y,z]. Z stays 0 so a 3D null
    // lands on the comp plane rather than in front of or behind it.
    function getCompCenter(comp){
        return [comp.width * 0.5 , comp.height * 0.5 , 0];
        };

    // The null goes 3D as soon as any selected layer is 3D, so it can carry
    // depth for the layers that need it.
    function isThreeDimensional(layers){
        for(var i = 0 ; i < layers.length ; i++){
            if(layers[i].threeDLayer) return true;
            };
        return false;
        };

    // =====================================================================
    // Time range
    // =====================================================================

    // Earliest in point of the selection. Deliberately NOT a sort: the old
    // version sorted the caller's array in place and silently destroyed the
    // index ordering that moveBefore() relies on.
    function getSelectionInPoint(layers){
        if(layers.length == 0) return null;

        var min = layers[0].inPoint;
        for(var i = 1 ; i < layers.length ; i++){
            if(layers[i].inPoint < min) min = layers[i].inPoint;
            };
        return min;
        };

    // Latest out point of the selection.
    function getSelectionOutPoint(layers){
        if(layers.length == 0) return null;

        var max = layers[0].outPoint;
        for(var i = 1 ; i < layers.length ; i++){
            if(layers[i].outPoint > max) max = layers[i].outPoint;
            };
        return max;
        };

    // In point, out point and duration of the selection as a whole.
    function getSelectionTimeRange(comp , layers){
        var inPoint  = getSelectionInPoint(layers);
        var outPoint = getSelectionOutPoint(layers);
        if(inPoint == null || outPoint == null) return null;

        // Never hand AE a zero or negative duration; a single frame is the
        // smallest layer bar that can exist.
        var duration = Math.max(outPoint - inPoint , comp.frameDuration);

        return { inPoint : inPoint , outPoint : outPoint , duration : duration };
        };

    // Align the null's layer bar to the selection.
    // The null is created with the selection's duration as its SOURCE
    // duration, so all that is left here is sliding the bar into place.
    // startTime moves in and out together, so it goes first; the out point
    // is then trimmed back. Doing it the other way round can momentarily
    // leave outPoint before inPoint, which AE rejects with an error.
    function setNullTimeRange(masterNull , range){
        if(!range) return;

        // addNull(duration) already gave the layer the right length, with the
        // bar sitting at [0 , duration]. Sliding startTime moves the in and
        // out points together, so this lands it on [inPoint , outPoint] and
        // there is nothing left to trim.
        //
        // Do NOT clamp against masterNull.source.duration here. A null's
        // source is a still, and AVItem.duration is 0 for stills - not the
        // layer's duration. Clamping against it collapsed every null to a
        // zero length bar.
        masterNull.startTime = range.inPoint;
        };

    // =====================================================================
    // Parenting
    // =====================================================================

    function isLayerInSelection(layer , layers){
        for(var i = 0 ; i < layers.length ; i++){
            if(layers[i].index == layer.index) return true;
            };
        return false;
        };

    // The ROOTS of the selection: the selected layers that are not already
    // following another selected layer.
    //
    // A layer whose parent is also selected reaches the null through that
    // parent anyway, so attaching it directly would only flatten the rig.
    function getSelectionRoots(layers , masterNull){
        var roots = [];

        for(var i = 0 ; i < layers.length ; i++){
            var layer = layers[i];
            if(layer.index == masterNull.index) continue;
            if(layer.parent && isLayerInSelection(layer.parent , layers)) continue;
            roots.push(layer);
            };
        return roots;
        };

    // The layer that every root already hangs from, or null when there
    // isn't exactly one.
    //
    // Requiring ALL roots to share it is deliberate. If only some of them
    // have a parent, hanging the null off that parent would drag the
    // unparented ones into a hierarchy they were never part of, which is a
    // structural change rather than a preserved structure.
    function getSharedParent(roots){
        if(roots.length == 0) return null;

        var shared = roots[0].parent;
        if(!shared) return null;

        for(var i = 1 ; i < roots.length ; i++){
            if(!roots[i].parent) return null;
            if(roots[i].parent.index != shared.index) return null;
            };
        return shared;
        };

    // Attach the selection to the null.
    //
    // Default: only roots that have no parent at all are touched. Anything
    // already hanging off a layer outside the selection is left alone.
    //
    // forceReparent (ctrl / cmd): INSERT the null into the hierarchy rather
    // than break it. The null takes the roots' place under their existing
    // parent, and the roots then hang off the null - so the chain above the
    // selection survives intact.
    //
    //   L1
    //   +- L2      selecting L2..L5 and holding ctrl gives:
    //   +- L3
    //      +- L4     L1
    //      +- L5     +- MASTER - NULL
    //                   +- L2
    //                   +- L3
    //                      +- L4
    //                      +- L5
    //
    // The old behaviour simply reassigned L2 and L3 to the null, which cut
    // the whole group loose from L1.
    function parentLayers(layers , masterNull , forceReparent){
        var roots = getSelectionRoots(layers , masterNull);

        if(forceReparent){
            // Slot the null in above the roots, under whatever they were
            // already following. AE preserves world transforms on every
            // assignment, so the order of these two steps does not matter.
            var shared = getSharedParent(roots);
            if(shared) masterNull.parent = shared;

            for(var i = 0 ; i < roots.length ; i++){
                roots[i].parent = masterNull;
                };
            return;
            };

        for(var r = 0 ; r < roots.length ; r++){
            if(!roots[r].parent) roots[r].parent = masterNull;
            };
        };

    // =====================================================================
    // Main
    // =====================================================================

    // useCompCenter - place the null at the comp centre instead of at the
    //                 average of the selection.
    // mods          - modifier snapshot from getModifiers(). Optional, so
    //                 that _mn.addMasterNull() still works when called from
    //                 another script with no click behind it.
    function addMasterNull(useCompCenter , mods){
        if(!mods) mods = getModifiers();

        var comp = aeobj.getComp();
        if(!comp){
            alert("Open or select a composition first." , scriptName);
            return;
            };

        var selection = comp.selectedLayers;
        if(!selection || selection.length == 0){
            alert("Select at least one layer." , scriptName);
            return;
            };

        // Validation is done, so nothing past this point can open an empty
        // undo group. try / finally guarantees the group is closed even if
        // something below throws - an undo group left open wedges AE's undo
        // stack until some other script happens to close one.
        aeobj.doUndo(useCompCenter ? "Add Master Null (Comp Centre)" : "Add Master Null");
        try{
            buildMasterNull(comp , selection , useCompCenter , mods);
            }
        finally{
            aeobj.endUndo();
            };
        };

    // The actual work. Called inside the undo group above.
    function buildMasterNull(comp , selection , useCompCenter , mods){
        // Work on our own copy so nothing downstream mutates the selection.
        var layers = selection.slice(0);
        layers.sort(function(a , b){
            return a.index - b.index;
            });

        var is3D = isThreeDimensional(layers);

        // Secondary click drops the null at the comp centre and ignores the
        // selection's geometry entirely. Shift is meaningless in this mode -
        // there is nothing being averaged - but Alt and Ctrl / Cmd still
        // apply, because parenting is a separate decision from placement.
        var position;
        if(useCompCenter){
            position = getCompCenter(comp);
            }
        else{
            position = getAveragePosition(layers , mods.useArea);
            };
        if(!position) return;

        // Build the null with the selection's duration baked into its source,
        // instead of creating a comp length null and trimming the bar. The
        // null's actual duration now matches the selection.
        var range = getSelectionTimeRange(comp , layers);
        var masterNull = range ? comp.layers.addNull(range.duration) : comp.layers.addNull();

        masterNull.threeDLayer = is3D;
        masterNull.name = "MASTER - NULL";
        masterNull.moveBefore(layers[0]);

        // position is always [x,y,z]; a 2D null only accepts [x,y].
        masterNull.property(aeobj.match.transform)
                  .property(aeobj.match.position)
                  .setValue(is3D ? position : [position[0] , position[1]]);

        setNullTimeRange(masterNull , range);

        // Alt skips parenting entirely.
        if(!mods.skipParenting) parentLayers(layers , masterNull , mods.forceReparent);

        return masterNull;
        };

    // -- boot -------------------------------------------------------------
    // pieFX loads this file headlessly, to reach the functions below, and sets
    // __pieFXHeadless while it does. Without this guard the palette would open
    // in the middle of a gesture the first time each session. Running the
    // script normally - from the Scripts menu, or as a panel - is unaffected.
    if (!$.global.__pieFXHeadless) { showUI(thisObj); }

    // Public surface of the _mn global. Everything else stays private to
    // this closure.
    return {
        scriptName    : scriptName,
        version       : version,
        aeobj         : aeobj,
        addMasterNull : addMasterNull
        };

})(this);
