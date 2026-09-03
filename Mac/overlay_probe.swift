// Step 1 of MAC_PORT.md is "a measurement, not a build". This is the
// measuring instrument.
//
// It reports what the window server actually believes about every on-screen
// window, so the four properties the overlay has to hold can be checked as
// facts rather than as impressions:
//
//   1. transparent    -> alpha, and whether the window is opaque
//   2. click-through  -> NOT observable here; CGWindowList does not expose
//                        ignoresMouseEvents. It has to be tested by hand, by
//                        clicking AE through the wheel. Said out loud rather
//                        than silently omitted, because a probe that appears
//                        to cover four properties and covers three is worse
//                        than one that covers three.
//   3. all displays   -> the window's bounds against the union of the screens
//   4. above AE       -> window LEVEL, and the z-order of the list itself,
//                        which CGWindowListCopyWindowInfo returns front-to-back
//
// Run it with the overlay up (and, for the interesting answer, with After
// Effects up too):
//
//     swift Mac/overlay_probe.swift
//
import Cocoa

// The named levels, so the number that comes back is readable. AE's own
// windows sit at normal (0); the S3 spike proved a hand-built NSWindow holds
// above AE at NSStatusWindowLevel (25).
let levels: [Int: String] = [
    Int(CGWindowLevelForKey(.normalWindow)): "normal",
    Int(CGWindowLevelForKey(.floatingWindow)): "floating",
    Int(CGWindowLevelForKey(.modalPanelWindow)): "modalPanel",
    Int(CGWindowLevelForKey(.mainMenuWindow)): "mainMenu",
    Int(CGWindowLevelForKey(.statusWindow)): "status",
    Int(CGWindowLevelForKey(.popUpMenuWindow)): "popUpMenu",
    Int(CGWindowLevelForKey(.screenSaverWindow)): "screenSaver",
]

print("screens (AppKit points, origin bottom-left):")
var union = NSRect.zero
for s in NSScreen.screens {
    union = union.isEmpty ? s.frame : union.union(s.frame)
    print(String(format: "  %-22@ %@  scale %.1f",
                 s.localizedName as NSString,
                 NSStringFromRect(s.frame) as NSString,
                 s.backingScaleFactor))
}
print("  union: \(NSStringFromRect(union))")
if NSScreen.screens.count < 2 {
    print("  NOTE: one display. Property 3 (spanning all displays) is NOT")
    print("        under test on this machine — attach a second one.")
}

// Front-to-back, on-screen only, excluding desktop elements.
let opts: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
guard let raw = CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: Any]] else {
    print("no window list (needs Screen Recording permission for other apps' TITLES,")
    print("but bounds/level/owner come back without it)")
    exit(1)
}

print("\nwindows, front to back (z-order as the window server has it):")
print(String(format: "  %-4@ %-26@ %-11@ %-6@ %@",
             "z" as NSString, "owner" as NSString, "level" as NSString,
             "alpha" as NSString, "bounds (CG points, origin top-left)" as NSString))

var z = 0
for w in raw {
    let owner = w[kCGWindowOwnerName as String] as? String ?? "?"
    // Only the two processes this measurement is about, plus anything that
    // manages to sit between them — which is the failure this is looking for.
    let interesting = owner.contains("pieFX") || owner.contains("After Effects")
    let layer = w[kCGWindowLayer as String] as? Int ?? 0
    let alpha = w[kCGWindowAlpha as String] as? Double ?? 1
    guard interesting else { z += 1; continue }
    var b = CGRect.zero
    if let d = w[kCGWindowBounds as String] {
        b = CGRect(dictionaryRepresentation: d as! CFDictionary) ?? .zero
    }
    let name = levels[layer].map { "\($0)(\(layer))" } ?? "\(layer)"
    print(String(format: "  %-4d %-26@ %-11@ %-6.2f %@",
                 z, owner as NSString, name as NSString, alpha,
                 NSStringFromRect(b) as NSString))
    z += 1
}

let hasOverlay = raw.contains { ($0[kCGWindowOwnerName as String] as? String ?? "").contains("pieFX") }
let hasAE = raw.contains { ($0[kCGWindowOwnerName as String] as? String ?? "").contains("After Effects") }
print("")
if !hasOverlay { print("VERDICT: no pieFX window on screen — nothing to measure.") }
else if !hasAE  { print("VERDICT: overlay is up, but After Effects is NOT. Property 4 (above AE)") 
                  print("         is untested; the z-order above is against the desktop only.") }
else {
    // The whole question, reduced to one comparison: does anything belonging to
    // After Effects come BEFORE the overlay in a front-to-back list?
    var firstOverlay = Int.max, firstAE = Int.max, i = 0
    for w in raw {
        let o = w[kCGWindowOwnerName as String] as? String ?? ""
        if o.contains("pieFX"), firstOverlay == Int.max { firstOverlay = i }
        if o.contains("After Effects"), firstAE == Int.max { firstAE = i }
        i += 1
    }
    print(firstOverlay < firstAE
        ? "VERDICT: overlay is ABOVE After Effects (z \(firstOverlay) vs \(firstAE))."
        : "VERDICT: overlay is BELOW After Effects (z \(firstOverlay) vs \(firstAE)) — property 4 FAILS.")
    print("         Click-through and focus-stealing are still by hand.")
}
