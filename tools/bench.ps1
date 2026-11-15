<#
    CardBridge benchmark: the representative workflows from the performance review, measured end to end.

      powershell -ExecutionPolicy Bypass -File tools\bench.ps1 [-Out out\bench] [-Repeats 3]

    Scenarios (all use the card and library you point it at; no mod files are required to run):
      cold    - full PMX export from scratch (card, assemble, bake, export)
      warm    - the same again (the process is fresh each time; the OS file cache is warm, like normal use)
      outfit  - the same with --outfit N (proxy for switching/equipping clothing in one process)
      no-bake - convert without the shader bake (upper bound of the viewer's E button pipeline)
      second  - a second, different card, for comparison
      viewer  - the in-process reload path (bench.exe): two loads in one process, the way the viewer's outfit switch works

    Requires: build\bin\cardbridge.exe (build.ps1), a library (library_full) and a card.
    Set CARDBRIDGE_TIMING=1 to also print each run's per-stage breakdown.

    The numbers are wall-clock from PowerShell's Stopwatch; repeated runs vary with background load;
    treat anything within ~15% as noise and use -Repeats for a stable median.
#>

param(
    [string]$Out = 'out\bench',
    [string]$Library = 'library_full',
    [string]$Card = 'image\demo_card1.png',
    [string]$SecondCard = 'image\demo_card2.png',
    [int]$Repeats = 3
)

$ErrorActionPreference = 'Continue'
Set-Location (Split-Path -Parent $PSScriptRoot)

$exe = 'build\bin\cardbridge.exe'
if (-not (Test-Path $exe)) { throw "bench: $exe not found - run build.ps1 first" }
if (-not (Test-Path $Library)) { throw "bench: library '$Library' not found" }
if (-not (Test-Path $Card)) { throw "bench: card '$Card' not found" }

function Invoke-Convert {
    param([string]$card, [string]$tag, [string]$extra)
    $times = @()
    for ($i = 0; $i -lt $Repeats; $i++) {
        $dir = Join-Path $Out "$tag-$i"
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
        $extraArgs = @()
        if ($extra) { $extraArgs = $extra -split ' ' | Where-Object { $_ } }
        $args = @('convert', $card, '--library', $Library, '--out', $dir, '--overwrite') + $extraArgs
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $log = & $exe @args 2>&1
        $sw.Stop()
        if ($LASTEXITCODE -ne 0) {
            $log | Out-String | Write-Host
            throw "convert failed for '$card' ($tag, run $i) - exit $LASTEXITCODE"
        }
        $times += $sw.Elapsed.TotalSeconds
    }
    $sorted = $times | Sort-Object
    $median = $sorted[[int](($sorted.Count - 1) / 2)]
    $fmt = ($times | ForEach-Object { '{0:n2}s' -f $_ }) -join ', '
    Write-Host ("{0,-28} median {1,8:n2}s   runs: {2}" -f $tag, $median, $fmt)
    return [pscustomobject]@{ scenario = $tag; median_s = [math]::Round($median, 2); runs = $times }
}

$results = @()
New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Directory | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue   # keep out\bench.exe (the viewer-reload harness)

$results += Invoke-Convert -card $Card -tag 'cold' -extra ''
$results += Invoke-Convert -card $Card -tag 'warm' -extra ''
$results += Invoke-Convert -card $Card -tag 'outfit-0' -extra '--outfit 0'
$results += Invoke-Convert -card $Card -tag 'outfit-2' -extra '--outfit 2'
$results += Invoke-Convert -card $Card -tag 'no-bake' -extra '--no-bake'
if (Test-Path $SecondCard) { $results += Invoke-Convert -card $SecondCard -tag 'second-card' -extra '' }

# The viewer's in-process reload path (assemble only, no export): out\bench.exe is the harness.
if (Test-Path 'out\bench.exe') {
    $times = @()
    for ($i = 0; $i -lt $Repeats; $i++) {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $log = & 'out\bench.exe' $Card $Library '--repeat 2' 2>&1
        $sw.Stop()
        if ($LASTEXITCODE -ne 0) { $log | Out-String | Write-Host; throw "bench harness failed for '$card'" }
        $warm = ($log | Select-String 'iter 1').ToString()
        if ($warm -match 'assemble\s+([\d.]+)s') { $times += [double]$matches[1] } else { Write-Host "viewer warm reload: could not parse '$warm'" }
    }
    if ($times.Count) {
        $sorted = $times | Sort-Object
        $median = $sorted[[int](($sorted.Count - 1) / 2)]
        Write-Host ("{0,-28} median {1,8:n2}s   runs: {2}" -f 'viewer warm reload', $median, (($times | ForEach-Object { '{0:n2}s' -f $_ }) -join ', '))
        $results += [pscustomobject]@{ scenario = 'viewer warm reload'; median_s = [math]::Round($median, 2); runs = $times }
    }
}

$results | ConvertTo-Json | Set-Content -Encoding ascii (Join-Path $Out 'results.json')
Write-Host "results written to $Out\results.json"
