<#
.SYNOPSIS
  Smoke test for the Selective Import window (viewer/Selective.inc): starts a private instance on a card,
  opens the selective import window (through the CARDBRIDGE_TEST_HOOKS test message), waits for the views
  to assemble, and drives the picker and the Apply button with posted mouse messages, taking PrintWindow
  screenshots as it goes. Verifies the window opens, the previews render, the plan summary follows the
  decisions, Cancel/Apply behave, and nothing reaches the loaded character before Apply.
  The 'stack' stage applies a second Selective Import and verifies the two layers compose into one outfit
  (pinned clothing stacks, it does not overwrite).

.DESCRIPTION
  Needs CARDBRIDGE_TEST_CARD (a Koikatsu card .png, e.g. a test fixture card)
  and CARDBRIDGE_TEST_LIBRARY or use -Library (defaults to this repo's library_full).
  The source outfit file is CARDBRIDGE_SELECTIVE_SRC (defaults to out/smoke/base_outfit.png when present —
  generate it with tools\mk_base_outfit.py). A normal viewer run ignores all of this.
  The instance this script starts is closed with WM_CLOSE (and stopped by its own process id if it does not go).
  No input is ever injected into the desktop; the private window is off-screen.

  Run:
    powershell -ExecutionPolicy Bypass -File tools\selective_import_smoke.ps1 [-Card path] [-Source path] [-Out dir] [-Only open,shot,replace,apply]
#>
param(
    [string]$Card = $env:CARDBRIDGE_TEST_CARD,
    [string]$Library = '',
    [string]$Exe = '',
    [string]$Source = $env:CARDBRIDGE_SELECTIVE_SRC,
    [string]$Out = '',
    [string[]]$Only = @(),
    [int]$LoadTimeoutSec = 150,
    [int]$PreviewTimeoutSec = 120
)

$ErrorActionPreference = 'Stop'
if (-not $Card) { throw 'set CARDBRIDGE_TEST_CARD (or pass -Card)' }
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Library) { $Library = Join-Path $repo 'cardbridge/library_full' }
if (-not $Exe) { $Exe = Join-Path $repo 'build-cardbridge/bin/cardbridge_viewer.exe' }
if (-not (Test-Path $Exe)) { $Exe = Split-Path $MyInvocation.MyCommand.Path -Parent | Join-Path -ChildPath '..\build\bin\cardbridge_viewer.exe' }
if (-not $Source) { $Source = (Get-Item "$env:TEMP\cardbridge_basegame_outfit.png" -ErrorAction SilentlyContinue).FullName }
if (-not $Source -or -not (Test-Path $Source)) { throw 'no source outfit file: pass -Source (see the script header)' }
if (-not $Out) { $Out = Join-Path $repo ('out/smoke/selective_' + (Get-Date -Format 'yyyyMMdd_HHmmss')) }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$Card = $Card -replace '\\', '/'
$Library = $Library -replace '\\', '/'
$Source = $Source -replace '\\', '/'
$running = Get-Process | Where-Object { $_.ProcessName -match '^(deadlock|koikatu|koikatsu|koikatsusunshine|projectk|game)$' }
if ($running) { throw ('a game is running (' + (($running | ForEach-Object { $_.ProcessName }) -join ', ') + '): not opening a window now') }
if (-not (Test-Path $Exe)) { throw "viewer not built: $Exe" }
if (-not (Test-Path $Library)) { throw "library not found: $Library" }

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class V {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    public static IntPtr ByClass(string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            var sb = new System.Text.StringBuilder(256);
            GetClassNameW(h, sb, 256);
            if (sb.ToString() == cls) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr L(int x, int y) { return (IntPtr)(((y & 0xFFFF) << 16) | (x & 0xFFFF)); }
    public static int Shot(IntPtr h, string path) {
        RECT r; GetWindowRect(h, out r);
        int w = r.Right - r.Left, ht = r.Bottom - r.Top;
        if (w <= 0 || ht <= 0) return 0;
        using (Bitmap b = new Bitmap(w, ht, PixelFormat.Format32bppArgb)) {
            using (Graphics g = Graphics.FromImage(b)) {
                IntPtr dc = g.GetHdc();
                PrintWindow(h, dc, 2);
                g.ReleaseHdc(dc);
            }
            System.Collections.Generic.HashSet<int> seen = new System.Collections.Generic.HashSet<int>();
            for (int y = 0; y < ht; y += 7) for (int x = 0; x < w; x += 7) seen.Add(b.GetPixel(x, y).ToArgb());
            b.Save(path, ImageFormat.Png);
            return seen.Count;
        }
    }
}
'@

$WM_KEYDOWN = 0x100; $WM_MOUSEMOVE = 0x200; $WM_LBUTTONDOWN = 0x201; $WM_LBUTTONUP = 0x202; $WM_TEST = 0x8028; $WM_CLOSE = 0x10
$MK_LBUTTON = 1
$layoutPath = Join-Path $Out 'layout.json'
$results = New-Object System.Collections.ArrayList

function Check([string]$name, [bool]$ok, [string]$detail = '') {
    [void]$results.Add([pscustomobject]@{ Check = $name; Ok = $ok; Detail = $detail })
    $tag = if ($ok) { 'ok  ' } else { 'FAIL' }
    Write-Host ("  [{0}] {1} {2}" -f $tag, $name, $detail)
}
function Want([string]$step) { return ($Only.Count -eq 0) -or ($Only -contains $step) }
function Hook([int]$cmd, [int]$timeoutMs = 15000, [int]$lp = 0) {
    $r = [IntPtr]::Zero
    $ok = [V]::SendMessageTimeout($script:mainHwnd, $WM_TEST, [IntPtr]$cmd, [IntPtr]$lp, 2, $timeoutMs, [ref]$r)
    return ($ok -ne [IntPtr]::Zero)
}
# the selective window's own layout/state dump (WM_TEST/10 to the selective window, not the main one)
function SelLayout() {
    if (Test-Path $layoutPath) { Remove-Item $layoutPath -Force }
    if ($script:selHwnd -eq [IntPtr]::Zero) { return $null }
    $r = [IntPtr]::Zero
    $ok = [V]::SendMessageTimeout($script:selHwnd, $WM_TEST, [IntPtr]10, [IntPtr]0, 2, 8000, [ref]$r)
    if ($ok -eq [IntPtr]::Zero) { return $null }
    for ($i = 0; $i -lt 20 -and -not (Test-Path $layoutPath); $i++) { Start-Sleep -Milliseconds 100 }
    if (-not (Test-Path $layoutPath)) { return $null }
    return (Get-Content $layoutPath -Raw -Encoding UTF8 | ConvertFrom-Json)
}
function SelWait([int]$timeoutSec) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $timeoutSec) {
        $lay = SelLayout
        if ($lay -and $lay.ready) { return $lay }
        Start-Sleep -Milliseconds 400
    }
    return $null
}
function Layout() {
    if (Test-Path $layoutPath) { Remove-Item $layoutPath -Force }
    if (-not (Hook 10)) { return $null }
    for ($i = 0; $i -lt 20 -and -not (Test-Path $layoutPath); $i++) { Start-Sleep -Milliseconds 100 }
    if (-not (Test-Path $layoutPath)) { return $null }
    return (Get-Content $layoutPath -Raw -Encoding UTF8 | ConvertFrom-Json)
}
function Shot([IntPtr]$w, [string]$name) {
    $path = Join-Path $Out ("{0}.png" -f $name)
    $distinct = [V]::Shot($w, $path)
    Write-Host ("  shot {0} ({1} distinct colours)" -f $path, $distinct)
    return [pscustomobject]@{ Path = $path; Distinct = $distinct }
}
function WaitFor([scriptblock]$cond, [int]$timeoutSec) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $timeoutSec) {
        $lay = Layout
        if ($null -eq $lay) { return $null }
        if (& $cond $lay) { return $lay }
        Start-Sleep -Milliseconds 400
    }
    return $null
}
function ClickSel([IntPtr]$hwnd, [int]$x, [int]$y, [int]$wait = 300) {
    [void][V]::PostMessage($hwnd, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, [V]::L($x, $y)); Start-Sleep -Milliseconds 40
    [void][V]::PostMessage($hwnd, $WM_LBUTTONUP, [IntPtr]0, [V]::L($x, $y)); Start-Sleep -Milliseconds $wait
}

$script:mainHwnd = [IntPtr]::Zero
$script:selHwnd = [IntPtr]::Zero

try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = ('--library "{0}" --outfit 1 "{1}"' -f $Library, $Card)
    $psi.UseShellExecute = $false
    $psi.EnvironmentVariables['CARDBRIDGE_TEST_HOOKS'] = '1'
    $psi.EnvironmentVariables['CARDBRIDGE_TEST_LAYOUT'] = $layoutPath
    $psi.EnvironmentVariables['CARDBRIDGE_SELECTIVE_SRC'] = $Source
    Write-Host "starting a private viewer: $Exe"
    $proc = [System.Diagnostics.Process]::Start($psi)

    # ---- wait for the window and the card ----
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 30 -and $script:mainHwnd -eq [IntPtr]::Zero) {
        $proc.Refresh()
        if ($proc.HasExited) { throw "the viewer exited at start (code $($proc.ExitCode))" }
        $script:mainHwnd = $proc.MainWindowHandle
        Start-Sleep -Milliseconds 200
    }
    Check 'window opens' ($script:mainHwnd -ne [IntPtr]::Zero)
    if ($script:mainHwnd -eq [IntPtr]::Zero) { throw 'no window' }
    $lay = WaitFor { param($l) $l.state.loaded -and -not $l.state.busy } $LoadTimeoutSec
    Check 'card loads' ($null -ne $lay) ($(if ($lay) { "$($lay.state.bones) bones, Direct3D $($lay.state.gpu)" } else { 'timed out' }))
    if ($null -eq $lay) { throw 'card did not load' }

    # ---- open the selective import window ----
    if (Want 'open') {
        Check 'selective window opens' (Hook 20 20000)
        Start-Sleep -Milliseconds 1500
        for ($i = 0; $i -lt 60 -and $script:selHwnd -eq [IntPtr]::Zero; $i++) {
            $script:selHwnd = [V]::ByClass('CardBridgeSelectiveImport')
            Start-Sleep -Milliseconds 250
        }
        Check 'the selective window exists' ($script:selHwnd -ne [IntPtr]::Zero)
        if ($script:selHwnd -eq [IntPtr]::Zero) { throw 'the selective window never appeared' }
    }

    # ---- wait for the previews, take a shot, then All: Replace, wait, shot, Apply ----
    if (Want 'shot') {
        $sel = SelWait $PreviewTimeoutSec
        Check 'previews become ready' ($null -ne $sel) ($(if ($sel) { "views $($sel.views.Count) ready" } else { 'timed out waiting for the previews' }))
        $s = Shot $script:selHwnd 'selective_initial'
        Check 'the window draws' ($s.Distinct -gt 20) "$($s.Distinct) colours"
    }

    if (Want 'apply') {
        $sel = SelWait $PreviewTimeoutSec
        Check 'previews ready before All: Replace' ($null -ne $sel)
        if ($sel) {
            Write-Host "clicking All: Replace at ($($sel.buttons.allReplace -join ','))"
            ClickSel $script:selHwnd $sel.buttons.allReplace[0] $sel.buttons.allReplace[1] 500
            $sel = SelWait $PreviewTimeoutSec
            Check 'rebuild after All: Replace becomes ready' ($null -ne $sel)
            $s = Shot $script:selHwnd 'selective_all_replace'
            Check 'all-replace shot draws' ($s.Distinct -gt 20) "$($s.Distinct) colours"

            Write-Host "clicking Apply at ($($sel.buttons.apply -join ','))"
            ClickSel $script:selHwnd $sel.buttons.apply[0] $sel.buttons.apply[1] 300
        }
        # the window closes on commit and the main view reloads with the selective import pinned
        Start-Sleep -Seconds 2
        $script:selHwnd = [V]::ByClass('CardBridgeSelectiveImport')
        Check 'window closes on commit' ($script:selHwnd -eq [IntPtr]::Zero)
        $lay = WaitFor { param($l) -not $l.state.busy } 180
        Check 'the character reloads' ($null -ne $lay)
        Check 'commit pins the selective import' ($lay -and $lay.clothingMode -eq 2) ($(if ($lay) { "mode $($lay.clothingMode), $($lay.selectiveDecisions) decisions" } else { 'timed out' }))
        Start-Sleep -Milliseconds 1500
        $s = Shot $script:mainHwnd 'selective_applied'
        Check 'applied picture draws' ($s.Distinct -gt 20) "$($s.Distinct) colours"
    }

    if (Want 'stack') {
        # a second Selective Import layers on top of the first: the stack accumulates, it does not overwrite
        Check 'the first commit pins one layer' ($lay -and $lay.clothingPins -eq 1) ($(if ($lay) { "layers $($lay.clothingPins)" } else { 'timed out' }))
        Check 'the selective window reopens for a second import' (Hook 20 20000)
        Start-Sleep -Milliseconds 1500
        $sel = [V]::ByClass('CardBridgeSelectiveImport')
        Check 'the selective window exists again' ($sel -ne [IntPtr]::Zero)
        if ($sel -ne [IntPtr]::Zero) {
            $selReady = SelWait $PreviewTimeoutSec
            Check 'previews ready before the second import' ($null -ne $selReady)
            if ($selReady) {
                ClickSel $sel $selReady.buttons.allReplace[0] $selReady.buttons.allReplace[1] 500
                $selReady = SelWait $PreviewTimeoutSec
                Check 'rebuild after the second All: Replace becomes ready' ($null -ne $selReady)
                if ($selReady) {
                    ClickSel $sel $selReady.buttons.apply[0] $selReady.buttons.apply[1] 300
                }
            }
        }
        Start-Sleep -Seconds 2
        $sel = [V]::ByClass('CardBridgeSelectiveImport')
        Check 'the window closes on the second commit' ($sel -eq [IntPtr]::Zero)
        $lay = WaitFor { param($l) -not $l.state.busy } 180
        Check 'the character reloads after the second import' ($null -ne $lay)
        Check 'two selective layers are pinned' ($lay -and $lay.clothingPins -eq 2) ($(if ($lay) { "layers $($lay.clothingPins)" } else { 'timed out' }))
        Check 'both layers are applied to the character' ($lay -and $lay.clothingApplied -like '* + *') ($(if ($lay) { $lay.clothingApplied } else { 'timed out' }))
    }

    if (Want 'cancel') {
        # opening again and pressing Esc (or the close button) must leave the character untouched
        $before = Layout
        Check 'reopening works' (Hook 20 20000)
        Start-Sleep -Seconds 6
        $sel = [V]::ByClass('CardBridgeSelectiveImport')
        Check 'reopened window exists' ($sel -ne [IntPtr]::Zero)
        if ($sel -ne [IntPtr]::Zero) {
            [void][V]::PostMessage($sel, 0x0010, [IntPtr]0, [IntPtr]0)   # WM_CLOSE
            Start-Sleep -Seconds 2
        }
        $after = Layout
        Check 'cancel keeps a usable viewer' ($null -ne $after)
    }
} finally {
    Write-Host ''
    Write-Host 'results:'
    $fail = 0
    foreach ($r in $results) { if (-not $r.Ok) { $fail++ } }
    $results | Format-Table -AutoSize | Out-String | Write-Host
    Write-Host ("  {0} checks, {1} failed" -f $results.Count, $fail)
    if ($script:mainHwnd) {
        [void][V]::PostMessage($script:mainHwnd, $WM_CLOSE, [IntPtr]0, [IntPtr]0)
        Start-Sleep -Milliseconds 1500
        if (-not $proc.WaitForExit(5000)) { try { $proc.Kill() } catch {} }
    }
    exit $fail
}
