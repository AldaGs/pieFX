// Can ONE window span two displays on this Mac?
//
// "Displays have separate Spaces" is ON by default, and the received wisdom is
// that it stops a window covering two screens. If that is true here, the
// overlay cannot use the Windows shape (one window over the whole virtual
// desktop) and has to move to the cursor's screen on each summon instead.
//
// Deliberately NOT built on Tauri. Tauri's own geometry is already known to be
// wrong on a mixed-DPI desktop — it computes the union in "physical px", where
// each monitor's position is scaled by its OWN factor — so a Tauri window that
// failed to span would not tell us whether Spaces or the arithmetic did it.
// This is a hand-built NSWindow with a frame taken straight from NSScreen, the
// same way S3 was built, so the only variable left is the window server.
//
//     swift Mac/span_test.swift
//
// It puts a translucent sheet over every screen with a bright marker at each
// screen's centre, holds for 20 seconds, and exits by itself.
import Cocoa

let app = NSApplication.shared
// Accessory, so this measurement cannot itself steal the foreground and
// contaminate what it is measuring.
app.setActivationPolicy(.accessory)

let screens = NSScreen.screens
guard !screens.isEmpty else { print("no screens"); exit(1) }

var union = screens[0].frame
for s in screens { union = union.union(s.frame) }

print("screens (AppKit points, origin bottom-left):")
for s in screens {
    print("  \(s.localizedName)  \(NSStringFromRect(s.frame))  scale \(s.backingScaleFactor)")
}
print("  union asked for: \(NSStringFromRect(union))")
if screens.count < 2 {
    print("\nONE display attached — this test has nothing to measure.")
    exit(1)
}

// The markers: one per screen, at its centre, in the window's own coordinates.
final class Sheet: NSView {
    var screensInWindowSpace: [(String, NSRect)] = []
    override func draw(_ dirty: NSRect) {
        // A wash over the whole frame, so the window's true extent is visible
        // as a shape rather than inferred from the markers.
        NSColor.systemBlue.withAlphaComponent(0.18).setFill()
        bounds.fill()
        NSColor.systemRed.withAlphaComponent(0.85).setFill()
        for (name, r) in screensInWindowSpace {
            let c = NSRect(x: r.midX - 140, y: r.midY - 60, width: 280, height: 120)
            c.fill()
            let attrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.boldSystemFont(ofSize: 20),
                .foregroundColor: NSColor.white,
            ]
            (name as NSString).draw(at: NSPoint(x: c.minX + 14, y: c.midY - 10), withAttributes: attrs)
        }
    }
}

let win = NSWindow(contentRect: union,
                   styleMask: .borderless,
                   backing: .buffered,
                   defer: false)
win.level = NSWindow.Level(rawValue: 25) // NSStatusWindowLevel, as the overlay uses
win.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary, .ignoresCycle]
win.isOpaque = false
win.backgroundColor = .clear
win.ignoresMouseEvents = true
win.hasShadow = false

let sheet = Sheet(frame: NSRect(origin: .zero, size: union.size))
sheet.screensInWindowSpace = screens.map { s in
    // NSScreen frames are in the global space; the window's content view starts
    // at the union's origin, so shift each screen into window coordinates.
    (s.localizedName,
     NSRect(x: s.frame.minX - union.minX, y: s.frame.minY - union.minY,
            width: s.frame.width, height: s.frame.height))
}
win.contentView = sheet
// setFrame AFTER construction as well: a borderless window can still be
// constrained on creation, and this is the call the overlay would make.
win.setFrame(union, display: true)
win.orderFrontRegardless()

print("  frame the window ACTUALLY got: \(NSStringFromRect(win.frame))")
if win.frame == union {
    print("  -> the window server accepted the spanning frame.")
} else {
    print("  -> REFUSED: the frame came back different. Spanning is constrained.")
}
print("")
print("LOOK AT BOTH SCREENS NOW (20 seconds).")
print("  Both washed blue, a red marker on each -> spanning WORKS; Spaces is not")
print("     a problem, and the overlay can keep the Windows one-window shape.")
print("  Only one screen shows anything -> Spaces is clipping it, and the")
print("     overlay must move to the cursor's screen on each summon.")

Timer.scheduledTimer(withTimeInterval: 20.0, repeats: false) { _ in
    print("done")
    NSApp.terminate(nil)
}
app.run()
