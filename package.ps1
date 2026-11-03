<#
    Makes the release files. The release is ONE exe: CardBridge-<version>.exe is the viewer with everything in it (icon, logo, the converter,
    the exporter). On first run it asks for the user's Koikatsu folder and builds its asset library from it (about 17 s); it asks for the
    mods folder; after that it needs nothing. Drop a card on its window, press E to export a PMX.

    Also made: CardBridge-CLI-<version>.exe (the command line converter, for scripts) and a .sha256 beside each.

    The asset library holds copies of the game's data files, which belong to their authors, so it is NOT inside the exe. -LibraryPack <dir>
    additionally zips a prepared library as a separate "library pack" for LOCAL testing; do not publish it without the owner's explicit
    decision.

    -BinDir <dir>        where the built exes are (default: build\bin here, or ..\build-cardbridge-rel\bin or ..\build-cardbridge\bin in Project K)
    -OutDir <dir>        where the files go (default: dist here)
    -LibraryPack <dir>   also zip this library folder (abdata, colorgrade, library.json)

    The version comes from the VERSION file next to this script. Build first (build.ps1, or scripts\cardbridge.ps1 in Project K).
#>
param(
    [string]$BinDir = '',
    [string]$OutDir = '',
    [string]$LibraryPack = ''
)

$ErrorActionPreference = 'Stop'
$cb = $PSScriptRoot
$version = (Get-Content (Join-Path $cb 'VERSION') -TotalCount 1).Trim()
if (-not $BinDir) {
    foreach ($c in 'build\bin', '..\build-cardbridge-rel\bin', '..\build-cardbridge\bin') {
        $p = Join-Path $cb $c
        if (Test-Path (Join-Path $p 'cardbridge_viewer.exe')) { $BinDir = $p; break }
    }
}
if (-not $BinDir) { throw 'no built exes found: run build.ps1 first (or pass -BinDir)' }
foreach ($exe in 'cardbridge.exe', 'cardbridge_viewer.exe') { if (-not (Test-Path (Join-Path $BinDir $exe))) { throw "missing $exe in $BinDir" } }
if (-not $OutDir) { $OutDir = Join-Path $cb 'dist' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Publish([string]$from, [string]$to) {
    Copy-Item $from $to -Force
    $hash = (Get-FileHash $to -Algorithm SHA256).Hash
    "$hash  $(Split-Path $to -Leaf)" | Set-Content -Path "$to.sha256" -Encoding ascii
    Write-Host ("{0}  ({1:N1} MB)  sha256 {2}" -f $to, ((Get-Item $to).Length / 1MB), $hash)
}
Publish (Join-Path $BinDir 'cardbridge_viewer.exe') (Join-Path $OutDir "CardBridge-$version.exe")
Publish (Join-Path $BinDir 'cardbridge.exe') (Join-Path $OutDir "CardBridge-CLI-$version.exe")

if ($LibraryPack) {
    if (-not (Test-Path (Join-Path $LibraryPack 'abdata'))) { throw "not a library folder: $LibraryPack" }
    $zip = Join-Path $OutDir "CardBridge-library-pack-$version.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    # the zip holds a folder named "library": unzip it next to the exe, or into %LOCALAPPDATA%\CardBridge
    $stage = Join-Path $OutDir 'packstage'
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item $LibraryPack (Join-Path $stage 'library') -Recurse
    [System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::Fastest, $false)
    Remove-Item $stage -Recurse -Force
    Write-Host ("{0}  ({1:N1} MB)  LOCAL TEST ONLY: contains game data" -f $zip, ((Get-Item $zip).Length / 1MB))
}
