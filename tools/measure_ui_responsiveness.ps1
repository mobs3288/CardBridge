<#
.SYNOPSIS
  Measures how responsive the viewer's UI thread stays while a card / outfit loads.

  Starts a private, off-screen instance of cardbridge_viewer.exe, polls its message
  queue with SendMessageTimeout(SMTO_ABORTIFHUNG) while loading, and reports the
  longest interval in which the UI thread did not answer. A Windows "Not Responding"
  window is exactly a window that stops answering this kind of message.

  Usage:
    powershell -ExecutionPolicy Bypass -File tools/measure_ui_responsiveness.ps1 `
      -Card <card.png> [-Clothes <outfit.png>] [-Outfit N] [-Seconds 120]
#>
param(
    [Parameter(Mandatory = $true)][string]$Card,
    [string]$Clothes = '',
    [int]$Outfit = -1,
    [string]$Exe = '',
    [string]$Library = '',
    [string]$Mods = '',
    [int]$Seconds = 90,
    [string]$Tag = 'ui'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $MyInvocation.MyCommand.Path -Parent) -Parent
if (-not $Exe) { $Exe = Join-Path $repo 'build/bin/cardbridge_viewer.exe' }
if (-not (Test-Path $Exe)) { throw "viewer not built: $Exe" }
if (-not (Test-Path $Card)) { throw "card not found: $Card" }
if (-not $Library) { $Library = Join-Path $repo 'library_full' }
if (-not $Mods) { $Mods = 'D:/Project K/cardbridge/_missing_mods_for_measure' }
$outDir = Join-Path $repo "out/$Tag"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class U {
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
}
'@

$env:CARDBRIDGE_TEST_HOOKS = '1'
$env:CARDBRIDGE_TIMING = '1'
$env:CARDBRIDGE_TIMING_JSON = (Join-Path $outDir 'timing.json')
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = (Resolve-Path $Exe).Path
$args = @('--library', $Library, '--mods', $Mods)
if ($Outfit -ge 0) { $args += @('--outfit', $Outfit) }
if ($Clothes) { $args += @('--clothes', $Clothes) }
$args += $Card
$psi.Arguments = ($args | ForEach-Object { '"' + $_ + '"' }) -join ' '
$psi.UseShellExecute = $false
$proc = [System.Diagnostics.Process]::Start($psi)
$hwnd = [IntPtr]::Zero

try {
    $t0 = [Diagnostics.Stopwatch]::StartNew()
    while ($t0.Elapsed.TotalSeconds -lt 20 -and $hwnd -eq [IntPtr]::Zero) {
        $proc.Refresh()
        if ($proc.HasExited) { throw "viewer exited at startup (code $($proc.ExitCode))" }
        $hwnd = $proc.MainWindowHandle
        Start-Sleep -Milliseconds 100
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw 'no viewer window' }
    $title = [Text.StringBuilder]::new(512)
    [void][U]::GetWindowTextW($hwnd, $title, 512)
    $startTitle = $title.ToString()

    $pollMs = 100
    $timeoutMs = 200
    $timeouts = 0
    $longestBlockMs = 0
    $samples = 0
    $loadedAt = -1.0
    $firstTimeoutAt = -1.0
    $timeoutTimes = @()
    while ($t0.Elapsed.TotalSeconds -lt $Seconds) {
        $r = [IntPtr]::Zero
        $ok = [U]::SendMessageTimeout($hwnd, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero, 0x0002, $timeoutMs, [ref]$r)
        ++$samples
        if ($ok -eq [IntPtr]::Zero) {
            ++$timeouts
            if ($firstTimeoutAt -lt 0) { $firstTimeoutAt = $t0.Elapsed.TotalMilliseconds }
            $timeoutTimes += [Math]::Round($t0.Elapsed.TotalMilliseconds, 0)
            $longestBlockMs = [Math]::Max($longestBlockMs, $timeoutMs)
        }
        [void]$title.Clear()
        [void][U]::GetWindowTextW($hwnd, $title, 512)
        if ($title.ToString() -ne $startTitle -and $loadedAt -lt 0) { $loadedAt = $t0.Elapsed.TotalMilliseconds }
        if ($loadedAt -ge 0 -and $t0.Elapsed.TotalMilliseconds -gt $loadedAt + 1500) { break }
        Start-Sleep -Milliseconds $pollMs
    }
    $result = [ordered]@{
        card = $Card
        clothes = $Clothes
        loadedMs = [Math]::Round($loadedAt, 1)
        pollSamples = $samples
        unansweredPolls = $timeouts
        firstUnansweredMs = [Math]::Round($firstTimeoutAt, 1)
        unansweredAtMs = $timeoutTimes
        maxUnansweredMs = $longestBlockMs
        elapsedMs = [Math]::Round($t0.Elapsed.TotalMilliseconds, 1)
        startTitle = $startTitle
        finalTitle = $title.ToString()
    }
    $result | ConvertTo-Json | Tee-Object -FilePath (Join-Path $outDir 'responsiveness.json')
}
finally {
    if ($hwnd -ne [IntPtr]::Zero -and [U]::IsWindow($hwnd)) { [void][U]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    if (-not $proc.WaitForExit(5000)) { $proc.Kill(); $proc.WaitForExit(5000) }
}
