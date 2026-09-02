# Consolidation harness. Uses NON-default pipe names passed on the command line,
# which is the path a second AE instance takes, and probes the toast channel.
$ErrorActionPreference = "Stop"
$exe = "C:\AE_SDK\ae25.6_61.64bit.AfterEffectsSDK\Examples\Template\pieFX\poc\overlay\src-tauri\target\release\pieFX-overlay.exe"

$tx = New-Object System.IO.Pipes.NamedPipeServerStream("pieFX-test99", [System.IO.Pipes.PipeDirection]::Out, 1)
$rx = New-Object System.IO.Pipes.NamedPipeServerStream("pieFX-cmd-test99", [System.IO.Pipes.PipeDirection]::In, 1)

# `--settings none` pins the overlay to its BUILT-IN defaults. Everything this
# script asserts is a default binding, so without it the harness would pass or
# fail according to whatever the developer happens to have configured - and a
# failure would read as a transport bug, which is the one thing this script
# exists to catch.
$args = @("--events", "\\.\pipe\pieFX-test99", "--actions", "\\.\pipe\pieFX-cmd-test99",
          "--settings", "none")
$proc = Start-Process -FilePath $exe -PassThru -ArgumentList $args
Write-Output "overlay pid $($proc.Id) (custom pipe names)"

$t = $tx.WaitForConnectionAsync()
if (-not $t.Wait(15000)) { Write-Output "FAIL: TX never opened"; $proc.Kill(); exit 1 }
$t2 = $rx.WaitForConnectionAsync()
if (-not $t2.Wait(15000)) { Write-Output "FAIL: RX never opened"; $proc.Kill(); exit 1 }
Write-Output "PASS: connected on custom names (second-instance path works)"

$w = New-Object System.IO.StreamWriter($tx); $w.AutoFlush = $true
$r = New-Object System.IO.StreamReader($rx)

function Fire($label, $steps) {
    foreach ($s in $steps) { $w.WriteLine($s); Start-Sleep -Milliseconds 220 }
    $w.WriteLine('{"type":"release"}')
    $line = $r.ReadLineAsync()
    if ($line.Wait(8000)) { Write-Output "  $label -> $($line.Result)" }
    else { Write-Output "  $label -> FAIL (nothing)" }
}

$summon = '{"type":"summon","x":800,"y":500,"hasSelection":true,"layerCount":1}'

Fire "S verb        " @($summon, '{"type":"cursor","x":800,"y":800}')
Fire "NE default    " @($summon, '{"type":"cursor","x":1060,"y":350}')
Fire "NE drill -> N " @($summon, '{"type":"cursor","x":1060,"y":350}', '{"type":"cursor","x":802,"y":502}', '{"type":"cursor","x":800,"y":200}')
Fire "NW anchor c0  " @($summon, '{"type":"cursor","x":540,"y":350}', '{"type":"cursor","x":802,"y":502}', '{"type":"cursor","x":760,"y":460}')
Fire "SW layer -> N " @($summon, '{"type":"cursor","x":540,"y":650}', '{"type":"cursor","x":802,"y":502}', '{"type":"cursor","x":800,"y":200}')

# --- context gating (`requires`) ------------------------------------------
# Same strokes, summoned with nothing selected. A slot that needs a selection
# must stay silent; one that needs nothing must still fire. Silence is the PASS
# here, so it gets its own runner rather than reading as a failure.
# A read that times out stays pending on the stream — starting a second one
# throws "the stream is currently in use". So the pending task is kept and
# reused: it is the same stream in the same order, so whatever arrives next is
# exactly what the next release produced.
$script:pending = $null
function Expect($label, $steps, $want) {
    foreach ($s in $steps) { $w.WriteLine($s); Start-Sleep -Milliseconds 220 }
    $w.WriteLine('{"type":"release"}')
    if (-not $script:pending) { $script:pending = $r.ReadLineAsync() }
    $got = ""
    if ($script:pending.Wait(3000)) {
        $got = $script:pending.Result
        $script:pending = $null
    }
    $ok = if ($want -eq "silence") { $got -eq "" } else { $got -ne "" }
    Write-Output "  $label -> $(if ($ok) { 'PASS' } else { 'FAIL' })  $got"
}

$noSel = '{"type":"summon","x":800,"y":500,"hasSelection":false,"hasComp":true,"layerCount":0}'
$noComp = '{"type":"summon","x":800,"y":500,"hasSelection":false,"hasComp":false,"layerCount":0}'

Write-Output "context gating:"
Expect "no-sel NE (Master Null, dead) " @($noSel, '{"type":"cursor","x":1060,"y":350}') "silence"
Expect "no-sel S  (Render Queue, live)" @($noSel, '{"type":"cursor","x":800,"y":800}') "fire"
Expect "no-sel SE>S (Comp, live)      " @($noSel, '{"type":"cursor","x":1060,"y":650}', '{"type":"cursor","x":802,"y":502}', '{"type":"cursor","x":800,"y":800}') "fire"
Expect "no-comp SE>N (Solid, dead)    " @($noComp, '{"type":"cursor","x":1060,"y":650}', '{"type":"cursor","x":802,"y":502}', '{"type":"cursor","x":800,"y":200}') "silence"
Expect "no-comp S (Render Queue, dead)" @($noComp, '{"type":"cursor","x":800,"y":800}') "silence"

# toast channel: nothing comes back, it is one-way to the user
$w.WriteLine('{"type":"toast","level":"error","text":"_mn is undefined"}')
Write-Output "  toast sent (check overlay log for receipt)"
Start-Sleep -Milliseconds 800

# The quit message: the overlay must go on its own, while the pipe is still
# whole. This is the path AE's death hook takes, and it is the one that has to
# work — terminating a process that is blocked in a read on a half-dead pipe
# leaves it un-dead, which is exactly what happened in AE.
$w.WriteLine('{"type":"quit"}')
if ($proc.WaitForExit(5000)) {
    Write-Output "PASS: overlay quit on request"
} else {
    Write-Output "FAIL: overlay ignored quit; falling back to kill"
}

# Kill the TREE, not just the launcher. The overlay spawns WebView2 children,
# and one of these runs left something alive that held the release binary and
# made the next cargo build fail with "access denied" — the same leak the
# plug-in now uses a job object to prevent.
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
$proc.WaitForExit(4000) | Out-Null
$tx.Dispose(); $rx.Dispose()

$stray = @(Get-Process pieFX-overlay -ErrorAction SilentlyContinue)
if ($stray.Count) {
    Write-Output "WARNING: $($stray.Count) pieFX-overlay process(es) still running: $($stray.Id -join ', ')"
} else {
    Write-Output "no stray overlay processes"
}
Write-Output "done"
