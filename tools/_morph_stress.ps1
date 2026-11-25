param(
  [string]$Card = 'C:\Users\pepsi\Downloads\KK_137244.png',
  [string]$Exe = 'D:\Project K\build-cardbridge\bin\cardbridge_viewer.exe',
  [int]$Seconds = 60,
  [switch]$Lit,
  [switch]$Cycle
)
$ErrorActionPreference = 'Stop'
$repo = 'D:\Project K\cardbridge'
$library = Join-Path $repo 'library_full'
$mods = 'E:/Game/[ScrewThisNoise] Koikatsu BetterRepack RX21/mods'
$out = Join-Path $repo 'out\morph_stress'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$layoutPath = Join-Path $out 'layout.json'
$WM_MOUSEMOVE=0x200; $WM_LBUTTONDOWN=0x201; $WM_LBUTTONUP=0x202; $WM_TEST=0x8028; $WM_CLOSE=0x10; $MK_LBUTTON=1
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SV {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  public static IntPtr L(int x,int y){return (IntPtr)(((y&0xFFFF)<<16)|(x&0xFFFF));}
}
'@
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = ('--library "{0}" --mods "{1}" "{2}"' -f $library,$mods,$Card)
$psi.UseShellExecute = $false
$psi.EnvironmentVariables['CARDBRIDGE_TEST_HOOKS']='1'
$psi.EnvironmentVariables['CARDBRIDGE_TEST_LAYOUT']=$layoutPath
$proc = [System.Diagnostics.Process]::Start($psi)
$hwnd = [IntPtr]::Zero
function Hook([int]$cmd,[int]$timeout=5000){ $r=[IntPtr]::Zero; return ([SV]::SendMessageTimeout($hwnd,$WM_TEST,[IntPtr]$cmd,[IntPtr]0,2,$timeout,[ref]$r) -ne [IntPtr]::Zero) }
function Layout(){ if(Test-Path $layoutPath){Remove-Item $layoutPath -Force}; if(-not (Hook 10)){return $null}; for($i=0;$i -lt 20 -and -not(Test-Path $layoutPath);$i++){Start-Sleep -Milliseconds 100}; if(-not(Test-Path $layoutPath)){return $null}; return (Get-Content $layoutPath -Raw -Encoding UTF8|ConvertFrom-Json) }
try {
  $t0=Get-Date
  while(((Get-Date)-$t0).TotalSeconds -lt 90 -and $hwnd -eq [IntPtr]::Zero){ $proc.Refresh(); if($proc.HasExited){throw "viewer exited code $($proc.ExitCode)"}; $hwnd=$proc.MainWindowHandle; Start-Sleep -Milliseconds 200 }
  if($hwnd -eq [IntPtr]::Zero){throw 'no window'}
  $lay=$null
  $t0=Get-Date
  while(((Get-Date)-$t0).TotalSeconds -lt 90){ $lay=Layout; if($null -ne $lay -and $lay.rows){break}; Start-Sleep -Milliseconds 500 }
  if($null -eq $lay){throw 'card did not load'}
  Write-Host ("loaded: {0} rows, gpu={1}" -f $lay.rows.Count,$lay.state.gpu)
  # Wait for the one-time as-exported bake (WARP can take a couple of minutes) so the stress measures morph handling, not baking.
  $tBake=Get-Date
  while(((Get-Date)-$tBake).TotalSeconds -lt 240){ $lay=Layout; if($null -ne $lay -and $lay.state.expOk){break}; Start-Sleep -Milliseconds 800 }
  Write-Host ("bake ready: expOk={0}, showExp={1}" -f $lay.state.expOk,$lay.state.showExp)
  if($Lit){
    [void][SV]::PostMessage($hwnd,0x100,[IntPtr][int][char]'X',[IntPtr]0); Start-Sleep -Milliseconds 100
    $t1=Get-Date
    while(((Get-Date)-$t1).TotalSeconds -lt 90){ $lay=Layout; if($null -ne $lay -and -not $lay.state.showExp){break}; Start-Sleep -Milliseconds 300 }
    Write-Host ("Lit view: gpu={0}, showExp={1}" -f $lay.state.gpu,$lay.state.showExp)
  }
  $tab=[int]$lay.tabs[4][0]; $tabY=[int]$lay.tabs[4][1]
  [void][SV]::PostMessage($hwnd,$WM_LBUTTONDOWN,[IntPtr]$MK_LBUTTON,[SV]::L($tab,$tabY)); Start-Sleep -Milliseconds 40
  [void][SV]::PostMessage($hwnd,$WM_LBUTTONUP,[IntPtr]0,[SV]::L($tab,$tabY)); Start-Sleep -Milliseconds 400
  $lay=Layout
  $row=$lay.rows | Where-Object { $_.kind -eq 1 -and $_.y -ge 0 } | Select-Object -First 1
  if($null -eq $row){ throw 'no morph slider visible' }
  $y=[int]$row.y; $x0=[int]$lay.track[0]+2; $x1=[int]$lay.track[1]-2
  Write-Host ("dragging morph '{0}' at y={1} track {2}..{3}" -f $row.label,$y,$x0,$x1)
  $start=Get-Date; $n=0; $slow=0; $maxWs=0; $hangs=0; $switches=0; $rowIdx=0
  $rows = @($lay.rows | Where-Object { $_.kind -eq 1 -and $_.y -ge 0 })
  [void][SV]::PostMessage($hwnd,$WM_LBUTTONDOWN,[IntPtr]$MK_LBUTTON,[SV]::L($x0,$y))
  while(((Get-Date)-$start).TotalSeconds -lt $Seconds){
    if($Cycle -and $rows.Count -gt 1 -and (($n % 400) -eq 0)){
      # release the old slider and press a different visible morph: exercises switching and several morphs active at once
      [void][SV]::PostMessage($hwnd,$WM_LBUTTONUP,[IntPtr]0,[SV]::L($x1,$y)); Start-Sleep -Milliseconds 60
      $rowIdx=($rowIdx+1) % $rows.Count; $y=[int]$rows[$rowIdx].y; $switches++
      [void][SV]::PostMessage($hwnd,$WM_LBUTTONDOWN,[IntPtr]$MK_LBUTTON,[SV]::L($x0,$y)); Start-Sleep -Milliseconds 60
    }
    for($k=0;$k -lt 40;$k++){
      $x = if(($k % 2) -eq 0){$x1}else{$x0}
      [void][SV]::PostMessage($hwnd,$WM_MOUSEMOVE,[IntPtr]$MK_LBUTTON,[SV]::L($x,$y)); $n++
    }
    Start-Sleep -Milliseconds 120
    $before=Get-Date
    $ok=Hook 10 5000
    $resp=((Get-Date)-$before).TotalMilliseconds
    if(-not $ok){$hangs++}
    if($resp -gt 200){$slow++}
    $proc.Refresh(); $ws=[math]::Round($proc.WorkingSet64/1MB,1); if($ws -gt $maxWs){$maxWs=$ws}
    if(($n % 400) -eq 0){ Write-Host ("  {0:N0} moves, resp {1:N0} ms, WS {2} MB, hangs {3}, switches {4}" -f $n,$resp,$ws,$hangs,$switches) }
  }
  [void][SV]::PostMessage($hwnd,$WM_LBUTTONUP,[IntPtr]0,[SV]::L($x1,$y))
  Write-Host ("DONE: {0} moves in {1:N1}s, slow answers {2}, hangs {3}, max WS {4} MB, switches {5}, exited={6}" -f $n,((Get-Date)-$start).TotalSeconds,$slow,$hangs,$maxWs,$switches,$proc.HasExited)
} finally {
  if($hwnd -ne [IntPtr]::Zero -and [SV]::IsWindow($hwnd)){ [void][SV]::PostMessage($hwnd,$WM_CLOSE,[IntPtr]0,[IntPtr]0) }
  if(-not $proc.HasExited){ if(-not $proc.WaitForExit(5000)){ Write-Host 'private viewer did not close on WM_CLOSE; leaving it (not killing by name)' } }
}
