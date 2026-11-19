; CardBridge installer (Inno Setup 6).
;
; The application itself lives outside this file (src/, viewer/). This script only decides how it is
; installed: where it goes, what shortcuts it makes, what it does about upgrades, and what is left behind.
;
; Default: a per-machine install into Program Files\CardBridge (needs administrator rights, as any
; Program Files install does). Non-administrators get offered a per-user install into
; %LOCALAPPDATA%\Programs\CardBridge instead (the wizard asks; or pass /CURRENTUSER — for example on
; a locked-down or shared machine). Same binaries either way, Start Menu integration and Add/Remove
; Programs in both cases.
;
; Version number is not written here. The build script (build_installer.ps1) reads VERSION at the repository
; root and passes it in as /DMyAppVersion=<version> and /DMyAppVersionWin=<a.b.c.d>, so the repository has
; one source of truth for the version.
;
; Application files go to {app} (Program Files\CardBridge by default) and are removed on uninstall.
; User data NEVER goes to {app} and is never removed by the uninstaller: the viewer keeps its settings,
; the asset library, the mod index and its logs in %LOCALAPPDATA%\CardBridge, exports go where the user
; points the export dialog (the Setup tab, "one fixed export folder"), and none of that is touched here.
;
; Upgrades: the AppId below is what Windows and CardBridge use to recognise an existing 0.3.x install.
; A newer installer run over an older one updates the application files in place; {app} files this
; installer no longer ships (the pre-rename names) are removed first so nothing stale accumulates.
; User settings and exports are not part of the install and are kept.

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#ifndef MyAppVersionWin
  #define MyAppVersionWin "0.0.0.0"
#endif

#define MyAppName "CardBridge"
#define MyAppPublisher "Ryan Oktaviandi Susilo Wibowo"
#define MyAppURL "https://github.com/mobs3288/CardBridge"
#define MyAppIssuesURL "https://github.com/mobs3288/CardBridge/issues"
#define MyAppReleasesURL "https://github.com/mobs3288/CardBridge/releases"

[Setup]
AppId={{556C6277-D8A5-43C7-BF1F-C0B3718BEDEB}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppIssuesURL}
AppUpdatesURL={#MyAppReleasesURL}
DefaultDirName={autopf}\CardBridge
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=auto
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\CardBridge.exe
SetupIconFile=..\viewer\res\cardbridge.ico
LicenseFile=..\LICENSE
WizardStyle=modern
WizardImageFile=assets\wizard-large.png
WizardSmallImageFile=assets\wizard-small.png
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.10240
OutputDir=..\dist
OutputBaseFilename=CardBridge-Setup-{#MyAppVersion}
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
CloseApplications=yes
RestartApplications=no
UsePreviousAppDir=yes
UsePreviousGroup=yes
UsePreviousTasks=yes
VersionInfoVersion={#MyAppVersionWin}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} Setup
VersionInfoCopyright=MIT License
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
AppCopyright=Copyright (C) Ryan Oktaviandi Susilo Wibowo. MIT License.

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[InstallDelete]
; pre-installer release naming: the old plain-copy layout put these in {app} on dev machines.
Type: files; Name: "{app}\cardbridge_viewer.exe"
Type: files; Name: "{app}\cardbridge.exe"
Type: files; Name: "{app}\cardbridge_cli.exe"

[Files]
; The installer and the portable ZIP carry the same two executables, built from the same source in the
; same Release configuration — there is no "installer edition" with different features.
Source: "..\dist\staging\CardBridge.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\staging\CardBridge-CLI.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\CardBridge"; Filename: "{app}\CardBridge.exe"; WorkingDir: "{app}"
Name: "{group}\CardBridge Command Line"; Filename: "{app}\CardBridge-CLI.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall CardBridge"; Filename: "{uninstallexe}"
Name: "{autodesktop}\CardBridge"; Filename: "{app}\CardBridge.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\CardBridge.exe"; Description: "Launch CardBridge"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Nothing here. Everything CardBridge writes is either its own install files or the user's data in
; %LOCALAPPDATA%\CardBridge (settings, asset library, mod index, logs) and wherever the user exported.
; The uninstaller removes exactly what this installer put in {app} — never user content.
