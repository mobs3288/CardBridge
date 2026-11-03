<#
    Makes the release files. The release is ONE exe: CardBridge-<version>.exe is the viewer with everything in it: the icon, the logo, the
    converter and the exporter. With -Library the base-game library is appended to the exe as a zip with a small footer
    (CardBridge-<version>-with-library.exe). On first run that one unpacks the library into %LOCALAPPDATA%\CardBridge\library (about 20 s), asks for the user's mods folder, and needs nothing else.
    Drop a card on its window, press E to export a PMX. Mods are never inside it: each user points it at their own.

    Without -Library the exe has no game data and asks for the user's Koikatsu folder on first run instead.

    Also made: CardBridge-CLI-<version>.exe (the command line converter) and a .sha256 beside each file.

    -Library <dir>   the library folder to embed (abdata, colorgrade, library.json, cab_index.json); needs the owner's OK to publish
    -BinDir <dir>    where the built exes are (default: build\bin here, or ..\build-cardbridge-rel\bin or ..\build-cardbridge\bin in Project K)
    -OutDir <dir>    where the files go (default: dist here)

    The version comes from the VERSION file next to this script. Build first (build.ps1, or scripts\cardbridge.ps1 in Project K).
#>
param(
    [string]$BinDir = '',
    [string]$OutDir = '',
    [string]$Library = ''
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
$viewer = Join-Path $BinDir 'cardbridge_viewer.exe'
Publish $viewer (Join-Path $OutDir "CardBridge-$version.exe")
Publish (Join-Path $BinDir 'cardbridge.exe') (Join-Path $OutDir "CardBridge-CLI-$version.exe")

if ($Library) {
    if (-not (Test-Path (Join-Path $Library 'abdata'))) { throw "not a library folder: $Library" }
    Write-Host 'zipping the library ...'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $stage = Join-Path $OutDir 'packstage'
    $zip = Join-Path $OutDir 'library.zip.tmp'
    foreach ($p in $stage, $zip) { if (Test-Path $p) { Remove-Item $p -Recurse -Force } }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item $Library (Join-Path $stage 'library') -Recurse
    [System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::Fastest, $false)
    Remove-Item $stage -Recurse -Force
    # exe + zip + footer ("CBLIBPK1", zip offset, zip size)
    $out = Join-Path $OutDir "CardBridge-$version-with-library.exe"
    $exeLen = (Get-Item $viewer).Length
    $zipLen = (Get-Item $zip).Length
    $o = [System.IO.File]::Create($out)
    foreach ($f in $viewer, $zip) {
        $i = [System.IO.File]::OpenRead($f); $i.CopyTo($o); $i.Dispose()
    }
    $footer = [System.Text.Encoding]::ASCII.GetBytes('CBLIBPK1') + [BitConverter]::GetBytes([uint64]$exeLen) + [BitConverter]::GetBytes([uint64]$zipLen)
    $o.Write($footer, 0, $footer.Length)
    $o.Dispose()
    Remove-Item $zip -Force
    $hash = (Get-FileHash $out -Algorithm SHA256).Hash
    "$hash  $(Split-Path $out -Leaf)" | Set-Content -Path "$out.sha256" -Encoding ascii
    Write-Host ("{0}  ({1:N1} MB)  sha256 {2}" -f $out, ((Get-Item $out).Length / 1MB), $hash)
}
