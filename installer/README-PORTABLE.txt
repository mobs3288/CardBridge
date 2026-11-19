CardBridge - Portable edition
=============================

This folder is everything CardBridge needs to run. No installation, no required DLLs, nothing
outside this folder is added to your system by running it.

To run:
  1. Keep the whole folder together. CardBridge.exe is the application; CardBridge-CLI.exe is the
     command line converter.
  2. Double-click CardBridge.exe (you can also drop a Koikatsu character card .png onto the window).
  3. The first time, CardBridge asks for your Koikatsu folder and copies the base-game data it needs
     into %LOCALAPPDATA%\CardBridge\library (one time, about 0.9 GB, for the full library; about
     5 MB if you choose the minimal library). After that, the game is never needed again.
  4. It also asks for your mods folder, or you can skip it and use base-game items only.
  5. Press E (or the Export PMX button) to export a PMX.

If you prefer a proper Windows application with Start Menu shortcuts, an uninstaller and Add/Remove
Programs integration, use CardBridge-Setup-{{VERSION}}.exe instead. The installer and this portable package
are built from exactly the same binaries — both distributions have the same features.

User data (kept for both editions)
-----------------------------------
CardBridge stores everything it needs between runs in %LOCALAPPDATA%\CardBridge:
  - library          the copied base-game data (your library, yours alone)
  - viewer.json      your settings, library/mods folders, export defaults
  - modindex.json    the cached mod index
  - logs             the viewer log
Exports go wherever you point the export dialog at (or where you chose in the Setup tab).

Nothing user-specific is written into this folder, so you can delete this whole folder at any time and
your settings and exports stay untouched.

CardBridge works fully offline. Game files and mods are never uploaded anywhere and never stay inside
the executables. Koikatsu (or Koikatsu Party) is only needed once to build the library.

MIT License - see LICENSE.txt. CardBridge: https://github.com/mobs3288/CardBridge
