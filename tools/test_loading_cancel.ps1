$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class W2 {
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@
$layout = "$env:TEMP\cardbridge_cancel_layout.json"
$env:CARDBRIDGE_TEST_HOOKS = '1'
$env:CARDBRIDGE_TEST_LAYOUT = $layout
$card = "$env:USERPROFILE\Downloads\KKS.png"
$psi = [Diagnostics.ProcessStartInfo]::new()
$psi.FileName = 'D:\Project K\cardbridge\build\bin\cardbridge_viewer.exe'
$psi.Arguments = '"--library" "D:/Project K/cardbridge/library_full" "--mods" "D:/Project K/cardbridge/_missing_mods_for_measure" "' + $card + '"'
$psi.UseShellExecute = $false
$p = [Diagnostics.Process]::Start($psi)
try {
  $h = [IntPtr]::Zero
  for ($i=0; $i -lt 100 -and $h -eq [IntPtr]::Zero; $i++) { $p.Refresh(); $h=$p.MainWindowHandle; Start-Sleep -Milliseconds 100 }
  if ($h -eq [IntPtr]::Zero) { throw 'no window' }
  Start-Sleep -Milliseconds 800
  [void][W2]::PostMessage($h,0x100,[IntPtr]0x1B,[IntPtr]::Zero)  # VK_ESCAPE cancels the load
  [void][W2]::PostMessage($h,0x101,[IntPtr]0x1B,[IntPtr]::Zero)
  $polls=0; $timeouts=0
  for ($i=0; $i -lt 60; $i++) {
    $r=[IntPtr]::Zero
    $ok=[W2]::SendMessageTimeout($h,0x8028,[IntPtr]10,[IntPtr]::Zero,2,1000,[ref]$r)
    ++$polls
    if ($ok -eq [IntPtr]::Zero) { ++$timeouts }
    $j = Get-Content $layout -Raw | ConvertFrom-Json
    if (-not $j.progress.active -and -not $j.state.busy) { break }
    Start-Sleep -Milliseconds 250
  }
  "after cancel: loaded=$($j.state.loaded) busy=$($j.state.busy) progressActive=$($j.progress.active) polls=$polls timeouts=$timeouts"
  # the window must still work after a cancelled load
  $r=[IntPtr]::Zero
  $ok=[W2]::SendMessageTimeout($h,0x8028,[IntPtr]10,[IntPtr]::Zero,2,1500,[ref]$r)
  "window answers after cancel: $($ok -ne [IntPtr]::Zero)"
}
finally {
  if ($h -ne [IntPtr]::Zero) { [void][W2]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero) }
  if (-not $p.WaitForExit(5000)) { $p.Kill(); $p.WaitForExit(5000) }
}
