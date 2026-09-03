;	pieFX -- Windows installer (Inno Setup 6)
;
;	Compile with Win\build_installer.ps1, not by hand: that script stages the
;	payload and REFUSES to package a .aex that is the Phase 0 spike rather than
;	the product. See _archive/phase0-spike-win/README.md for why that check
;	exists.
;
;	Three measured facts shape everything below.
;
;	1.	Every plug-in folder After Effects scans is under C:\Program Files.
;		AE's own "Plugin Loading.log" lists six roots and all six are there,
;		so there is no per-user location to install into and no way to avoid
;		elevation. The goal is therefore ONE UAC prompt and nothing else to
;		think about -- not zero prompts, which is not on offer.
;
;	2.	The registry knows where AE is and the filesystem does not. The
;		development machine has a
;		C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\Plug-ins
;		folder with no AfterFX.exe beside it -- a leftover from an uninstall.
;		A glob over Program Files would install into it. So versions come from
;		HKLM\SOFTWARE\Adobe\After Effects\<ver>\InstallPath and are then
;		confirmed by the presence of AfterFX.exe.
;
;	3.	The payload is THREE things that must land together: pieFX.aex, the
;		overlay .exe the plug-in launches, and the scripts folder that slot
;		actions resolve relative paths against. A user who copies only the
;		.aex out of a zip gets a plug-in that loads, arms, and never shows a
;		wheel. Shipping them as one atomic install is most of the reason this
;		file exists.

#define AppName        "pieFX"
#define AppPublisher   "Aldair Gonzalez"
#define AppURL         "https://github.com/AldaGs/pieFX"

;	Passed in by build_installer.ps1 (-DMyAppVersion=...). The fallback is only
;	so that opening this file in the Inno IDE still compiles.
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif

;	Where build_installer.ps1 staged the payload.
#ifndef StageDir
  #define StageDir "build\stage"
#endif

[Setup]
AppId={{7F3C1A62-9E44-4B7E-A0D1-2C5F8B6E4A11}
AppName={#AppName}
AppVersion={#MyAppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
VersionInfoVersion={#MyAppVersion}

;	pieFX does not live in Program Files\pieFX -- it lives inside each After
;	Effects install that was ticked on the versions page. {app} is therefore
;	never used for the payload, and the directory page is turned off so nobody
;	is asked a question whose answer would be ignored. {app} still has to be
;	SOMETHING for the uninstaller to live in, so it gets its own folder.
DefaultDirName={autopf}\pieFX
DisableDirPage=yes
DisableProgramGroupPage=yes
DefaultGroupName={#AppName}

;	The .aex goes under Program Files. There is no unprivileged path (fact 1),
;	so ask once, up front, rather than failing partway through a copy.
PrivilegesRequired=admin

;	Inno warns that an admin-mode install touches per-user areas, and it is
;	right to: the only per-user thing here is %APPDATA%\pieFX, and under
;	elevation that resolves to whichever profile answered the UAC prompt. The
;	warning is acknowledged rather than ignored -- see the comment above
;	CurUninstallStepChanged for what is done about it, and why leaving a
;	settings file behind is the safe direction to be wrong in.
UsedUserAreasWarning=no

;	The plug-in is x64 only, like After Effects.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

OutputDir=build
OutputBaseFilename=pieFX-{#MyAppVersion}-win-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName} {#MyAppVersion}
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
;	One block per detected After Effects, six slots deep. Inno wants its file
;	list at compile time and the number of AE installs is only known at run
;	time, so the list is unrolled and each entry asks whether its slot was
;	detected and ticked. Six is far more side-by-side AE versions than anyone
;	has; slots past the end are simply never selected.
;
;	Everything lands in <AE>\Support Files\Plug-ins\pieFX\ -- a SUBFOLDER, which
;	AE scans (its loading log says "and sub-directories", and every vendor in
;	that folder already does this). It keeps the three payload items together,
;	it makes the uninstall exact, and the plug-in finds its neighbours by
;	GetModuleFileName on itself, so nothing needs to know the extra level.
#define I
#sub FileEntries
Source: "{#StageDir}\pieFX.aex";         DestDir: "{code:AEPluginDir|{#I}}"; Flags: ignoreversion; Check: AESelected({#I})
Source: "{#StageDir}\pieFX-overlay.exe"; DestDir: "{code:AEPluginDir|{#I}}"; Flags: ignoreversion; Check: AESelected({#I})
Source: "{#StageDir}\scripts\*";         DestDir: "{code:AEPluginDir|{#I}}\scripts"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: AESelected({#I})
#endsub
#for {I = 0; I < 6; I++} FileEntries

[UninstallDelete]
;	effects.json is a CACHE the plug-in rebuilds by walking AE, and recents.json
;	is a convenience. Neither is the user's work, so both go. settings.json is
;	the user's own bindings and is handled in code, with a question.
Type: files; Name: "{userappdata}\pieFX\effects.json"
Type: files; Name: "{userappdata}\pieFX\recents.json"
Type: files; Name: "{userappdata}\pieFX\presets.json"

[Code]
const
  MaxAE = 6;

var
  AEVersion:  array[0..MaxAE - 1] of String;   { "26.0" }
  AELabel:    array[0..MaxAE - 1] of String;   { "After Effects 2026" }
  AESupport:  array[0..MaxAE - 1] of String;   { ...\Support Files\ }
  AECount:    Integer;
  AEPage:     TInputOptionWizardPage;

{	AE's registry version is 26.0; its folder is "Adobe After Effects 2026".
	The folder name is what a user recognises, so it is what the checkbox
	says -- read from the install path rather than computed from the number,
	because the mapping from 26.0 to 2026 is a naming convention and not a
	guarantee. }
function AEDisplayName(const SupportPath, Ver: String): String;
var
  P: String;
  I: Integer;
begin
  P := RemoveBackslash(SupportPath);       { ...\Adobe After Effects 2026\Support Files }
  P := ExtractFileDir(P);                  { ...\Adobe After Effects 2026 }
  I := Length(P);
  while (I > 0) and (P[I] <> '\') do I := I - 1;
  Result := Copy(P, I + 1, Length(P) - I);
  if Result = '' then
    Result := 'After Effects ' + Ver;
end;

{	Detection. The registry is the source of truth (fact 2), and AfterFX.exe
	beside the recorded path is the confirmation that the install is real and
	not the shell an uninstaller left behind. }
procedure DetectAE;
var
  Vers: TArrayOfString;
  I: Integer;
  Support: String;
begin
  AECount := 0;
  if not RegGetSubkeyNames(HKLM64, 'SOFTWARE\Adobe\After Effects', Vers) then
    Exit;
  for I := 0 to GetArrayLength(Vers) - 1 do
  begin
    if AECount >= MaxAE then
      Break;
    if not RegQueryStringValue(HKLM64, 'SOFTWARE\Adobe\After Effects\' + Vers[I],
                               'InstallPath', Support) then
      Continue;
    if not FileExists(AddBackslash(Support) + 'AfterFX.exe') then
      Continue;                            { a leftover folder, not an install }
    if not DirExists(AddBackslash(Support) + 'Plug-ins') then
      Continue;
    AEVersion[AECount] := Vers[I];
    AESupport[AECount] := AddBackslash(Support);
    AELabel[AECount]   := AEDisplayName(Support, Vers[I]);
    AECount := AECount + 1;
  end;
end;

{	Called by every [Files] entry, once per slot. }
function AEPluginDir(Param: String): String;
var
  Idx: Integer;
begin
  Idx := StrToIntDef(Param, -1);
  if (Idx < 0) or (Idx >= AECount) then
    Result := ExpandConstant('{tmp}\unused')
  else
    Result := AESupport[Idx] + 'Plug-ins\pieFX';
end;

function AESelected(Idx: Integer): Boolean;
begin
  Result := (Idx < AECount) and (AEPage <> nil) and AEPage.Values[Idx];
end;

{	After Effects holds an open handle on every .aex it has loaded, so a copy
	over a running AE fails partway and leaves a half-installed plug-in. Asked
	before anything is written, and asked again on the way out of the versions
	page, because the usual sequence is "start the installer, then remember to
	quit AE". }
function AEIsRunning: Boolean;
var
  Code: Integer;
begin
  Result := False;
  if Exec(ExpandConstant('{cmd}'),
          '/C tasklist /FI "IMAGENAME eq AfterFX.exe" /NH | find /I "AfterFX.exe" > nul',
          '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Result := (Code = 0);
end;

function WarnIfAERunning: Boolean;
begin
  Result := True;
  while AEIsRunning do
  begin
    if MsgBox('After Effects is running.' + #13#10#13#10 +
              'It keeps the plug-in file open, so installing now would fail '
              + 'halfway and leave pieFX half-installed. Quit After Effects, '
              + 'then click Retry.',
              mbError, MB_RETRYCANCEL) <> IDRETRY then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function InitializeSetup: Boolean;
begin
  DetectAE;
  if AECount = 0 then
  begin
    MsgBox('No After Effects installation was found.' + #13#10#13#10 +
           'pieFX installs into After Effects'' own Plug-ins folder, so there '
           + 'is nowhere to put it. If AE is installed, it did not register '
           + 'itself under HKLM\SOFTWARE\Adobe\After Effects -- install pieFX '
           + 'by hand instead; see the README.',
           mbCriticalError, MB_OK);
    Result := False;
    Exit;
  end;
  Result := WarnIfAERunning;
end;

procedure InitializeWizard;
var
  I: Integer;
begin
  AEPage := CreateInputOptionPage(wpWelcome,
    'After Effects versions',
    'Where should pieFX be installed?',
    'pieFX lives inside After Effects'' own Plug-ins folder, so one copy is '
    + 'installed per version. These were found on this machine.',
    False, False);            { checkboxes, not radio buttons }
  for I := 0 to AECount - 1 do
    AEPage.Add(AELabel[I] + '   (' + AEVersion[I] + ')');
  { Newest first in the registry is not guaranteed, so tick everything: a
    second AE version that silently did not get the plug-in is a worse
    outcome than an extra copy the uninstaller will take back out. }
  for I := 0 to AECount - 1 do
    AEPage.Values[I] := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  I: Integer;
  Any: Boolean;
begin
  Result := True;
  if CurPageID = AEPage.ID then
  begin
    Any := False;
    for I := 0 to AECount - 1 do
      if AEPage.Values[I] then
        Any := True;
    if not Any then
    begin
      MsgBox('Tick at least one After Effects version, or cancel.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
    Result := WarnIfAERunning;
  end;
end;

{	What the Ready page lists. Without this it would say nothing at all,
	because the payload's destination is not the app directory.
	NOTE: a Pascal comment ends at the first closing brace, so an Inno
	constant written out in one of these comments truncates it. That is what
	this comment used to do. }
function UpdateReadyMemo(Space, NewLine, MemoUserInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  I: Integer;
  S: String;
begin
  S := 'Install pieFX into:' + NewLine;
  for I := 0 to AECount - 1 do
    if AEPage.Values[I] then
      S := S + Space + AESupport[I] + 'Plug-ins\pieFX' + NewLine;
  Result := S;
end;

{	One caveat, stated rather than hidden: the uninstaller runs elevated, so
	the per-user AppData path below is the profile that answered the UAC
	prompt. On a normal single-account Windows machine -- the user IS the
	administrator and consents in place -- that is their own profile and this
	is correct. Where a DIFFERENT administrator account elevates, pieFX's
	settings are left where they are rather than deleted from the wrong
	profile, which is the safe direction to be wrong in. }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Dir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    { The user's own bindings. Kept unless they say otherwise, because an
      uninstall to fix a bad install should not cost someone their wheel. }
    Dir := ExpandConstant('{userappdata}\pieFX');
    if FileExists(Dir + '\settings.json') then
      if MsgBox('Remove your pieFX settings as well?' + #13#10#13#10 +
                Dir + '\settings.json' + #13#10#13#10 +
                'Choose No to keep your slot bindings for a future install.',
                mbConfirmation, MB_YESNO) = IDYES then
        DeleteFile(Dir + '\settings.json');
    RemoveDir(Dir);   { only succeeds if it ended up empty }
  end;
end;
