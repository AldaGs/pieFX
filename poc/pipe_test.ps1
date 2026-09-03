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
# `--effects` is the same idea for the effect catalogue: the search window opens
# on a fixture with one of each of the catalogue's sharp edges in it (a display
# name that collides across categories, an `_Obsolete` entry, two internal ones,
# a match name at the 31-character cap) rather than on whatever this machine has
# installed. Under `--settings none` the recents file is disabled too, so a run
# of this script cannot write into the developer's own recents.
$fixture = Join-Path $PSScriptRoot "overlay\src\effects-sample.json"
$args = @("--events", "\\.\pipe\pieFX-test99", "--actions", "\\.\pipe\pieFX-cmd-test99",
          "--settings", "none", "--effects", $fixture)
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

# DISTANCE MATTERS NOW. Under the shipped `distance` arming rule the radius is
# the depth, and the threshold is the CENTRE hexagon's edge (54px): clear the
# middle and you are on a child. So every stroke here that leaves the dead zone
# selects a child, and the labels say so.
#
# WHAT IS NOT COVERED, deliberately: a category's DEFAULT action. The band that
# reaches it is now DEAD(49.7px) to ARM_DIST(54px) - about four pixels - so
# under this rule a default is not reachable by hand and there is nothing
# honest to assert about it. It is still reachable under `center` and `exit`,
# neither of which this script exercises; see HANDOFF.md.
#
# These two used to be labelled "default" at 300px out. They passed anyway, one
# because Comp's default and its S child happen to be the same command and the
# other because nothing checked WHICH snippet came back. A test that passes for
# the wrong reason is the thing this script exists to prevent, and it has now
# caught itself doing it twice.

# Comp's default and its S child are the SAME command, so a straight-out S
# stroke cannot tell those two apart and must not be asked to. The second leg
# turns to N instead, to `Comp Settings`, which nothing else can produce.
Fire "S > Queue to Render " @($summon, '{"type":"cursor","x":800,"y":600}')
Fire "S > Comp Settings   " @($summon, '{"type":"cursor","x":800,"y":800}', '{"type":"cursor","x":800,"y":200}')
Fire "NE > Area Center    " @($summon, '{"type":"cursor","x":887,"y":450}')
Fire "NE drill -> N     " @($summon, '{"type":"cursor","x":1060,"y":350}', '{"type":"cursor","x":802,"y":502}', '{"type":"cursor","x":800,"y":200}')
Fire "NW anchor c0      " @($summon, '{"type":"cursor","x":540,"y":350}', '{"type":"cursor","x":802,"y":502}', '{"type":"cursor","x":760,"y":460}')
Fire "SW layer -> N     " @($summon, '{"type":"cursor","x":540,"y":650}', '{"type":"cursor","x":802,"y":502}', '{"type":"cursor","x":800,"y":200}')
# The case the whole arming change was made for: ONE unbroken outward stroke
# into the child that lies in the parent's own direction. Neither of the older
# rules could do this - `center` held every child inert until the cursor came
# back to the middle, and `exit` held THIS child inert specifically.
Fire "SE straight through " @($summon, '{"type":"cursor","x":870,"y":540}', '{"type":"cursor","x":1060,"y":650}')


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

# The mirror of the stroke above: the centre still cancels, even though under
# `distance` the centre is INSIDE the arming radius rather than the thing that
# arms you. Silence is the pass, so this is an Expect - a Fire would time out
# and leave its read pending on the stream, which breaks every check after it.
Write-Output "cancel:"
Expect "SE opened, released in centre " @($summon, '{"type":"cursor","x":1060,"y":650}', '{"type":"cursor","x":803,"y":501}') "silence"

Write-Output "context gating:"
Expect "no-sel NE (Master Null, dead) " @($noSel, '{"type":"cursor","x":1060,"y":350}') "silence"
Expect "no-sel S  (Render Queue, live)" @($noSel, '{"type":"cursor","x":800,"y":800}') "fire"
Expect "no-sel SE>S (Comp, live)      " @($noSel, '{"type":"cursor","x":1060,"y":650}', '{"type":"cursor","x":802,"y":502}', '{"type":"cursor","x":800,"y":800}') "fire"
Expect "no-comp SE>N (Solid, dead)    " @($noComp, '{"type":"cursor","x":1060,"y":650}', '{"type":"cursor","x":802,"y":502}', '{"type":"cursor","x":800,"y":200}') "silence"
Expect "no-comp S (Render Queue, dead)" @($noComp, '{"type":"cursor","x":800,"y":800}') "silence"

# --- the effect search ------------------------------------------------------
# N is `Effects`, and it is the one slot whose release does NOT cross the pipe:
# it opens a focused window in this process, because a search needs a keyboard
# and nothing in pieFX can take a keystroke. So silence on the action pipe is
# only half the assertion - silence is also what a slot that fires nothing
# looks like, which is exactly what this widget WAS. The other half is the
# overlay's own log saying the window was built.
#
# It really does open a window, and it really does take the foreground for a
# moment. That is the feature.
Write-Output "effect search:"
$log = Join-Path $env:TEMP "piefx_overlay.log"
$before = if (Test-Path $log) { (Select-String -Path $log -Pattern "search window" -SimpleMatch).Count } else { 0 }
Expect "N Effects (nothing on the pipe)" @($summon, '{"type":"cursor","x":800,"y":200}') "silence"
Start-Sleep -Milliseconds 1200
$after = if (Test-Path $log) { (Select-String -Path $log -Pattern "search window" -SimpleMatch).Count } else { 0 }
if ($after -gt $before) {
    Write-Output "  search window opened (overlay log)  -> PASS"
} else {
    Write-Output "  search window opened (overlay log)  -> FAIL (no log line; the release fired nothing)"
}

# Two layers selected: the search must NOT open. The plug-in applies an effect
# through AEGP_GetActiveLayer, which returns a layer only when exactly one is
# selected, so a search that opened here would take a query and an Enter and
# then apply nothing. Both halves are asserted - silence on the pipe, and NO new
# window in the log - because silence alone was also what the old mock did.
$twoSel = '{"type":"summon","x":800,"y":500,"hasSelection":true,"hasComp":true,"layerCount":2}'
$before2 = if (Test-Path $log) { (Select-String -Path $log -Pattern "search window" -SimpleMatch).Count } else { 0 }
Expect "N Effects, 2 layers selected  " @($twoSel, '{"type":"cursor","x":800,"y":200}') "silence"
Start-Sleep -Milliseconds 1000
$after2 = if (Test-Path $log) { (Select-String -Path $log -Pattern "search window" -SimpleMatch).Count } else { 0 }
if ($after2 -eq $before2) {
    Write-Output "  no window opened for a multi-selection -> PASS"
} else {
    Write-Output "  no window opened for a multi-selection -> FAIL (it opened anyway)"
}

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

# Only THIS run's overlay is this run's business. The check used to count every
# pieFX-overlay on the machine, so anyone with After Effects open - which owns a
# perfectly healthy overlay of its own - got a WARNING about a leak that was not
# there. A harness that cries transport bug at a normal desktop is the same
# failure as one that passes for the wrong reason, pointed the other way.
$others = @(Get-Process pieFX-overlay -ErrorAction SilentlyContinue |
            Where-Object { $_.Id -ne $proc.Id })
if (-not $proc.HasExited) {
    Write-Output "WARNING: this run's overlay ($($proc.Id)) is still running"
} else {
    Write-Output "no stray overlay from this run"
}
if ($others.Count) {
    Write-Output "  note: $($others.Count) other overlay(s) alive ($($others.Id -join ', ')) - an open After Effects owns one; not this run's"
}
Write-Output "done"
