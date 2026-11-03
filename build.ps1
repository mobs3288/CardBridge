<#
    Builds CardBridge on its own (no Project K needed): cardbridge.exe, cardbridge_viewer.exe and cardbridge_tests.exe into build\bin.

    Needs on PATH: CMake 3.23 or newer, a C++20 compiler for Windows (MinGW-w64 g++ or MSVC) and, for the MinGW route, Ninja. The two
    header-only dependencies (nlohmann/json, stb) are downloaded by CMake the first time.

    -Test       run cardbridge_tests.exe after building (unit tests; the integration tests run when the CARDBRIDGE_TEST_* variables are set)
    -Debug      Debug instead of Release

    The build runs at idle priority.
#>
param([switch]$Test, [switch]$Debug)

$ErrorActionPreference = 'Stop'
[System.Diagnostics.Process]::GetCurrentProcess().PriorityClass = [System.Diagnostics.ProcessPriorityClass]::Idle
$src = $PSScriptRoot
$build = Join-Path $src 'build'
$type = if ($Debug) { 'Debug' } else { 'Release' }

$gen = @()
if (Get-Command ninja -ErrorAction SilentlyContinue) { $gen = @('-G', 'Ninja') }
cmake -S $src -B $build @gen "-DCMAKE_BUILD_TYPE=$type"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
cmake --build $build --config $type
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Write-Host "Built: $build\bin"

if ($Test) {
    & (Join-Path $build 'bin\cardbridge_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'tests failed' }
}
