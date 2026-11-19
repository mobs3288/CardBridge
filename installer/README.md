# CardBridge installer

This folder builds and describes the Windows installer for CardBridge. It is deliberately separate
from the application's source — nothing in `src/` or `viewer/` knows about it.

## Layout

```text
installer/
  CardBridge.iss          Inno Setup 6 script — where the app goes, shortcuts, upgrade and uninstall behaviour
  build_installer.ps1     the one command that produces dist\CardBridge-Setup-<v>.exe and dist\CardBridge-Portable-<v>.zip
  assets/                 branding for the wizard (derived from image/CB.png, the CardBridge logo)
    wizard-large.png      side panel, 164x314, shown on the welcome and finished pages
    wizard-small.png      header badge, 55x55, shown on the interior pages
  README-PORTABLE.txt     shipped inside the portable ZIP ({{VERSION}} is replaced at package time)
  README.md               this file
```

Branding is the CardBridge visual identity from the app itself (`docs/UI_DESIGN_SYSTEM.md`): the
logo on its native dark background (`#0A0B10`), one accent (`#609CD6`), Segoe UI. The same icon
(`viewer/res/cardbridge.ico`) is used for the installer, the installed app and the window/taskbar.

## What is installed, what is not

| Where | What | Removed on uninstall? |
|---|---|---|
| Program Files\CardBridge | `CardBridge.exe` (the app), `CardBridge-CLI.exe` (command line), `LICENSE.txt` | yes |
| Start Menu\CardBridge | `CardBridge`, `CardBridge Command Line`, `Uninstall CardBridge` shortcuts | yes |
| Desktop (optional) | `CardBridge` shortcut, only if the user ticks the box | yes |
| `%LOCALAPPDATA%\CardBridge` | settings (`viewer.json`), the asset library, the mod index, logs — created on first run | **never** |
| Wherever the user exports | the PMX, textures, manifest, preview | **never** |
| `{app}` legacy names | `cardbridge_viewer.exe`, `cardbridge.exe`, `cardbridge_cli.exe` are deleted before installing so nothing stale accumulates (see `[InstallDelete]`) | yes (they are app files) |

User data is deliberately out of the installer's reach: the uninstaller removes only what the
installer put into `{app}`. Deleting the folder by hand, or forgetting it, loses nothing personal.

## Upgrading

Inno Setup keys the install off a fixed `AppId` (see `[Setup]` in `CardBridge.iss`), so a newer
installer run over an existing 0.x installation upgrades in place: the application files are
replaced, obsolete names are removed, shortcuts and the previous directory are reused, and the
user's settings in `%LOCALAPPDATA%\CardBridge` are left untouched. Because the app files are
version-stamped from the same `VERSION` file as everything else, `ignoreversion` is used for the
binary copies so the newest build always wins.

## Building the release

One command, from a clean checkout:

```powershell
.\installer\build_installer.ps1
```

It builds the Release binaries with the repository's own `build.ps1` (with the unit tests), checks
that both exes depend only on Windows system DLLs, stages `dist\staging`, verifies it (presence,
non-zero size, `FileVersion` matching the repo's `VERSION`), compiles `CardBridge-Setup-<version>.exe`,
builds `CardBridge-Portable-<version>.zip` from the same staging folder, and writes `.sha256` files
for both. The installer is the recommended download; the ZIP is the identical build without the
installer. Either works from a plain folder.

Useful switches:

```powershell
.\installer\build_installer.ps1 -SkipBuild      # repackage the current build\bin output
.\installer\build_installer.ps1 -SkipInstaller  # portable ZIP only
.\installer\build_installer.ps1 -InnoSetup "C:\Path\to\ISCC.exe"
```

### Tools you need

- PowerShell 5.1+ (or PowerShell 7)
- CMake 3.23+ and a C++20 compiler on `PATH` (MinGW-w64 or MSVC), plus Ninja — the same toolchain
  `build.ps1` uses
- **Inno Setup 6** for the installer only (not needed with `-SkipInstaller`):
  download from <https://jrsoftware.org/isdl.php>, or `winget install --id JRSoftware.InnoSetup`.
  `build_installer.ps1` finds `ISCC.exe` on `PATH`, at the standard install locations, or via the
  `CARDBRIDGE_INNOSETUP` environment variable; failing that it stops with a clear message.
  CardBridge is built with [Inno Setup](https://jrsoftware.org/isinfo.php) by Jordan Russell and
  Martijn Laan — thanks for a great tool.

### The version

Never edited in this folder. `build_installer.ps1` reads the `VERSION` file at the repository root
and passes it to Inno as a `/D` define; the installed app's version, the installer's name, the
`Add/Remove Programs` entry, both `.exe` files' metadata and the ZIP name all derive from it.
Bump the version in `VERSION` (and everything that references it) as usual; see the root README and
`CHANGELOG.md`.

## Testing an installer build

Install into a scratch directory (not your real install) and check it end to end:

```powershell
# silent install into a scratch directory (administrator: default Program Files location)
.\CardBridge-Setup-0.3.0.exe /VERYSILENT /DIR="Program Files\CardBridge Scratch"
# on a machine without administrator rights: per-user install (same wizard choice)
.\CardBridge-Setup-0.3.0.exe /VERYSILENT /CURRENTUSER /DIR="C:\Path\To\Scratch\CardBridge"
# the app must start from that folder without the repository anywhere near it
& "C:\Path\To\Scratch\CardBridge\CardBridge.exe"
# uninstall: the folder, the shortcuts and the Add/Remove Programs entry go away,
# %LOCALAPPDATA%\CardBridge stays
& "C:\Path\To\Scratch\CardBridge\unins000.exe" /VERYSILENT
```

For a GUI check, run the installer normally and step through the wizard: the welcome page carries
the CardBridge artwork, the default location is `Program Files\CardBridge`, a desktop shortcut is
optional, and the last page offers to launch CardBridge.
