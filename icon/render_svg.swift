//	pieFX — render an SVG to a transparent PNG, at any size.
//
//	Exists because `qlmanage -t` does NOT do this. It reports `hasAlpha: yes`
//	and then hands back an image whose corner pixel is opaque WHITE — it
//	composites the page onto a white card, because it is a thumbnailer and a
//	thumbnail is meant to look like a document. An icon built from that render
//	has a white box behind it at every size, and nothing in the pipeline would
//	have said so.
//
//	WebKit renders the same SVG with a genuinely transparent background, so
//	long as three things are set: the web view must not draw its own
//	background, the page body must not have one, and the snapshot must be
//	taken after a screen update.
//
//	  swift icon/render_svg.swift <in.svg> <out.png> <pixels>
import AppKit
import WebKit

let args = CommandLine.arguments
guard args.count == 4, let px = Int(args[3]) else {
    FileHandle.standardError.write("usage: render_svg.swift <in.svg> <out.png> <pixels>\n".data(using: .utf8)!)
    exit(2)
}
let inURL = URL(fileURLWithPath: args[1])
let outURL = URL(fileURLWithPath: args[2])

guard let svg = try? String(contentsOf: inURL, encoding: .utf8) else {
    FileHandle.standardError.write("cannot read \(inURL.path)\n".data(using: .utf8)!); exit(1)
}

let app = NSApplication.shared
app.setActivationPolicy(.prohibited)          // no Dock icon while rendering

let cfg = WKWebViewConfiguration()
let view = WKWebView(frame: NSRect(x: 0, y: 0, width: px, height: px), configuration: cfg)
//	The web view's own background. Without this the snapshot is white whatever
//	the page does.
view.setValue(false, forKey: "drawsBackground")

//	The SVG is inlined into a page with a transparent body and no margin, and
//	sized to fill exactly, so the output is the artwork and nothing else.
let html = """
<!doctype html><html><head><meta charset="utf-8"><style>
  html,body { margin:0; padding:0; background:transparent; }
  svg { display:block; width:\(px)px; height:\(px)px; }
</style></head><body>\(svg)</body></html>
"""

final class Done: NSObject, WKNavigationDelegate {
    let px: Int, out: URL
    init(px: Int, out: URL) { self.px = px; self.out = out }
    func webView(_ w: WKWebView, didFinish n: WKNavigation!) {
        let snap = WKSnapshotConfiguration()
        snap.rect = NSRect(x: 0, y: 0, width: px, height: px)
        snap.afterScreenUpdates = true
        //	1.0, so `px` means PIXELS and not points — on a 2x display the
        //	default would silently produce a 2048 image for a 1024 request.
        snap.snapshotWidth = NSNumber(value: px)
        w.takeSnapshot(with: snap) { image, err in
            guard let image, err == nil else {
                FileHandle.standardError.write("snapshot failed: \(err?.localizedDescription ?? "?")\n".data(using: .utf8)!)
                exit(1)
            }
            guard let tiff = image.tiffRepresentation,
                  let rep = NSBitmapImageRep(data: tiff),
                  let png = rep.representation(using: .png, properties: [:]) else {
                FileHandle.standardError.write("encode failed\n".data(using: .utf8)!); exit(1)
            }
            do { try png.write(to: self.out) } catch {
                FileHandle.standardError.write("write failed: \(error)\n".data(using: .utf8)!); exit(1)
            }
            print("    \(self.out.lastPathComponent): \(rep.pixelsWide)x\(rep.pixelsHigh)")
            exit(0)
        }
    }
    func webView(_ w: WKWebView, didFail n: WKNavigation!, withError e: Error) {
        FileHandle.standardError.write("load failed: \(e)\n".data(using: .utf8)!); exit(1)
    }
}
let done = Done(px: px, out: outURL)
view.navigationDelegate = done
view.loadHTMLString(html, baseURL: nil)

//	A hard ceiling, so a render that never finishes fails the build instead of
//	hanging it.
DispatchQueue.main.asyncAfter(deadline: .now() + 30) {
    FileHandle.standardError.write("timed out\n".data(using: .utf8)!); exit(1)
}
app.run()
