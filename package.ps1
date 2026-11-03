<#
    Packs CardBridge as a ready-to-use zip: cardbridge.exe, cardbridge_viewer.exe, the launcher .bat files, README, the shader layers, and
    (with -WithLibrary) the asset library.

    The library holds copies of the game's data files, which belong to their authors. WITHOUT -WithLibrary the zip carries no game data:
    "Set up library.bat" builds the library from the user's own Koikatsu folder on first run. A zip made WITH -WithLibrary is for local
    testing; do not publish it without the owner's explicit decision.

    -WithLibrary <dir>   include this library folder (abdata, colorgrade, library.json) as "library"
    -BinDir <dir>        where the built exes are (default: build\bin here, or ..\build-cardbridge\bin inside the Project K repository)
    -OutDir <dir>        where the zip goes (default: dist here)

    The version comes from the VERSION file next to this script. Build first (build.ps1, or scripts\cardbridge.ps1 in Project K).
#>
param(
    [string]$WithLibrary = '',
    [string]$BinDir = '',
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$cb = $PSScriptRoot
$version = (Get-Content (Join-Path $cb 'VERSION') -TotalCount 1).Trim()
if (-not $BinDir) {
    foreach ($c in (Join-Path $cb 'build\bin'), (Join-Path $cb '..\build-cardbridge\bin')) { if (Test-Path (Join-Path $c 'cardbridge.exe')) { $BinDir = $c; break } }
}
if (-not $BinDir) { throw 'no built exes found: run build.ps1 first (or pass -BinDir)' }
foreach ($exe in 'cardbridge.exe', 'cardbridge_viewer.exe') { if (-not (Test-Path (Join-Path $BinDir $exe))) { throw "missing $exe in $BinDir" } }
if (-not $OutDir) { $OutDir = Join-Path $cb 'dist' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$name = "CardBridge-$version"
$stage = Join-Path $OutDir "stage\$name"
if (Test-Path (Join-Path $OutDir 'stage')) { Remove-Item (Join-Path $OutDir 'stage') -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item (Join-Path $BinDir 'cardbridge.exe') $stage
Copy-Item (Join-Path $BinDir 'cardbridge_viewer.exe') $stage
Copy-Item (Join-Path $cb 'release\*') $stage
Copy-Item (Join-Path $cb 'VERSION') $stage
Copy-Item (Join-Path $cb 'README.md') $stage
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'image') | Out-Null
Copy-Item (Join-Path $cb 'image\CB.png') (Join-Path $stage 'image')
Copy-Item (Join-Path $cb 'layers') $stage -Recurse

$suffix = 'win64'
if ($WithLibrary) {
    if (-not (Test-Path (Join-Path $WithLibrary 'abdata'))) { throw "not a library folder: $WithLibrary" }
    Write-Host "copying the library from $WithLibrary (local test zip: contains game data)"
    Copy-Item $WithLibrary (Join-Path $stage 'library') -Recurse
    $suffix = 'win64-with-library'
}

$zip = Join-Path $OutDir "$name-$suffix.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory((Join-Path $OutDir 'stage'), $zip, [System.IO.Compression.CompressionLevel]::Fastest, $false)
Remove-Item (Join-Path $OutDir 'stage') -Recurse -Force
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
"$hash  $(Split-Path $zip -Leaf)" | Set-Content -Path "$zip.sha256" -Encoding ascii
Write-Host ("{0}  ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
Write-Host "sha256 $hash"
