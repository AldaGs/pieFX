<#
	Build the Windows installer: stage the payload, check it, compile it.

	The Windows counterpart of Mac/package.sh, and it has the same job -- make
	the thing somebody else installs -- plus one check that file does not need.

	  .\Win\build_installer.ps1
	  .\Win\build_installer.ps1 -Version 0.2.0 -Rebuild

	The output is Win\build\pieFX-<version>-win-setup.exe.

	IT IS UNSIGNED, and on Windows that is not the free choice it turned out to
	be on macOS. A downloaded unsigned .exe gets SmartScreen's full-screen
	"Windows protected your PC" panel with the Run button behind "More info" --
	whereas a plain .zip of the same files does not. Signing is a real decision
	with a real price, not a formality; see docs/HANDOFF.md. When a certificate
	exists, one signtool call goes at the end of this script and nothing else
	changes.
#>

[CmdletBinding()]
param(
	[string] $Version = "0.1.0",
	[switch] $Rebuild,             # rebuild the .aex and the overlay first
	[switch] $KeepStage
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root    = Split-Path -Parent $Here
$Build   = Join-Path $Here  "build"
$Stage   = Join-Path $Build "stage"

function Fail([string] $Message) {
	Write-Host "!! $Message" -ForegroundColor Red
	exit 1
}

function Step([string] $Message) {
	Write-Host "== $Message" -ForegroundColor Cyan
}

#	--- the tools -----------------------------------------------------------

$Iscc = @(
	"$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
	"${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
	"$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $Iscc) {
	Fail ("Inno Setup 6 not found. Install it with:`n" +
	      "     winget install --id JRSoftware.InnoSetup")
}

#	--- optionally rebuild --------------------------------------------------

if ($Rebuild) {
	$MSBuild = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio" `
		-Recurse -Filter MSBuild.exe -ErrorAction SilentlyContinue |
		Where-Object { $_.FullName -match "Current\\Bin\\amd64\\MSBuild\.exe$" } |
		Select-Object -First 1 -ExpandProperty FullName
	if (-not $MSBuild) { Fail "MSBuild not found; build the .aex by hand and re-run without -Rebuild." }

	#	poc\native\Win, and no other. The repo used to contain a second
	#	project that emitted a pieFX.aex over the top of this one; it is
	#	archived now, and the payload check below is the belt to that braces.
	Step "building the plug-in (Release|x64)"
	& $MSBuild (Join-Path $Root "poc\native\Win\pieFX.sln") `
		-p:Configuration=Release -p:Platform=x64 -v:minimal -nologo
	if ($LASTEXITCODE -ne 0) { Fail "the plug-in did not build" }

	Step "building the overlay (release)"
	Push-Location (Join-Path $Root "poc\overlay\src-tauri")
	try {
		& cargo build --release
		if ($LASTEXITCODE -ne 0) { Fail "the overlay did not build" }
	} finally { Pop-Location }
}

#	--- find the payload ----------------------------------------------------

$AexCandidates = @()
if ($env:AE_PLUGIN_BUILD_DIR) {
	$AexCandidates += (Join-Path $env:AE_PLUGIN_BUILD_DIR "AEGP\pieFX.aex")
}
$AexCandidates += (Join-Path $Root "..\..\..\..\_build_out\AEGP\pieFX.aex")

$Aex = $AexCandidates |
	Where-Object { Test-Path $_ } |
	Select-Object -First 1
if (-not $Aex) {
	Fail ("pieFX.aex not found. Looked in:`n     " +
	      ($AexCandidates -join "`n     ") +
	      "`n   Build poc\native\Win\pieFX.sln, or re-run with -Rebuild.")
}
$Aex = (Resolve-Path $Aex).Path

$Overlay = Join-Path $Root "poc\overlay\src-tauri\target\release\pieFX-overlay.exe"
if (-not (Test-Path $Overlay)) {
	Fail ("pieFX-overlay.exe not found at`n     $Overlay`n" +
	      "   cd poc\overlay\src-tauri; cargo build --release")
}

$Scripts = Join-Path $Root "poc\scripts"
if (-not (Test-Path $Scripts)) { Fail "poc\scripts is missing" }

#	--- check the payload IS THE PRODUCT ------------------------------------
#
#	Mac/package.sh refuses to ship a single-architecture overlay, because the
#	whole point of that package is the other architecture. This is the same
#	kind of check for the mistake this platform actually made: the Phase 0
#	spike and the product were two projects emitting the same pieFX.aex into
#	the same folder, and the spike compiles just as cleanly and installs just
#	as happily. The only thing that tells them apart is what is inside, so
#	that is what gets read. Never trust a build log for this.

Step "checking the .aex is the product and not the Phase 0 spike"
$Bytes = [System.IO.File]::ReadAllBytes($Aex)
$Text  = [System.Text.Encoding]::ASCII.GetString($Bytes)
if ($Text -notlike "*pieFX (Show/Hide)*") {
	Fail ("$Aex does not contain the product's menu name `"pieFX (Show/Hide)`".`n" +
	      "   Build poc\native\Win\pieFX.sln -- NOT _archive\phase0-spike-win.")
}
if ($Text -like "*Anchor to Center*") {
	Fail ("$Aex is the PHASE 0 SPIKE (it contains `"Anchor to Center`").`n" +
	      "   Its Window menu reads `"pieFX S1 (Anchor to Center)`". Build`n" +
	      "   poc\native\Win\pieFX.sln and run this again.")
}
Write-Host "   ok -- product menu name present, spike menu name absent"

#	--- stage ---------------------------------------------------------------

Step "staging"
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage -Force | Out-Null

Copy-Item $Aex     (Join-Path $Stage "pieFX.aex")
Copy-Item $Overlay (Join-Path $Stage "pieFX-overlay.exe")
Copy-Item $Scripts (Join-Path $Stage "scripts") -Recurse

#	The scripts folder is not decoration: a slot whose action has a relative
#	`file` resolves it against the install directory, so shipping without it
#	produces a wheel whose Master Null slot throws. Named individually because
#	a silently-empty folder would pass a mere Test-Path.
foreach ($s in @("ag_masterNull.jsx", "ag_localeProbe.jsx")) {
	if (-not (Test-Path (Join-Path $Stage "scripts\$s"))) { Fail "scripts\$s missing from the payload" }
}

$StagedSize = (Get-ChildItem $Stage -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("   {0} files, {1:N1} MB" -f `
	(Get-ChildItem $Stage -Recurse -File).Count, ($StagedSize / 1MB))

#	--- compile -------------------------------------------------------------

Step "compiling the installer"
& $Iscc `
	"/DMyAppVersion=$Version" `
	"/DStageDir=$Stage" `
	(Join-Path $Here "pieFX.iss")
if ($LASTEXITCODE -ne 0) { Fail "ISCC failed" }

if (-not $KeepStage) { Remove-Item $Stage -Recurse -Force }

$Setup = Join-Path $Build "pieFX-$Version-win-setup.exe"
if (-not (Test-Path $Setup)) { Fail "ISCC reported success but $Setup is not there" }

Write-Host ""
Write-Host ("== {0}  ({1:N1} MB)" -f $Setup, ((Get-Item $Setup).Length / 1MB)) -ForegroundColor Green
Write-Host "   unsigned -- see the header of this script before shipping it"
