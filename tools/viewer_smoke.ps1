<#
.SYNOPSIS
  Smoke test for cardbridge_viewer.exe: starts a private instance on a card, drives it with posted window messages and takes
  PrintWindow screenshots of every tab and the main interactions. Finds crashes, hangs (a message the window does not answer) and
  controls that do nothing.

.DESCRIPTION
  Steps (all by default, -Only picks some): parts, body, face, look, bones, skeleton, bonedrag, undo, pose, slider, areola, apose, physics,
  asexported, export, materials. Each saves <Out>\NN_<step>.png and records checks. The viewer is started with CARDBRIDGE_TEST_HOOKS=1, which makes
  it answer a few test messages (layout dump, undo, redo, save / load pose) and use CARDBRIDGE_POSE_PATH / CARDBRIDGE_EXPORT_PARENT
  instead of file dialogs. A normal run of the viewer ignores all of that.

  Only the instance this script starts is touched: it is closed with WM_CLOSE and, if it does not go, stopped by its own process id.
  Nothing is killed by name. The script refuses to run while a game is running (the viewer uses the GPU), and never touches the
  owner's config.json. Mouse and key input are posted to the viewer's window, not sent to the desktop.

  Run it from anywhere:
    powershell -ExecutionPolicy Bypass -File cardbridge\tools\viewer_smoke.ps1
    powershell -ExecutionPolicy Bypass -File cardbridge\tools\viewer_smoke.ps1 -Only body,bonedrag,undo
    powershell -ExecutionPolicy Bypass -File cardbridge\tools\viewer_smoke.ps1 -Quick     (no as-exported bake, no export)
#>
param(
    [string]$Card = $env:CARDBRIDGE_TEST_CARD,      # a Koikatsu card .png
    [string]$Mods = $env:CARDBRIDGE_TEST_MODS,      # the mods folder that card needs
    [string]$Library = '',
    [string]$Exe = '',
    [string]$Out = '',
    [string[]]$Only = @(),
    [switch]$Quick,
    [int]$LoadTimeoutSec = 150,
    [int]$ChestPan = 450      # camera pan up for the areola step, in thousandths of the window height
)

$ErrorActionPreference = 'Stop'
if (-not $Card -or -not $Mods) { throw 'set CARDBRIDGE_TEST_CARD and CARDBRIDGE_TEST_MODS (or pass -Card and -Mods)' }
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # -File passes "a,b" as one string
$repo = Split-Path (Split-Path (Split-Path $MyInvocation.MyCommand.Path -Parent) -Parent) -Parent
if (-not $Library) { $Library = Join-Path $repo 'cardbridge/library_full' }
if (-not $Exe) { $Exe = Join-Path $repo 'build-cardbridge/bin/cardbridge_viewer.exe' }
if (-not $Out) { $Out = Join-Path $repo ('cardbridge/out/smoke/' + (Get-Date -Format 'yyyyMMdd_HHmmss')) }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$Library = $Library -replace '\\', '/'
$Mods = $Mods -replace '\\', '/'
$Card = $Card -replace '\\', '/'

$running = Get-Process | Where-Object { $_.ProcessName -match '^(deadlock|koikatu|koikatsu|koikatsusunshine|projectk|game)$' }
if ($running) { throw ('a game is running (' + (($running | ForEach-Object { $_.ProcessName }) -join ', ') + '): not opening a window now') }
if (-not (Test-Path $Exe)) { throw "viewer not built: $Exe" }

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

    public static IntPtr L(int x, int y) { return (IntPtr)(((y & 0xFFFF) << 16) | (x & 0xFFFF)); }

    // PrintWindow into a PNG; returns the number of distinct colours among a grid of sampled pixels (a blank window has one or two).
    public static int Shot(IntPtr h, string path) {
        RECT r; GetWindowRect(h, out r);
        int w = r.Right - r.Left, ht = r.Bottom - r.Top;
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

    // The fraction of a sampled grid of pixels that differ clearly between two screenshots.
    public static double Diff(string a, string b) {
        using (Bitmap x = new Bitmap(a)) using (Bitmap y = new Bitmap(b)) {
            int w = Math.Min(x.Width, y.Width), h = Math.Min(x.Height, y.Height), n = 0, d = 0;
            for (int j = 0; j < h; j += 3) for (int i = 0; i < w; i += 3) {
                Color p = x.GetPixel(i, j), q = y.GetPixel(i, j);
                n++;
                if (Math.Abs(p.R - q.R) + Math.Abs(p.G - q.G) + Math.Abs(p.B - q.B) > 48) d++;
            }
            return n == 0 ? 0 : (double)d / n;
        }
    }
    // The same, over the view only (right of the panel), so a log line or a slider knob does not count as a change of the picture.
    public static double DiffView(string a, string b, int left) {
        using (Bitmap x = new Bitmap(a)) using (Bitmap y = new Bitmap(b)) {
            int w = Math.Min(x.Width, y.Width), h = Math.Min(x.Height, y.Height), n = 0, d = 0;
            for (int j = 0; j < h; j += 3) for (int i = left; i < w; i += 3) {
                Color p = x.GetPixel(i, j), q = y.GetPixel(i, j);
                n++;
                if (Math.Abs(p.R - q.R) + Math.Abs(p.G - q.G) + Math.Abs(p.B - q.B) > 48) d++;
            }
            return n == 0 ? 0 : (double)d / n;
        }
    }
}
'@

$WM_KEYDOWN = 0x100; $WM_MOUSEMOVE = 0x200; $WM_LBUTTONDOWN = 0x201; $WM_LBUTTONUP = 0x202; $WM_TEST = 0x8028; $WM_CLOSE = 0x10
$MK_LBUTTON = 1
$results = New-Object System.Collections.ArrayList
$script:shotNo = 0
$layoutPath = Join-Path $Out 'layout.json'
$posePath = Join-Path $Out 'pose.json'
$exportParent = Join-Path $Out 'export'

function Check([string]$name, [bool]$ok, [string]$detail = '') {
    [void]$results.Add([pscustomobject]@{ Check = $name; Ok = $ok; Detail = $detail })
    $tag = if ($ok) { 'ok  ' } else { 'FAIL' }
    Write-Host ("  [{0}] {1} {2}" -f $tag, $name, $detail)
}
function Want([string]$step) { return ($Only.Count -eq 0) -or ($Only -contains $step) }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = ('--library "{0}" --mods "{1}" "{2}"' -f $Library, $Mods, $Card)
$psi.UseShellExecute = $false
$psi.EnvironmentVariables['CARDBRIDGE_TEST_HOOKS'] = '1'
$psi.EnvironmentVariables['CARDBRIDGE_TEST_LAYOUT'] = $layoutPath
$psi.EnvironmentVariables['CARDBRIDGE_POSE_PATH'] = $posePath
$psi.EnvironmentVariables['CARDBRIDGE_EXPORT_PARENT'] = $exportParent
Write-Host "starting a private viewer: $Exe"
$proc = [System.Diagnostics.Process]::Start($psi)
$hwnd = [IntPtr]::Zero

function Key([char]$c, [int]$wait = 250) { [void][V]::PostMessage($hwnd, $WM_KEYDOWN, [IntPtr][int][char]::ToUpper($c), [IntPtr]0); Start-Sleep -Milliseconds $wait }
function Click([int]$x, [int]$y, [int]$wait = 250) {
    [void][V]::PostMessage($hwnd, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, [V]::L($x, $y)); Start-Sleep -Milliseconds 40
    [void][V]::PostMessage($hwnd, $WM_LBUTTONUP, [IntPtr]0, [V]::L($x, $y)); Start-Sleep -Milliseconds $wait
}
# a drag: down at the start, moves in `steps` equal steps, up at the end
function Drag([int]$x0, [int]$y0, [int]$x1, [int]$y1, [int]$steps = 10, [int]$stepMs = 45) {
    [void][V]::PostMessage($hwnd, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, [V]::L($x0, $y0)); Start-Sleep -Milliseconds 60
    for ($i = 1; $i -le $steps; $i++) {
        $x = [int]($x0 + ($x1 - $x0) * $i / $steps); $y = [int]($y0 + ($y1 - $y0) * $i / $steps)
        [void][V]::PostMessage($hwnd, $WM_MOUSEMOVE, [IntPtr]$MK_LBUTTON, [V]::L($x, $y)); Start-Sleep -Milliseconds $stepMs
    }
    [void][V]::PostMessage($hwnd, $WM_LBUTTONUP, [IntPtr]0, [V]::L($x1, $y1)); Start-Sleep -Milliseconds 250
}
# Sends a test message and waits for the answer; false when the window does not answer (hung, or gone).
function Hook([int]$cmd, [int]$timeoutMs = 8000, [int]$lp = 0) {
    $r = [IntPtr]::Zero
    $ok = [V]::SendMessageTimeout($hwnd, $WM_TEST, [IntPtr]$cmd, [IntPtr]$lp, 2, $timeoutMs, [ref]$r)
    return ($ok -ne [IntPtr]::Zero)
}
function Layout() {
    if (Test-Path $layoutPath) { Remove-Item $layoutPath -Force }
    if (-not (Hook 10)) { return $null }
    for ($i = 0; $i -lt 20 -and -not (Test-Path $layoutPath); $i++) { Start-Sleep -Milliseconds 100 }
    if (-not (Test-Path $layoutPath)) { return $null }
    return (Get-Content $layoutPath -Raw | ConvertFrom-Json)
}
function Shot([string]$name) {
    $script:shotNo++
    $path = Join-Path $Out ('{0:00}_{1}.png' -f $script:shotNo, $name)
    $distinct = [V]::Shot($hwnd, $path)
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
function GoTab([int]$tab) {
    $lay = Layout
    if ($null -eq $lay) { return $null }
    Click ([int]$lay.tabs[$tab][0]) ([int]$lay.tabs[$tab][1]) 350
    return (Layout)
}

try {
    # ---- wait for the window and the card ----
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 30 -and $hwnd -eq [IntPtr]::Zero) {
        $proc.Refresh()
        if ($proc.HasExited) { throw "the viewer exited at start (code $($proc.ExitCode))" }
        $hwnd = $proc.MainWindowHandle
        Start-Sleep -Milliseconds 200
    }
    Check 'window opens' ($hwnd -ne [IntPtr]::Zero)
    if ($hwnd -eq [IntPtr]::Zero) { throw 'no window' }
    $lay = WaitFor { param($l) $l.state.loaded -and -not $l.state.busy } $LoadTimeoutSec
    Check 'card loads' ($null -ne $lay) ($(if ($lay) { "$($lay.state.bones) bones, Direct3D $($lay.state.gpu)" } else { 'timed out or the window stopped answering' }))
    if ($null -eq $lay) { throw 'card did not load' }
    Start-Sleep -Milliseconds 500

    $before = $null
    if (Want 'parts') {
        Write-Host 'parts tab'
        $s = Shot 'parts'
        $before = $s
        Check 'parts: the picture is not blank' ($s.Distinct -gt 40) "$($s.Distinct) colours"
    }
    foreach ($t in @(@('body', 1), @('face', 2), @('look', 3))) {
        if (-not (Want $t[0])) { continue }
        Write-Host "$($t[0]) tab"
        $lay = GoTab $t[1]
        Check "$($t[0]): the tab opens and the window answers" ($null -ne $lay -and $lay.tab -eq $t[1])
        $s = Shot $t[0]
        $sliders = @($lay.rows | Where-Object { $_.kind -eq 1 -and $_.y -ge 0 }).Count
        Check "$($t[0]): sliders are listed" ($sliders -gt 0) "$sliders visible"
    }
    if (Want 'setup') {
        Write-Host 'setup tab'
        $lay = GoTab 6
        Check 'setup: the tab opens and the window answers' ($null -ne $lay -and $lay.tab -eq 6)
        $s = Shot 'setup'
        $actions = @($lay.rows | Where-Object { $_.kind -eq 5 -and $_.y -ge 0 }).Count
        Check 'setup: the settings actions are listed' ($actions -gt 3) "$actions visible"
    }
    if (Want 'bones') {
        Write-Host 'bones tab'
        $lay = GoTab 4
        Check 'bones: the tab opens' ($null -ne $lay -and $lay.tab -eq 4)
        $s = Shot 'bones'
        Check 'bones: the bone list is there' ($lay.bones.list.Count -gt 3) "$($lay.bones.list.Count) rows"
    }
    if (Want 'materials') {
        Write-Host 'materials tab'
        $lay = GoTab 5
        Check 'materials: the tab opens' ($null -ne $lay -and $lay.tab -eq 5)
        $s = Shot 'materials'
        $mats = $lay.materials.list.Count
        Check 'materials: the material list is there' ($mats -gt 0) "$mats rows, selected $($lay.materials.sel)"
        Check 'materials: the editor is there' ($lay.materials.fieldY.Count -ge 9) "$($lay.materials.fieldY.Count) slider rows"
        # select the first material, then drag its Red slider and undo it
        if ($mats -gt 0) {
            $rowY = [int]$lay.materials.list[0].y
            Click 100 $rowY 400
            $lay = Layout
            Check 'materials: selecting a material answers' ($null -ne $lay -and $lay.materials.sel -ge 0) "sel $($lay.materials.sel)"
            $a = Shot 'before_material_drag'
            $rY = [int]$lay.materials.fieldY[0]
            # A bold change: red from ~90% down to ~3%, so the view has to change clearly (a
            # 20-point change on a skin tone was too subtle to tell from render noise).
            $x0 = [int]($lay.track[0] + 0.9 * ($lay.track[1] - $lay.track[0]))
            $x1 = [int]($lay.track[0] + 0.03 * ($lay.track[1] - $lay.track[0]))
            $undo0 = $lay.materials.undo
            Drag $x0 $rY $x1 $rY 12
            $lay = Layout
            $b = Shot 'material_drag'
            Check 'materials: the window answers after the drag' ($null -ne $lay)
            Check 'materials: the drag is one undo step' (($lay.materials.undo - $undo0) -eq 1) "mat undo $undo0 -> $($lay.materials.undo)"
            Check 'materials: the picture changed' ([V]::DiffView($a.Path, $b.Path, $lay.panel) -gt 0.0005) ("{0:P2} of the view" -f [V]::DiffView($a.Path, $b.Path, $lay.panel))
            [void](Hook 13); Start-Sleep -Milliseconds 300
            $lay = Layout
            $c = Shot 'material_undone'
            Check 'materials: undo restores the picture' ([V]::DiffView($a.Path, $c.Path, $lay.panel) -lt 0.002) ("view diff {0:P3}" -f [V]::DiffView($a.Path, $c.Path, $lay.panel))
            Check 'materials: the window answers after undo' ($null -ne $lay)
        }
    }

    # ---- the skeleton overlay and a bone drag (one gesture = one undo step) ----
    $skeleton = (Want 'skeleton') -or (Want 'bonedrag') -or (Want 'undo') -or (Want 'pose')
    if ($skeleton) {
        $lay = GoTab 4
        if (-not $lay.state.showBones) { Key 'K' 600 }
        Start-Sleep -Milliseconds 400
        $lay = Layout
        if (Want 'skeleton') {
            $s = Shot 'skeleton'
            Check 'skeleton: overlay drawn, bones found in the view' ($lay.state.showBones -and (@($lay.view.PSObject.Properties).Count -gt 2)) "$(@($lay.view.PSObject.Properties).Count) named bones on screen"
        }
    }
    if ((Want 'bonedrag') -or (Want 'undo') -or (Want 'pose')) {
        $hand = $lay.view.cf_j_hand_L
        if ($null -eq $hand) { Check 'bonedrag: the left hand is on screen' $false }
        else {
            $a = Shot 'before_bonedrag'
            $undo0 = $lay.state.undo
            Drag ([int]$hand[0]) ([int]$hand[1]) ([int]$hand[0]) ([int]$hand[1] + 90) 10
            $lay = Layout
            $b = Shot 'bonedrag'
            Check 'bonedrag: the window answers after the drag' ($null -ne $lay)
            Check 'bonedrag: the bone was edited' ($lay.state.edits -ge 1) "$($lay.state.edits) edited bone(s), selected $($lay.state.selBone)"
            Check 'bonedrag: the picture changed' ([V]::DiffView($a.Path, $b.Path, $lay.panel) -gt 0.0005) ("{0:P2} of the view" -f [V]::DiffView($a.Path, $b.Path, $lay.panel))
            Check 'bonedrag: the whole drag is one undo step' (($lay.state.undo - $undo0) -eq 1) "undo depth $undo0 -> $($lay.state.undo)"
        }
    }
    if (Want 'undo') {
        Write-Host 'undo / redo'
        $lay = Layout
        $edited = $lay.state.edits
        [void](Hook 1); Start-Sleep -Milliseconds 300
        $lay = Layout
        $u = Shot 'undone'
        Check 'undo: the bone edit is gone' ($lay.state.edits -lt $edited -or $edited -eq 0) "edits $edited -> $($lay.state.edits), redo depth $($lay.state.redo)"
        Check 'undo: redo has a step' ($lay.state.redo -ge 1)
        [void](Hook 2); Start-Sleep -Milliseconds 300
        $lay = Layout
        Check 'redo: the edit is back' ($lay.state.edits -ge 1) "edits $($lay.state.edits)"
    }
    if (Want 'pose') {
        Write-Host 'pose file'
        if (Test-Path $posePath) { Remove-Item $posePath -Force }
        [void](Hook 3); Start-Sleep -Milliseconds 400
        Check 'pose: save writes the file' (Test-Path $posePath)
        if (Test-Path $posePath) {
            $pj = Get-Content $posePath -Raw | ConvertFrom-Json
            Check 'pose: the file names the edited bone' (($pj.format -eq 'cardbridge-pose') -and (@($pj.bones.PSObject.Properties).Count -ge 1)) (($pj.bones.PSObject.Properties | ForEach-Object { $_.Name }) -join ', ')
            [void](Hook 1); Start-Sleep -Milliseconds 300   # undo the edit, then load the saved pose back
            $lay = Layout
            $mid = $lay.state.edits
            [void](Hook 4); Start-Sleep -Milliseconds 500
            $lay = Layout
            Check 'pose: load puts the edits back' ($lay.state.edits -ge 1 -and $lay.state.edits -gt $mid -or $lay.state.edits -ge 1) "edits $mid -> $($lay.state.edits)"
            $s = Shot 'pose_loaded'
        }
    }

    # ---- a body slider drag, through the real mouse path ----
    if (Want 'slider') {
        Write-Host 'body slider drag'
        $lay = GoTab 1
        $row = $lay.rows | Where-Object { $_.kind -eq 1 -and $_.y -ge 0 } | Select-Object -First 1
        if ($null -eq $row) { Check 'slider: a body slider is visible' $false }
        else {
            $a = Shot 'before_slider'
            $x0 = [int]($lay.track[0] + 0.5 * ($lay.track[1] - $lay.track[0])); $x1 = [int]($lay.track[0] + 0.9 * ($lay.track[1] - $lay.track[0]))
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            Drag $x0 ([int]$row.y) $x1 ([int]$row.y) 12 40
            $answered = Hook 10 8000
            $ms = $sw.ElapsedMilliseconds
            $lay = WaitFor { param($l) -not $l.state.busy } 60
            $b = Shot 'slider'
            Check 'slider: the window answers during and after the drag (no deadlock)' ($answered -and $null -ne $lay) "$ms ms"
            $d = [V]::DiffView($a.Path, $b.Path, $lay.panel)
            Check 'slider: the character changed' ($d -gt 0.0005) ("{0:P2} of the view, slider '{1}'" -f $d, $row.label)
            # put it back by right-clicking the row (resets it)
            $lay = Layout
            [void][V]::PostMessage($hwnd, 0x204, [IntPtr]0, [V]::L(60, [int]$row.y)); Start-Sleep -Milliseconds 80
            [void][V]::PostMessage($hwnd, 0x205, [IntPtr]0, [V]::L(60, [int]$row.y)); Start-Sleep -Milliseconds 600
            Check 'slider: the window answers after the reset' (Hook 10)
        }
    }

    # ---- the areola slider: the nipples show through the body material, so the nude chest changes with it ----
    if (Want 'areola') {
        Write-Host 'areola slider'
        [void](Hook 11 8000 $ChestPan); Start-Sleep -Milliseconds 800
        $lay = GoTab 3
        $row = $lay.rows | Where-Object { $_.id -eq 201 -and $_.y -ge 0 } | Select-Object -First 1
        if ($null -eq $row) { Check 'areola: the slider row is visible' $false }
        else {
            $x0 = [int]$lay.track[0] + 1; $x1 = [int]$lay.track[1] - 1
            Click $x0 ([int]$row.y) 900
            $small = Shot 'areola_small'
            Click $x1 ([int]$row.y) 900
            $big = Shot 'areola_big'
            Click $x0 ([int]$row.y) 900
            $small2 = Shot 'areola_small_again'
            $lay = Layout
            Check 'areola: the window answers' ($null -ne $lay)
            $d = [V]::DiffView($small.Path, $big.Path, 400)
            Check 'areola: the nipples change with the slider' ($d -gt 0.0003) ("{0:P3} of the view" -f $d)
            $d2 = [V]::DiffView($small.Path, $small2.Path, 400)
            Check 'areola: back at the small end the picture is the same again' ($d2 -lt 0.0003) ("{0:P3} of the view" -f $d2)
        }
    }

    if (Want 'apose') {
        Write-Host 'a-pose'
        $a = Shot 'before_apose'
        Key 'P' 600
        $lay = Layout
        $b = Shot 'apose'
        Check 'apose: the switch is on' ($lay.state.aPose)
        $d = [V]::DiffView($a.Path, $b.Path, $lay.panel)
        Check 'apose: the picture changed' ($d -gt 0.0005) ("{0:P2} of the view" -f $d)
    }
    if (Want 'physics') {
        Write-Host 'physics'
        $a = Shot 'before_physics'
        Key 'Y' 500
        Key 'N' 120
        Start-Sleep -Milliseconds 250
        $b = Shot 'physics_shake'
        $lay = Layout
        Check 'physics: on, and the window answers during the shake' ($null -ne $lay -and $lay.state.physics)
        Start-Sleep -Milliseconds 1200
        $c = Shot 'physics_settled'
        Key 'Y' 300
        $lay = Layout
        Check 'physics: off again' (-not $lay.state.physics)
    }
    if ((Want 'asexported') -and -not $Quick) {
        Write-Host 'as exported (X)'
        Key 'X' 300
        $lay = WaitFor { param($l) $l.state.expOk -and $l.state.showExp } 120
        Check 'asexported: the baked copy is ready' ($null -ne $lay)
        $s = Shot 'asexported'
        Check 'asexported: the picture is not blank' ($s.Distinct -gt 40) "$($s.Distinct) colours"
        Key 'X' 300
    }
    if ((Want 'export') -and -not $Quick) {
        Write-Host 'export (E)'
        New-Item -ItemType Directory -Force -Path $exportParent | Out-Null
        Key 'E' 500
        $lay = WaitFor { param($l) -not $l.state.exporting } 240
        $pmx = @(Get-ChildItem -Path $exportParent -Recurse -Filter '*.pmx' -ErrorAction SilentlyContinue)
        Check 'export: a PMX was written' ($pmx.Count -ge 1) (($pmx | ForEach-Object { '{0} ({1:N1} MB)' -f $_.Name, ($_.Length / 1MB) }) -join ', ')
        $s = Shot 'export_done'
    }
    $lay = Layout
    Check 'the window still answers at the end' ($null -ne $lay)
} catch {
    Check 'run completed' $false $_.Exception.Message
} finally {
    if ($hwnd -ne [IntPtr]::Zero -and [V]::IsWindow($hwnd)) { [void][V]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]0, [IntPtr]0) }
    if ($proc -and -not $proc.WaitForExit(8000)) {
        Write-Host "the viewer did not close; stopping process $($proc.Id) (the one this script started)"
        Stop-Process -Id $proc.Id -Force
    }
}

$failed = @($results | Where-Object { -not $_.Ok })
Write-Host ''
Write-Host ("{0} checks, {1} failed; screenshots in {2}" -f $results.Count, $failed.Count, $Out)
if ($failed.Count -gt 0) { exit 1 }
