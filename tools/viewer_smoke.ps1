<#
.SYNOPSIS
  Smoke test for cardbridge_viewer.exe: starts a private instance on a card, drives it with posted window messages and takes
  PrintWindow screenshots of every tab and the main interactions. Finds crashes, hangs (a message the window does not answer) and
  controls that do nothing.

.DESCRIPTION
  Steps (all by default, -Only picks some): parts, body, face, look, morphs, setup, bones, skeleton, bonedrag, undo, pose, slider, bwh, areola, apose, physics,
  asexported, export, materials, viewmode, gizmo, axeswidget. Each saves <Out>\NN_<step>.png and records checks. The viewer is started with CARDBRIDGE_TEST_HOOKS=1, which makes
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
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr hAfter, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    // The first visible top-level window of a class. (FindWindowW cannot be used here:
    // PowerShell marshals a $null string argument as "", which matches no window.)
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
    // The same with a configurable sampling stride and per-pixel tolerance sum: deterministic renders make even a small real movement measurable at stride 1.
    public static double DiffViewFine(string a, string b, int left, int sumTolerance, int stride) {
        using (Bitmap x = new Bitmap(a)) using (Bitmap y = new Bitmap(b)) {
            int w = Math.Min(x.Width, y.Width), h = Math.Min(x.Height, y.Height), n = 0, d = 0;
            for (int j = 0; j < h; j += stride) for (int i = left; i < w; i += stride) {
                Color p = x.GetPixel(i, j), q = y.GetPixel(i, j);
                n++;
                if (Math.Abs(p.R - q.R) + Math.Abs(p.G - q.G) + Math.Abs(p.B - q.B) > sumTolerance) d++;
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

# Every message goes to $script:target: the viewer, or the Material Workbench window
# while the materials step drives it (it is a second top-level window of the same
# process and answers the same test messages).
$script:target = [IntPtr]::Zero
function Key([char]$c, [int]$wait = 250) { [void][V]::PostMessage($script:target, $WM_KEYDOWN, [IntPtr][int][char]::ToUpper($c), [IntPtr]0); Start-Sleep -Milliseconds $wait }
function Click([int]$x, [int]$y, [int]$wait = 250) {
    [void][V]::PostMessage($script:target, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, [V]::L($x, $y)); Start-Sleep -Milliseconds 40
    [void][V]::PostMessage($script:target, $WM_LBUTTONUP, [IntPtr]0, [V]::L($x, $y)); Start-Sleep -Milliseconds $wait
}
# a drag: down at the start, moves in `steps` equal steps, up at the end
function Drag([int]$x0, [int]$y0, [int]$x1, [int]$y1, [int]$steps = 10, [int]$stepMs = 45) {
    [void][V]::PostMessage($script:target, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, [V]::L($x0, $y0)); Start-Sleep -Milliseconds 60
    for ($i = 1; $i -le $steps; $i++) {
        $x = [int]($x0 + ($x1 - $x0) * $i / $steps); $y = [int]($y0 + ($y1 - $y0) * $i / $steps)
        [void][V]::PostMessage($script:target, $WM_MOUSEMOVE, [IntPtr]$MK_LBUTTON, [V]::L($x, $y)); Start-Sleep -Milliseconds $stepMs
    }
    [void][V]::PostMessage($script:target, $WM_LBUTTONUP, [IntPtr]0, [V]::L($x1, $y1)); Start-Sleep -Milliseconds 250
}
# Sends a test message and waits for the answer; false when the window does not answer (hung, or gone).
function Hook([int]$cmd, [int]$timeoutMs = 8000, [int]$lp = 0) {
    $r = [IntPtr]::Zero
    $ok = [V]::SendMessageTimeout($script:target, $WM_TEST, [IntPtr]$cmd, [IntPtr]$lp, 2, $timeoutMs, [ref]$r)
    return ($ok -ne [IntPtr]::Zero)
}
function Layout() {
    if (Test-Path $layoutPath) { Remove-Item $layoutPath -Force }
    if (-not (Hook 10)) { return $null }
    for ($i = 0; $i -lt 20 -and -not (Test-Path $layoutPath); $i++) { Start-Sleep -Milliseconds 100 }
    if (-not (Test-Path $layoutPath)) { return $null }
    return (Get-Content $layoutPath -Raw -Encoding UTF8 | ConvertFrom-Json)
}
function Shot([string]$name, [IntPtr]$of = [IntPtr]::Zero) {
    $script:shotNo++
    $path = Join-Path $Out ('{0:00}_{1}.png' -f $script:shotNo, $name)
    $w = if ($of -eq [IntPtr]::Zero) { $script:target } else { $of }
    $distinct = [V]::Shot($w, $path)
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
    # Keep the private window off-screen so it never shows up over the user's game: PrintWindow still captures it
    # (it does not need the window to be visible on a monitor, only not minimized), and no mouse/key input is ever sent to the desktop.
    [void][V]::SetWindowPos($hwnd, [IntPtr]::Zero, -32000, -32000, 0, 0, 0x0001 -bor 0x0004 -bor 0x0010)   # SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
    $script:target = $hwnd   # every step aims at the viewer until the materials step retargets
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
        $lay = GoTab 7
        Check 'setup: the tab opens and the window answers' ($null -ne $lay -and $lay.tab -eq 7)
        $s = Shot 'setup'
        $actions = @($lay.rows | Where-Object { $_.kind -eq 5 -and $_.y -ge 0 }).Count
        Check 'setup: the settings actions are listed' ($actions -gt 3) "$actions visible"
    }
    if (Want 'bones') {
        Write-Host 'bones tab'
        $lay = GoTab 5
        Check 'bones: the tab opens' ($null -ne $lay -and $lay.tab -eq 5)
        $s = Shot 'bones'
        Check 'bones: the bone list is there' ($lay.bones.list.Count -gt 3) "$($lay.bones.list.Count) rows"
    }
    if (Want 'materials') {
        Write-Host 'materials tab'
        $lay = GoTab 6
        Check 'materials: the tab opens' ($null -ne $lay -and $lay.tab -eq 6)
        $s = Shot 'materials'
        $mats = $lay.materials.list.Count
        Check 'materials: the material list is there' ($mats -gt 0) "$mats rows, selected $($lay.materials.sel)"
        Check 'materials: the launcher button is there' ($lay.materials.open.Count -eq 2) "open at $($lay.materials.open -join ',')"
        Check 'materials: the filter button is there' ($lay.materials.filterBtn.Count -eq 2) "filter at $($lay.materials.filterBtn -join ',')"
        # select the first material from the launcher's list (the workbench follows it)
        if ($mats -gt 0) {
            $rowY = [int]$lay.materials.list[0].y
            Click 100 $rowY 400
            $lay = Layout
            Check 'materials: selecting a material answers' ($null -ne $lay -and $lay.materials.sel -ge 0) "sel $($lay.materials.sel)"
        }
        # open the Material Workbench window and drive it from here on
        $ob = $lay.materials.open
        Click ([int]$ob[0]) ([int]$ob[1]) 700
        $wb = [V]::ByClass('CardBridgeMaterialWorkbench')
        Check 'materials: the workbench opens in its own window' ($wb -ne [IntPtr]::Zero)
        if ($wb -ne [IntPtr]::Zero) {
            $script:target = $wb   # every click / hook / shot below aims at the workbench
            $lay = Layout
            Check 'materials: the workbench answers' ($null -ne $lay -and $lay.workbench.open -eq $true)
            Check 'materials: the workbench is as big as the viewer' ($lay.workbench.client[0] -ge 880 -and $lay.workbench.client[1] -ge 520) "$($lay.workbench.client -join 'x')"
            $s2 = Shot 'workbench'
            Check 'materials: the workbench draws' ($s2.Distinct -gt 40) "$($s2.Distinct) colours"
            Check 'materials: the workbench shows the list and the editor' ($lay.workbench.list.Count -gt 0 -and $lay.workbench.fieldY.Count -ge 9) "$($lay.workbench.list.Count) rows, $($lay.workbench.fieldY.Count) slider rows"
            # select a material in the workbench's own list (row 1), then go back to row 0:
            # the view checks below need a material that is actually on screen.
            if ($lay.workbench.list.Count -gt 1) {
                $wrow = [int]$lay.workbench.list[1].y
                $row0y = [int]$lay.workbench.list[0].y
                $idx0 = $lay.workbench.list[0].index
                Click 150 $wrow 400
                $lay = Layout
                Check 'materials: the workbench list selects' ($lay.workbench.sel -eq $lay.workbench.list[1].index) "sel $($lay.workbench.sel)"
                Click 150 $row0y 400
                $lay = Layout
                Check 'materials: the workbench list selects back' ($lay.workbench.sel -eq $idx0) "sel $($lay.workbench.sel)"
            }
            # drag the Red slider (the workbench's own track) and undo it; the view lives
            # in the main window, so those shots go to $hwnd.
            $a = Shot 'before_material_drag' $hwnd
            $rY = [int]$lay.workbench.fieldY[0]
            $tr = $lay.workbench.track
            # A bold change: red from ~90% down to ~3%, so the view has to change clearly (a
            # 20-point change on a skin tone was too subtle to tell from render noise).
            $x0 = [int]($tr[0] + 0.9 * ($tr[1] - $tr[0]))
            $x1 = [int]($tr[0] + 0.03 * ($tr[1] - $tr[0]))
            $undo0 = $lay.workbench.undo
            Drag $x0 $rY $x1 $rY 12
            $lay = Layout
            $b = Shot 'material_drag' $hwnd
            Check 'materials: the window answers after the drag' ($null -ne $lay)
            Check 'materials: the drag is one undo step' (($lay.workbench.undo - $undo0) -eq 1) "mat undo $undo0 -> $($lay.workbench.undo)"
            Check 'materials: the picture changed' ([V]::DiffView($a.Path, $b.Path, $lay.panel) -gt 0.0005) ("{0:P2} of the view" -f [V]::DiffView($a.Path, $b.Path, $lay.panel))
            [void](Hook 13); Start-Sleep -Milliseconds 300
            $lay = Layout
            $c = Shot 'material_undone' $hwnd
            Check 'materials: undo restores the picture' ([V]::DiffView($a.Path, $c.Path, $lay.panel) -lt 0.002) ("view diff {0:P3}" -f [V]::DiffView($a.Path, $c.Path, $lay.panel))
            Check 'materials: the window answers after undo' ($null -ne $lay)
            # Class dropdown: open it, pick Metal (index 5) directly instead of cycling.
            $cb = $lay.workbench.classBtn
            Click ([int]$cb[0]) ([int]$cb[1]) 400
            $lay = Layout
            Check 'materials: the class dropdown opens' ($lay.workbench.classDropOpen -eq $true)
            $rowH = [int]$lay.workbench.rowH
            # dropdown top = classDropTop; Metal is item 5 (Skin 0 .. Metal 5).
            $metalY = [int]$lay.workbench.classDropTop + 5 * $rowH + [int]($rowH / 2)
            Click ([int]$cb[0]) $metalY 400
            $lay = Layout
            Check 'materials: picking Metal in one click sets the class' ($lay.workbench.classDropOpen -eq $false -and $lay.workbench.cls -eq 5) "cls $($lay.workbench.cls)"
            # Colour picker: open it, drag in the SV square to a dark/saturated colour.
            $a2 = Shot 'before_picker' $hwnd
            $undo1 = $lay.workbench.undo
            $pb = $lay.workbench.pickBtn
            Click ([int]$pb[0]) ([int]$pb[1]) 400
            $lay = Layout
            Check 'materials: the colour picker opens' ($lay.workbench.pickerOpen -eq $true)
            $sv = $lay.workbench.pickerSV; $svLo = $lay.workbench.pickerSVLo
            Drag ([int]$sv[0]) ([int]$sv[1]) ([int]$svLo[0]) ([int]$svLo[1]) 10
            $lay = Layout
            $b2 = Shot 'picker_drag' $hwnd
            Check 'materials: the picker drag is one undo step' (($lay.workbench.undo - $undo1) -eq 1) "mat undo $undo1 -> $($lay.workbench.undo)"
            Check 'materials: the picker changes the picture' ([V]::DiffView($a2.Path, $b2.Path, $lay.panel) -gt 0.0005) ("{0:P2} of the view" -f [V]::DiffView($a2.Path, $b2.Path, $lay.panel))
            Click ([int]$pb[0]) ([int]$pb[1]) 400
            $lay = Layout
            Check 'materials: the colour picker closes' ($lay.workbench.pickerOpen -eq $false -and $null -ne $lay)
            # Texture inspector: open it, check the format string is recorded, drag the UV canvas.
            $ib = $lay.workbench.inspectBtn
            Click ([int]$ib[0]) ([int]$ib[1]) 400
            $lay = Layout
            Check 'materials: the texture inspector opens' ($lay.workbench.inspectOpen -eq $true)
            Check 'materials: the texture format is recorded' ($lay.workbench.texFormat -ne $null -and $lay.workbench.texFormat -ne '') "tex $($lay.workbench.texFormat)"
            $s3 = Shot 'inspector'
            Check 'materials: the inspector shows the preview and UV' ($s3.Distinct -gt 40) "$($s3.Distinct) colours"
            $uv = $lay.workbench.uvCanvas
            Drag ([int]$uv[0]) ([int]$uv[1]) ([int]$uv[0] + 30) ([int]$uv[1] + 10) 8
            $lay = Layout
            Check 'materials: dragging the UV canvas answers' ($null -ne $lay -and $lay.workbench.inspectOpen -eq $true)
            Click ([int]$ib[0]) ([int]$ib[1]) 400
            $lay = Layout
            Check 'materials: the texture inspector closes back to shading' ($lay.workbench.inspectOpen -eq $false -and $null -ne $lay)
            # The file row: Save / Load are there (never clicked: no file dialog is wanted).
            Check 'materials: save and load are on the file row' ($lay.workbench.saveBtn.Count -eq 2 -and $lay.workbench.loadBtn.Count -eq 2)
            # Closing the workbench hides it; the launcher opens it again with its state.
            [void][V]::PostMessage($wb, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero); Start-Sleep -Milliseconds 400
            $lay = Layout
            Check 'materials: closing the workbench hides it' ($lay.workbench.open -eq $false)
            $script:target = $hwnd   # the launcher button lives in the viewer's panel
            Click ([int]$ob[0]) ([int]$ob[1]) 700
            $lay = Layout
            Check 'materials: the launcher reopens it' ($lay.workbench.open -eq $true -and $lay.workbench.sel -ge 0) "sel $($lay.workbench.sel)"
            $script:target = $hwnd   # the rest of the run drives the viewer again
        }
    }

    if (Want 'viewmode') {
        Write-Host 'viewmode strip'
        $lay = GoTab 0
        # move the pointer off the panel first, twice, with the queue draining in between: the materials step leaves a hover
        # on a row, and its orange highlight of the selected material's meshes would otherwise be part of the "before" Lit picture
        [void][V]::PostMessage($script:target, 0x200, [IntPtr]0, [V]::L(700, 400)); Start-Sleep -Milliseconds 300
        [void](Layout)
        [void][V]::PostMessage($script:target, 0x200, [IntPtr]0, [V]::L(720, 400)); Start-Sleep -Milliseconds 300
        [void](Layout)
        [void][V]::PostMessage($script:target, 0x200, [IntPtr]0, [V]::L(700, 400)); Start-Sleep -Milliseconds 500
        $lay = Layout
        # clicking the Lit cell re-renders the view with no change of state: without that, the picture is the stale surface from
        # the materials step (its render happened while the Mats tab and the edited material were selected - the orange glow).
        $client = $lay.client
        if ($null -ne $client) {
            $cw = [int]$client[0]; $kPanel = [int]$lay.panel
            $cellW = [int](($cw - $kPanel - 12 - 64 - 8) / 11)
            $stripY = [int]$client[1] - 17
            $litX = $kPanel + 6 + [int]($cellW / 2)
            Click $litX $stripY 800
            $lay = Layout
        }
        $a = $null; $prev = $null
        for ($i = 0; $i -lt 40 -and $null -eq $a; $i++) {
            $try = Join-Path $Out ('xx_viewmode_try_{0}.png' -f $i)
            [void][V]::Shot($script:target, $try)
            Start-Sleep -Milliseconds 250
            if ($null -ne $prev) {
                $d = [V]::DiffView($prev, $try, $lay.panel)
                if ($d -lt 0.0005) { $a = @{ Path = $prev }; Remove-Item $try -Force -ErrorAction SilentlyContinue; break }
                Remove-Item $prev -Force -ErrorAction SilentlyContinue
            }
            $prev = $try
        }
        if ($null -eq $a) { Remove-Item $prev -Force -ErrorAction SilentlyContinue; $a = Shot 'before_viewmode' }
        Write-Host ("  viewmode     : the picture is stable after {0} shot(s)" -f ($i + 1))
        $client = $lay.client
        if ($null -eq $client) { Check 'viewmode: the layout reports the client size' $false; }
        else {
            $cw = [int]$client[0]; $kPanel = [int]$lay.panel
            $cellW = [int](($cw - $kPanel - 12 - 64 - 8) / 11)
            $stripY = [int]$client[1] - 17
            # M-ID (mode 10) cell centre; the Lit cell is at index 0
            $mIdX = $kPanel + 6 + 10 * $cellW + [int]($cellW / 2)
            $litX = $kPanel + 6 + [int]($cellW / 2)
            Click $mIdX $stripY 700
            $lay = Layout
            $b = Shot 'viewmode_materialid'
            Check 'viewmode: switching to MaterialID changes the picture' ([V]::DiffView($a.Path, $b.Path, $lay.panel) -gt 0.002) ("{0:P2} of the view" -f [V]::DiffView($a.Path, $b.Path, $lay.panel))
            Check 'viewmode: the layout answers with the new mode' ($lay.state.diagMode -eq 10) "diagMode $($lay.state.diagMode)"
            $litX = $kPanel + 6 + [int]($cellW / 2)
            Click $litX $stripY 700
            $lay = Layout
            $c = Shot 'viewmode_lit'
            Check 'viewmode: back to Lit restores the picture' ([V]::DiffView($a.Path, $c.Path, $lay.panel) -lt 0.002) ("view diff {0:P3}" -f [V]::DiffView($a.Path, $c.Path, $lay.panel))
            Check 'viewmode: the layout answers with the lit mode' ($lay.state.diagMode -eq 0) "diagMode $($lay.state.diagMode)"
        }
    }

    # ---- the skeleton overlay and a bone drag (one gesture = one undo step) ----
    $skeleton = (Want 'skeleton') -or (Want 'bonedrag') -or (Want 'undo') -or (Want 'pose')
    if ($skeleton) {
        $lay = GoTab 5
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

    # ---- the accessory move gizmo: select an accessory with its chip, drag an axis, reset the move ----
    if (Want 'gizmo') {
        Write-Host 'accessory gizmo'
        $WM_MOUSEWHEEL = 0x020A
        $lay = GoTab 0
        if ($null -eq $lay) { Check 'gizmo: the Parts tab answers' $false } else { Write-Host ("  GoTab 0    : yaw={0:F1} zoom={1:F2} drag={2}" -f $lay.state.camYaw, $lay.state.camZoom, $lay.state.drag) }
        # the accessories sit at the bottom of the list: scroll it there first, and wait until the
        # list's ease has stopped (a click on a stale row position lands on the wrong row)
        # (WM_MOUSEWHEEL carries SCREEN coordinates, and the private window sits off-screen: ask it where)
        $gw = New-Object V+RECT
        [void][V]::GetWindowRect($script:target, [ref]$gw)
        $gx = $gw.Left + 100; $gy = $gw.Top + 300
        $acc = $null
        for ($i = 0; $i -lt 40; $i++) {
            $acc = @($lay.accessories | Where-Object { $_.inView }) | Select-Object -First 1
            if ($null -ne $acc) { break }
            [void][V]::PostMessage($script:target, $WM_MOUSEWHEEL, [IntPtr](-7864320), [V]::L($gx, $gy))
            Start-Sleep -Milliseconds 180
            $lay = Layout
            if ($null -eq $lay) { break }
            Write-Host ("  wheel {0,2}: yaw={1:F1} zoom={2:F2} drag={3} accInView={4}" -f $i, $lay.state.camYaw, $lay.state.camZoom, $lay.state.drag, @($lay.accessories | Where-Object { $_.inView }).Count)
        }
        if ($null -ne $acc) {
            $prevY = [double]((@($lay.accessories | Where-Object { $_.part -eq $acc.part }))[0].y)
            for ($i = 0; $i -lt 20; $i++) {
                Start-Sleep -Milliseconds 250
                $lay = Layout
                if ($null -eq $lay) { break }
                $nowAcc = @($lay.accessories | Where-Object { $_.part -eq $acc.part }) | Select-Object -First 1
                if ($null -eq $nowAcc) { break }
                if ([math]::Abs([double]$nowAcc.y - $prevY) -lt 1.0 -and $nowAcc.inView) { $acc = $nowAcc; break }
                $prevY = [double]$nowAcc.y
            }
            $acc = @($lay.accessories | Where-Object { $_.inView }) | Select-Object -First 1
        }
        if ($null -eq $acc) { Check 'gizmo: an accessory is visible in the parts list (after scrolling)' $false }
        else {
            Write-Host ("  settled     : yaw={0:F1} zoom={1:F2} drag={2} acc={3}" -f $lay.state.camYaw, $lay.state.camZoom, $lay.state.drag, $acc.id)
            Write-Host "  with $($acc.id)"
            $a = Shot 'before_gizmo'
            $l2 = Layout; Write-Host ("  shot a      : yaw={0:F1} zoom={1:F2} drag={2}" -f $l2.state.camYaw, $l2.state.camZoom, $l2.state.drag)
            Click ([int]$acc.chip[0]) ([int]$acc.chip[1]) 600
            $l3 = Layout; Write-Host ("  click chip  : yaw={0:F1} zoom={1:F2} drag={2} selAcc={3}" -f $l3.state.camYaw, $l3.state.camZoom, $l3.state.drag, $l3.state.selAccessory)
            $lay = Layout
            Write-Host ("  cam yaw={0:F1} pitch={1:F1} zoom={2:F2} drag={3} selAcc={4}" -f $lay.state.camYaw, $lay.state.camPitch, $lay.state.camZoom, $lay.state.drag, $lay.state.selAccessory)
            $selRow = $lay.accessories | Where-Object { $_.part -eq $acc.part } | Select-Object -First 1
            Check 'gizmo: the chip selects the accessory' ($null -ne $selRow -and [bool]$selRow.sel)
            Check 'gizmo: the gizmo is drawn' ($null -ne $lay.gizmo)
            if ($null -ne $lay.gizmo) {
                $s = Shot 'gizmo'
                Check 'gizmo: the picture changed' ([V]::DiffView($a.Path, $s.Path, $lay.panel) -gt 0.0005) ("{0:P2} of the view" -f [V]::DiffView($a.Path, $s.Path, $lay.panel))
                $best = -1; $bestLen = -1.0
                for ($i = 0; $i -lt 3; $i++) { if ([double]$lay.gizmo.axisLen[$i] -gt $bestLen) { $bestLen = [double]$lay.gizmo.axisLen[$i]; $best = $i } }
                $undo0 = $lay.state.undo
                $o = @($lay.gizmo.origin); $tx = [double]$lay.gizmo.tipX[$best]; $ty = [double]$lay.gizmo.tipY[$best]
                $ox = [double]$o[0]; $oy = [double]$o[1]
                # start near the arrowhead, not at the origin: there all axes meet (distance 0 to every one) and the pick is a tie,
                # which would drag an axis that is not the one being moved
                $sx = [int]($ox + 0.8 * ($tx - $ox)); $sy = [int]($oy + 0.8 * ($ty - $oy))
                $ex = [int]($ox + 1.3 * ($tx - $ox)); $ey = [int]($oy + 1.3 * ($ty - $oy))
                Drag $sx $sy $ex $ey 10
                $lay = Layout
                Write-Host ("  cam yaw={0:F1} pitch={1:F1} zoom={2:F2} drag={3} undo={4} edits={5}" -f $lay.state.camYaw, $lay.state.camPitch, $lay.state.camZoom, $lay.state.drag, $lay.state.undo, $lay.state.edits)
                $b = Shot 'gizmo_drag'
                Check 'gizmo: the window answers after the drag' ($null -ne $lay)
                Check 'gizmo: the accessory was edited' ($lay.state.edits -ge 1) "$($lay.state.edits) edit(s)"
                Check 'gizmo: the picture changed' ([V]::DiffView($a.Path, $b.Path, $lay.panel) -gt 0.0005) ("{0:P2} of the view" -f [V]::DiffView($a.Path, $b.Path, $lay.panel))
                Check 'gizmo: the drag is one undo step' (($lay.state.undo - $undo0) -eq 1) "undo $undo0 -> $($lay.state.undo)"
                # right-click the chip to reset the move
                [void][V]::PostMessage($script:target, 0x204, [IntPtr]0, [V]::L([int]$acc.chip[0], [int]$acc.chip[1])); Start-Sleep -Milliseconds 80
                [void][V]::PostMessage($script:target, 0x205, [IntPtr]0, [V]::L([int]$acc.chip[0], [int]$acc.chip[1])); Start-Sleep -Milliseconds 700
                $lay = Layout
                $c = Shot 'gizmo_reset'
                Check 'gizmo: the window answers after the reset' ($null -ne $lay)
                Check 'gizmo: the reset puts the picture back' ([V]::DiffView($s.Path, $c.Path, $lay.panel) -lt 0.002) ("view diff {0:P3}" -f [V]::DiffView($s.Path, $c.Path, $lay.panel))
                # Esc deselects
                [void][V]::PostMessage($script:target, $WM_KEYDOWN, [IntPtr]27, [IntPtr]0); Start-Sleep -Milliseconds 400
                $lay = Layout
                Check 'gizmo: Esc deselects' ($null -eq $lay.gizmo)
            }
        }
    }

    # ---- the navigation widget (top right): click an axis ball to look from that side ----
    if (Want 'axeswidget') {
        Write-Host 'navigation widget'
        $lay = Layout
        if ($null -eq $lay -or $null -eq $lay.axesWidget) { Check 'axeswidget: the layout reports the widget' $false }
        else {
            $balls = $lay.axesWidget.balls
            Click ([int]$balls[2][0]) ([int]$balls[2][1]) 400
            $lay = Layout
            Check 'axeswidget: the view is at the front after +Z' ([math]::Abs([double]$lay.state.camYaw) -lt 0.01) ("yaw $($lay.state.camYaw)")
            $balls = $lay.axesWidget.balls
            Click ([int]$balls[0][0]) ([int]$balls[0][1]) 400
            $lay = Layout
            Check 'axeswidget: the view turns to +X' ([math]::Abs([double]$lay.state.camYaw - 90) -lt 0.01) ("yaw $($lay.state.camYaw)")
            $balls = $lay.axesWidget.balls
            $c = $lay.axesWidget.center
            Write-Host ("  drag from ({0},{1}) to ({2},{3})" -f $c[0], $c[1], ([int]$c[0] - 80), $c[1])
            Drag ([int]$c[0]) ([int]$c[1]) ([int]$c[0] - 80) ([int]$c[1]) 8
            $lay = Layout
            Check 'axeswidget: dragging it orbits the view' ([math]::Abs([double]$lay.state.camYaw - 90) -gt 5) ("yaw $($lay.state.camYaw)")
            $balls = $lay.axesWidget.balls
            Click ([int]$balls[5][0]) ([int]$balls[5][1]) 400
            $lay = Layout
            Check 'axeswidget: -Z snaps to the back view' ([math]::Abs([double]$lay.state.camYaw - 180) -lt 0.01) ("yaw $($lay.state.camYaw)")
            $balls = $lay.axesWidget.balls
            Click ([int]$balls[0][0]) ([int]$balls[0][1]) 400
            $lay = Layout
            Check 'axeswidget: back to the right view' ([math]::Abs([double]$lay.state.camYaw - 90) -lt 0.01) ("yaw $($lay.state.camYaw)")
        }
    }

    # ---- the Morphs tab: the exported PMX's morphs, live ----
    if (Want 'morphs') {
        Write-Host 'morphs tab'
        $lay = GoTab 4
        Check 'morphs: the tab opens' ($null -ne $lay -and $lay.tab -eq 4)
        # every morph of the exported PMX is listed: the game's patterns, the raw shapes and the MMD standard ones
        $sliders = @($lay.rows | Where-Object { $_.kind -eq 1 -and $_.y -ge 0 }).Count
        $heads = @($lay.rows | Where-Object { $_.kind -eq 0 }).Count
        $resetAll = @($lay.rows | Where-Object { $_.kind -eq 5 -and $_.y -ge 0 }).Count
        Check 'morphs: the morphs are listed, grouped' ($sliders -gt 5 -and $heads -ge 2 -and $resetAll -ge 1) "$sliders sliders, $heads group headers, reset-all present"
        # each section with left/right twins is split into Left and Right sub-groups
        $subs = @($lay.rows | Where-Object { $_.kind -eq 0 })
        $hasLeft = @($subs | Where-Object { $_.label -like 'Left*' }).Count -ge 1
        $hasRight = @($subs | Where-Object { $_.label -like 'Right*' }).Count -ge 1
        Check 'morphs: the Left and Right sub-groups are listed' ($hasLeft -and $hasRight) "Left: $hasLeft, Right: $hasRight"
        # click a group header: the group folds (its sliders disappear), click again: it re-opens
        $sec = $lay.rows | Where-Object { $_.kind -eq 0 -and $_.y -ge 0 } | Select-Object -First 1
        if ($null -eq $sec) { Check 'morphs: a section header is visible to click' $false }
        else {
            $full = @($lay.rows | Where-Object { $_.kind -eq 1 -and $_.y -ge 0 }).Count
            Click 30 ([int]$sec.y) 500
            $after = Layout
            $folded = @($after.rows | Where-Object { $_.kind -eq 1 -and $_.y -ge 0 }).Count
            Check 'morphs: clicking the header folds the group' ($null -ne $after -and $folded -lt $full) "$full sliders -> $folded"
            $sec2 = $after.rows | Where-Object { $_.kind -eq 0 } | Select-Object -First 1
            if ($null -eq $sec2) { Check 'morphs: the folded header is still listed' $false }
            else {
                Click 30 ([int]$sec2.y) 500
                $after2 = Layout
                $open = @($after2.rows | Where-Object { $_.kind -eq 1 -and $_.y -ge 0 }).Count
                Check 'morphs: clicking it again re-opens the group' ($null -ne $after2 -and $open -ge $full) "$folded -> $open"
            }
        }
        # Shift+click on a morph row renames its displayed name (settings file roundtrip, restored afterwards)
        $settingsPath = Join-Path $env:LOCALAPPDATA 'CardBridge\viewer.json'
        $backup = "$settingsPath.smoke.bak"
        $hadSettings = Test-Path $settingsPath
        if ($hadSettings) { Copy-Item $settingsPath $backup -Force }
        $row = $lay.rows | Where-Object { $_.kind -eq 1 -and $_.y -ge 0 } | Select-Object -First 1
        if ($null -ne $row) {
            $WM_CHAR = 0x102; $VK_SHIFT = 0x10; $VK_RETURN = 0x0D
            [void][V]::PostMessage($script:target, $WM_KEYDOWN, [IntPtr]$VK_SHIFT, [IntPtr]0)
            Click 60 ([int]$row.y) 400
            [void][V]::PostMessage($script:target, $WM_KEYDOWN, [IntPtr]0x2E, [IntPtr]0)   # VK_DELETE: clear the original name out of the box
            [void][V]::PostMessage($script:target, $WM_CHAR, [IntPtr][char]'A', [IntPtr]0)
            [void][V]::PostMessage($script:target, $WM_KEYDOWN, [IntPtr]$VK_RETURN, [IntPtr]0)
            [void][V]::PostMessage($script:target, 0x101, [IntPtr]$VK_SHIFT, [IntPtr]0)   # VK_SHIFT up
            Start-Sleep -Milliseconds 500
            $json = if (Test-Path $settingsPath) { Get-Content $settingsPath -Raw | ConvertFrom-Json } else { $null }
            $aliasCount = if ($json -and $json.morphAliases) { @($json.morphAliases.PSObject.Properties).Count } else { 0 }
            Check 'morphs: the rename is written to the viewer settings' ($aliasCount -ge 1) "$aliasCount alias(es)"
            # right-click on the renamed row restores the original name
            $lay2 = Layout
            $renamed = $lay2.rows | Where-Object { $_.kind -eq 1 -and $_.label -eq 'A' } | Select-Object -First 1
            if ($null -ne $renamed) {
                [void][V]::PostMessage($script:target, 0x204, [IntPtr]0, [V]::L(60, [int]$renamed.y)); Start-Sleep -Milliseconds 80
                [void][V]::PostMessage($script:target, 0x205, [IntPtr]0, [V]::L(60, [int]$renamed.y)); Start-Sleep -Milliseconds 500
                $json2 = if (Test-Path $settingsPath) { Get-Content $settingsPath -Raw | ConvertFrom-Json } else { $null }
                $aliasCount2 = if ($json2 -and $json2.morphAliases) { @($json2.morphAliases.PSObject.Properties).Count } else { 0 }
                Check 'morphs: right-click restores the original name' ($aliasCount2 -eq 0) "$aliasCount2 alias(es) left"
            } else {
                Check 'morphs: the renamed row is visible' $false
            }
        } else { Check 'morphs: a morph row is visible to rename' $false }
        if ($hadSettings) { Copy-Item $backup $settingsPath -Force; Remove-Item $backup -Force } else { Remove-Item $settingsPath -Force -ErrorAction SilentlyContinue }
        # drag a vertical gesture on the list: it scrolls (the row's y moves), and the window stays responsive
        $row3 = $lay.rows | Where-Object { $_.kind -eq 1 -and $_.y -ge 0 } | Select-Object -First 1
        if ($null -ne $row3) {
            $yBefore = [int]$row3.y
            Drag 60 $yBefore 60 ($yBefore - 90) 8 45
            $answered = Hook 10 8000
            $after3 = Layout
            $row4 = $after3.rows | Where-Object { $_.kind -eq 1 -and $_.y -ge 0 } | Select-Object -First 1
            $yAfter = if ($null -eq $row4) { -1 } else { [int]$row4.y }
            Check 'morphs: a vertical drag scrolls the list (the rows move)' ($answered -and ($yAfter -ne $yBefore)) "row y $yBefore -> $yAfter, answered $answered"
        }
        # (WM_MOUSEWHEEL carries SCREEN coordinates, and the private window sits off-screen: ask it where)
        $wr = New-Object V+RECT
        [void][V]::GetWindowRect($hwnd, [ref]$wr)
        $wx = $wr.Left + 160; $wy = $wr.Top + 400
        # the drag test above scrolled the shared slider list down; the steps after the morphs section expect it at the top, so scroll back
        for ($i = 0; $i -lt 12; $i++) { [void][V]::PostMessage($hwnd, 0x20A, [IntPtr](120 * 65536), [V]::L($wx, $wy)); Start-Sleep -Milliseconds 100 }
        # the earlier axes-widget step leaves the camera side-on (an eye edge-on is sub-pixel): click the front ball to face the camera again
        $layF = Layout
        if ($null -ne $layF -and $null -ne $layF.axesWidget) {
            $front = $layF.axesWidget.balls[2]   # +Z: the front view
            Click ([int]$front[0]) ([int]$front[1]) 600
        }
        # the earlier wheel-zoom tests leave the camera at the minimum zoom; pull it back so the face is visible for the picture check
        $lay = Layout
        for ($i = 0; $i -lt 40 -and $null -ne $lay -and $lay.state.camZoom -lt 0.9; $i++) {
            [void][V]::PostMessage($hwnd, 0x20A, [IntPtr](120 * 65536), [V]::L($wr.Left + 800, $wr.Top + 400))
            Start-Sleep -Milliseconds 100
            $lay = Layout
        }
        # the list holds every morph; scroll until a big one is visible ("Mouth 0 open" - the jaw drops - with "Eyes 0 closed" as fallback),
        # then drag it to full - a brow morph at this zoom barely clears the noise floor
        $lay = Layout
        $row = $null
        for ($i = 0; $i -lt 120 -and $null -eq $row; $i++) {
            foreach ($want in @('Eyes 0 closed', 'Mouth 0 open', 'あ', 'まばたき')) {
                $row = $lay.rows | Where-Object { $_.kind -eq 1 -and $_.y -ge 0 -and $_.label -eq $want } | Select-Object -First 1
                if ($null -ne $row) { break }
            }
            if ($null -ne $row) { break }
            [void][V]::PostMessage($hwnd, 0x20A, [IntPtr](-120 * 65536), [V]::L($wx, $wy))
            Start-Sleep -Milliseconds 150
            $lay = Layout
        }
        if ($null -eq $row) { $row = $lay.rows | Where-Object { $_.kind -eq 1 -and $_.y -ge 0 } | Select-Object -First 1 }
        if ($null -eq $row) { Check 'morphs: a morph row is visible' $false }
        else {
            $a = Shot 'before_morph'
            $x0 = [int]$lay.track[0] + 1; $x1 = [int]($lay.track[0] + 0.97 * ($lay.track[1] - $lay.track[0]))
            Drag $x0 ([int]$row.y) $x1 ([int]$row.y) 12 40
            $answered = Hook 10 8000
            $lay = Layout
            $b = Shot 'morph_max'
            Check 'morphs: the window answers after the drag (no deadlock)' $answered
            $d = [V]::DiffViewFine($a.Path, $b.Path, $lay.panel, 30, 1)
            # deterministic renders: 0 means nothing moved. The face is sub-pixel at full-body framing, so the blink moves only ~50 px; the bar
            # sits between the reset noise floor (~0.001 %) and the observed blink diff (~0.04 %).
            Check 'morphs: the picture changed' ($d -gt 0.00005) ("{0:P2} of the view, morph '{1}'" -f $d, $row.label)
            # right-click the row resets the weight to 0
            [void][V]::PostMessage($hwnd, 0x204, [IntPtr]0, [V]::L(60, [int]$row.y)); Start-Sleep -Milliseconds 80
            [void][V]::PostMessage($hwnd, 0x205, [IntPtr]0, [V]::L(60, [int]$row.y)); Start-Sleep -Milliseconds 700
            $lay = Layout
            $c = Shot 'morph_reset'
            Check 'morphs: the right-click reset puts the picture back' ([V]::DiffView($a.Path, $c.Path, $lay.panel) -lt 0.002) ("view diff {0:P3}" -f [V]::DiffView($a.Path, $c.Path, $lay.panel))
        }
    }

    # ---- the Body measurements (BWH) rows: three macro rows at the top of the Body tab ----
    if (Want 'bwh') {
        Write-Host 'body measurements (BWH)'
        $lay = GoTab 1
        $ids = @(22100, 22101, 22102)
        foreach ($id in $ids) {
            $row = $lay.rows | Where-Object { $_.id -eq $id -and $_.y -ge 0 } | Select-Object -First 1
            Check "bwh: the measurement row $id is listed" ($null -ne $row)
        }
        # the underlying Bust > Size slider (id 4) sits further down the same tab; the rows are kind 1
        $under = $lay.rows | Where-Object { $_.id -eq 4 -and $_.y -ge 0 }
        Check 'bwh: the underlying bust slider is still listed too' ($null -ne $under)
        # drag the Hip row: the width and depth sliders move together (the picture changes)
        $hip = $lay.rows | Where-Object { $_.id -eq 22102 -and $_.y -ge 0 } | Select-Object -First 1
        if ($null -eq $hip) { Check 'bwh: the hip row is visible to drag' $false }
        else {
            $a = Shot 'before_bwh'
            $x0 = [int]($lay.track[0] + 0.5 * ($lay.track[1] - $lay.track[0]))
            $x1 = [int]($lay.track[0] + 0.9 * ($lay.track[1] - $lay.track[0]))
            Drag $x0 ([int]$hip.y) $x1 ([int]$hip.y) 12 40
            $answered = Hook 10 8000
            $lay = WaitFor { param($l) -not $l.state.busy } 60
            Check 'bwh: the window answers during and after the drag' ($answered -and $null -ne $lay)
            $b = Shot 'bwh_hip'
            $d = [V]::DiffView($a.Path, $b.Path, $lay.panel)
            Check 'bwh: the hip measurement changes the picture' ($d -gt 0.0005) ("{0:P2} of the view" -f $d)
            # put it back: right-click the row resets the measurement to the card
            [void][V]::PostMessage($hwnd, 0x204, [IntPtr]0, [V]::L(60, [int]$hip.y)); Start-Sleep -Milliseconds 80
            [void][V]::PostMessage($hwnd, 0x205, [IntPtr]0, [V]::L(60, [int]$hip.y)); Start-Sleep -Milliseconds 600
            Check 'bwh: the window answers after the reset' (Hook 10)
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
        # the Areola row sits below the fold at scroll 0: wheel the list down until it is in view
        $wrA = New-Object V+RECT
        [void][V]::GetWindowRect($hwnd, [ref]$wrA)
        $ax = $wrA.Left + 160; $ay = $wrA.Top + 400
        $row = $lay.rows | Where-Object { $_.id -eq 201 -and $_.y -ge 0 } | Select-Object -First 1
        for ($i = 0; $i -lt 10 -and $null -eq $row; $i++) {
            [void][V]::PostMessage($hwnd, 0x20A, [IntPtr](-120 * 65536), [V]::L($ax, $ay))
            Start-Sleep -Milliseconds 200
            $lay = Layout
            $row = $lay.rows | Where-Object { $_.id -eq 201 -and $_.y -ge 0 } | Select-Object -First 1
        }
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
        $lay0 = Layout
        $was = [bool]$lay0.state.aPose
        Key 'P' 600
        $lay = Layout
        $b = Shot 'apose'
        # P toggles the switch: A-pose is on by default (commit 9a160c1), so a press can
        # turn it either way - what matters is that it moved.
        Check 'apose: the switch toggles' ([bool]$lay.state.aPose -ne $was) "was $was, now $($lay.state.aPose)"
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
        $lay = WaitFor { param($l) $l.state.expOk -and $l.state.showExp } 300
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
