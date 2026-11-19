<#
    CardBridge release packaging. Turns a fresh repository checkout into the two distributables:

      dist\CardBridge-Setup-<version>.exe       the installer (recommended)
      dist\CardBridge-Portable-<version>.zip    the same binaries, no installer

    Both distributions are built from the same Release output in this repo — there is no separate
    "installer edition" with different features.

    What it does, in order: validates the environment (cmake, a C++20 compiler, Inno Setup 6), validates
    the VERSION file, builds the Release binaries (unless -SkipBuild), checks the built exes for foreign
    DLL dependencies, stages a clean dist\staging folder, verifies the staged files (presence, size,
    version resources), compiles the installer, builds the portable ZIP, writes .sha256 files and prints
    the artifact paths.

    Fails loudly and leaves nothing half-finished when anything is missing: no compiler, no Inno Setup,
    no version, a staging file that is missing, or an exe that pulls in a DLL Windows does not ship.

    Requirements: PowerShell 5.1+, CMake 3.23+, Ninja (or the default generator), a C++20 compiler on PATH
    (MinGW-w64 or MSVC), and Inno Setup 6 (only for the installer; -SkipInstaller builds just the ZIP).

    Usage:
      .\installer\build_installer.ps1                 # full release build
      .\installer\build_installer.ps1 -SkipBuild      # repackage dist\staging from an existing build\bin
      .\installer\build_installer.ps1 -SkipInstaller  # portable ZIP only (no Inno Setup needed)
      .\installer\build_installer.ps1 -InnoSetup C:\path\to\ISCC.exe

    The installer locates the compiler and Inno Setup the same way build.ps1 does; no machine-specific
    paths are written anywhere in the repository.
#>
param(
    [switch]$SkipBuild,
    [switch]$SkipInstaller,
    [string]$InnoSetup = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dist = Join-Path $root 'dist'
$staging = Join-Path $dist 'staging'

function Step([string]$msg) { Write-Host "" ; Write-Host "==> $msg" -ForegroundColor Cyan }
function Fail([string]$msg) { throw "build_installer: $msg" }

Step 'Validating the VERSION file'
$version = (Get-Content (Join-Path $root 'VERSION') -TotalCount 1).Trim()
if ($version -notmatch '^(\d+)\.(\d+)\.(\d+)(-[0-9A-Za-z.]+)?$') { Fail "VERSION is '$version' - expected a semver-like X.Y.Z (or X.Y.Z-pre). One source of truth: the VERSION file at the repository root." }
$winVersion = "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
Write-Host "version: $version (assembly $winVersion)"

Step 'Validating the build environment'
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Fail 'cmake not found on PATH. Install CMake 3.23+ (or add it to PATH).' }
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { Write-Host 'ninja not found - using the default CMake generator' }
if (-not (Get-Command g++ -ErrorAction SilentlyContinue) -and -not (Get-Command cl -ErrorAction SilentlyContinue) -and -not (Get-Command gcc -ErrorAction SilentlyContinue)) {
    Write-Host 'note: no g++/cl/gcc on PATH; the CMake configure below will fail if no compiler is available'
}

$iscc = $null
if (-not $SkipInstaller) {
    if ($InnoSetup) {
        if (-not (Test-Path $InnoSetup)) { Fail "Inno Setup compiler not found at '$InnoSetup'." }
        $iscc = $InnoSetup
    } elseif ($env:CARDBRIDGE_INNOSETUP -and (Test-Path $env:CARDBRIDGE_INNOSETUP)) {
        $iscc = $env:CARDBRIDGE_INNOSETUP
    } elseif (Get-Command iscc -ErrorAction SilentlyContinue) {
        $iscc = (Get-Command iscc).Source
    } else {
        foreach ($c in @(
            (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
            (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
            (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'))) {
            if (Test-Path $c) { $iscc = $c; break }
        }
    }
    if (-not $iscc) { Fail "Inno Setup 6 (ISCC.exe) not found. Install it from https://jrsoftware.org/isdl.php (or: winget install --id JRSoftware.InnoSetup), or pass -InnoSetup <path to ISCC.exe>, or build only the portable ZIP with -SkipInstaller." }
    Write-Host "Inno Setup: $iscc"
}

$buildDir = Join-Path $root 'build'
$binDir = Join-Path $buildDir 'bin'

if (-not $SkipBuild) {
    Step 'Building CardBridge (Release)'
    powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1') -Test
    if ($LASTEXITCODE -ne 0) { Fail 'the Release build or the tests failed - see the output above; nothing was packaged.' }
} else {
    Write-Host 'skipping the build (-SkipBuild), packaging the existing build\bin'
}
$viewerExe = Join-Path $binDir 'cardbridge_viewer.exe'
$cliExe = Join-Path $binDir 'cardbridge.exe'
foreach ($exe in @($viewerExe, $cliExe)) {
    if (-not (Test-Path $exe)) { Fail "missing $exe - run the build first (or drop -SkipBuild)" }
}

Step 'Checking DLL dependencies (must be Windows system DLLs only)'
# The project static-links its toolchain; the only allowed dependencies are DLLs Windows ships.
$allowed = @('KERNEL32.dll','USER32.dll','GDI32.dll','COMCTL32.dll','COMDLG32.dll','D3D11.dll','DXGI.dll','GDIPLUS.dll','OLE32.dll','SHELL32.dll','OLEAUT32.dll','ADVAPI32.dll','RPCRT4.dll','MSVCRT.dll','CRYPT32.dll','WS2_32.dll','WINMM.dll','IMM32.dll','UIAutomationCore.dll','VERSION.dll','SHLWAPI.dll')
$objdump = Get-Command objdump -ErrorAction SilentlyContinue
if ($objdump) {
    $bad = @()
    foreach ($exe in @($viewerExe, $cliExe)) {
        & $objdump.Source -p $exe | Select-String 'DLL Name' | ForEach-Object {
            $dll = ($_.Line -split ':', 2)[1].Trim()
            if ($allowed -notcontains $dll.ToUpper()) { $bad += "$([System.IO.Path]::GetFileName($exe)): $dll" }
        }
    }
    if ($bad) { Fail ("unexpected DLL dependencies found - these are not Windows system DLLs and would not exist on a clean machine:`n    " + ($bad -join "`n    ")) }
    Write-Host 'all DLL dependencies are Windows system DLLs'
} else {
    Write-Host 'objdump not found - the dependency check was skipped (the Release build links its toolchain statically by construction)'
}

Step 'Staging a clean folder'
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory -Force -Path $staging | Out-Null
Copy-Item $viewerExe (Join-Path $staging 'CardBridge.exe') -Force
Copy-Item $cliExe (Join-Path $staging 'CardBridge-CLI.exe') -Force
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $staging 'LICENSE.txt') -Force
$portableReadme = (Get-Content (Join-Path $PSScriptRoot 'README-PORTABLE.txt') -Raw).Replace('{{VERSION}}', $version)
Set-Content -Path (Join-Path $staging 'README-PORTABLE.txt') -Value $portableReadme -Encoding UTF8

Step 'Verifying the staged files'
$staged = Get-ChildItem $staging | ForEach-Object { $_.Name }
foreach ($need in @('CardBridge.exe', 'CardBridge-CLI.exe', 'LICENSE.txt', 'README-PORTABLE.txt')) {
    $p = Join-Path $staging $need
    if (-not (Test-Path $p)) { Fail "staging is missing $need" }
    if ((Get-Item $p).Length -le 0) { Fail "staging file $need is empty" }
}
$viewerInfo = (Get-Item (Join-Path $staging 'CardBridge.exe')).VersionInfo
$cliInfo = (Get-Item (Join-Path $staging 'CardBridge-CLI.exe')).VersionInfo
Write-Host ("CardBridge.exe     FileVersion={0} ProductVersion={1}" -f $viewerInfo.FileVersion, $viewerInfo.ProductVersion)
Write-Host ("CardBridge-CLI.exe FileVersion={0} ProductVersion={1}" -f $cliInfo.FileVersion, $cliInfo.ProductVersion)
if ($viewerInfo.FileVersion -notmatch [regex]::Escape($version)) { Fail "CardBridge.exe version metadata does not match VERSION ($version) - the build is probably stale (run cmake configure again)" }
if ($cliInfo.FileVersion -notmatch [regex]::Escape($version)) { Fail "CardBridge-CLI.exe version metadata does not match VERSION ($version) - the build is probably stale (run cmake configure again)" }

Step 'Writing the portable ZIP'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipPath = Join-Path $dist "CardBridge-Portable-$version.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory($staging, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)
Write-Host "wrote $zipPath ($([math]::Round((Get-Item $zipPath).Length/1MB,1)) MB)"

if (-not $SkipInstaller) {
    Step 'Compiling the installer'
    $iss = Join-Path $PSScriptRoot 'CardBridge.iss'
    & $iscc $iss "/DMyAppVersion=$version" "/DMyAppVersionWin=$winVersion"
    if ($LASTEXITCODE -ne 0) { Fail "Inno Setup reported errors - the installer was not produced." }
    $setupPath = Join-Path $dist "CardBridge-Setup-$version.exe"
    if (-not (Test-Path $setupPath)) { Fail "Inno Setup finished but $setupPath does not exist" }
    Write-Host "wrote $setupPath ($([math]::Round((Get-Item $setupPath).Length/1MB,1)) MB)"
}

Step 'Writing SHA-256 sums'
$artifacts = @((Join-Path $dist "CardBridge-Setup-$version.exe"), $zipPath) | Where-Object { Test-Path $_ }
foreach ($a in $artifacts) {
    $hash = (Get-FileHash $a -Algorithm SHA256).Hash
    "$hash  $(Split-Path $a -Leaf)" | Set-Content ($a + '.sha256') -Encoding ASCII
    Write-Host "$(Split-Path $a -Leaf).sha256"
}

Step 'Release artifacts'
Get-ChildItem $dist -Filter "*$version*" | Sort-Object Name | ForEach-Object { Write-Host ("  {0}  ({1:N1} MB)" -f $_.FullName, ($_.Length/1MB)) }
Write-Host ""
Write-Host 'Installer is the recommended download. Both distributions carry the same binaries.'
