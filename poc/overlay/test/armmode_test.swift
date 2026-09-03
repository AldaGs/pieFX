//	pieFX — the arm-mode migration, without a browser.
//
//	"exit" (arm on leaving the direction you came from) was removed. Removing a
//	settings VALUE is not like removing a UI control: files naming it already
//	exist on disk, and an unrecognised arm mode would leave the wheel unable to
//	arm at level 2 at all — every child permanently unselectable, with nothing
//	logged. So the coercion in parseSettings is load-bearing and is worth a test.
//
//	There is no Node in this project on purpose (frontendDist is static, so
//	cargo alone builds the overlay), and macOS ships no `jsc` binary. JSContext
//	is the engine that is already here. It has no module loader, so the `export`
//	keywords are stripped before evaluation — a mechanical transform of the REAL
//	menu.js, not a copy of it, which is the property that matters.
//
//	  swift poc/overlay/test/armmode_test.swift
import Foundation
import JavaScriptCore

let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let menu = here.deletingLastPathComponent().appendingPathComponent("src/menu.js")

guard var src = try? String(contentsOf: menu, encoding: .utf8) else {
    print("cannot read \(menu.path)"); exit(1)
}
//	`export function f` -> `function f`, `export const X` -> `const X`, and the
//	trailing re-export list dropped.
src = src.replacingOccurrences(of: #"(?m)^export \{[^}]*\};?$"#, with: "",
                               options: .regularExpression)
src = src.replacingOccurrences(of: #"(?m)^export "#, with: "",
                               options: .regularExpression)

let ctx = JSContext()!
ctx.exceptionHandler = { _, e in print("  JS EXCEPTION: \(e?.toString() ?? "?")"); exit(1) }
ctx.evaluateScript(src)

var pass = 0, fail = 0
func check(_ ok: Bool, _ what: String) {
    print("  [\(ok ? "ok" : "FAIL")] \(what)"); ok ? (pass += 1) : (fail += 1)
}

//	A minimal but VALID settings file: parseSettings rejects one with no
//	wheel.slots, and a rejected file would return defaults and make every
//	assertion below pass for the wrong reason.
func settings(_ mode: String?) -> String {
    let g = mode.map { "\"armMode\": \($0), \"holdMs\": 200" } ?? "\"holdMs\": 200"
    return """
    { "gesture": { \(g) },
      "wheel": { "slots": [ { "label": "x", "action": { "kind": "builtin", "name": "copy-frame" } } ] } }
    """
}
func armMode(_ json: String) -> String? {
    ctx.evaluateScript("JSON.stringify(parseSettings(\(json.debugDescription)).gesture.armMode)")?
        .toString().flatMap { $0 == "null" ? nil : $0.trimmingCharacters(in: CharacterSet(charactersIn: "\"")) }
}
func slotCount(_ json: String) -> Int {
    Int(ctx.evaluateScript("parseSettings(\(json.debugDescription)).wheel.slots.length")?.toInt32() ?? -1)
}

print("pieFX_armmode_test\n")
print("the removed mode is gone from the list")
let modes = ctx.evaluateScript("ARM_MODES.join(',')")!.toString()!
check(!modes.contains("exit"), "\"exit\" is not an arm mode")
check(modes == "distance,center", "two modes remain: \(modes)")

print("\nmigration — a settings file that still names it")
check(armMode(settings("\"exit\"")) == "distance", "\"exit\" becomes \"distance\"")
check(slotCount(settings("\"exit\"")) == 1,
      "and the user's slot tree SURVIVES — coerced, not rejected")

print("\nthe surviving modes are untouched")
for m in ["distance", "center"] {
    check(armMode(settings("\"\(m)\"")) == m, "\"\(m)\" is kept as-is")
}

print("\nanything else lands in the same place")
for m in ["\"\"", "null", "\"nonsense\"", "42"] {
    check(armMode(settings(m)) == "distance", "\(m) -> \"distance\"")
}
check(armMode(settings(nil)) == "distance", "no armMode key at all -> the default")

//	The hold threshold, which had the same shape of bug as the arm mode: a
//	value the user could set, that the wheel then did not use. Two halves —
//	the plug-in never received it (fixed in pieFX_gesture.mm), and the range
//	was enforced in three places with three different answers.
print("\nthe hold threshold's range is ONE range")
let lo = ctx.evaluateScript("HOLD_MS_MIN")!.toInt32()
let hi = ctx.evaluateScript("HOLD_MS_MAX")!.toInt32()
check(lo == 80 && hi == 2000, "HOLD_MS_MIN..MAX is \(lo)..\(hi), matching the plug-in's clamp")

func holdMs(_ v: String) -> Int32 {
    let json = """
    { "gesture": { "holdMs": \(v) },
      "wheel": { "slots": [ { "label": "x", "action": { "kind": "builtin", "name": "copy-frame" } } ] } }
    """
    return ctx.evaluateScript("parseSettings(\(json.debugDescription)).gesture.holdMs")!.toInt32()
}
//	3000 is the value that started this: typed into the settings window, stored
//	as 3000, and silently clamped to 2000 inside the plug-in where nobody could
//	see it.
check(holdMs("3000") == 2000, "3000 is clamped to 2000 in the FILE, not silently in the plug-in")
check(holdMs("0") == 80, "0 is raised to 80 — a zero would take away AE's context menu")
check(holdMs("600") == 600, "a value in range is untouched")
check(holdMs("\"nonsense\"") == 200, "a non-number falls back to the default")

print("\n\(pass) passed, \(fail) failed")
exit(fail == 0 ? 0 : 1)
