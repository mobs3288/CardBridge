$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@
$layout = "$env:TEMP\cardbridge_concurrency_layout.json"
$env:CARDBRIDGE_TEST_HOOKS = '1'
$env:CARDBRIDGE_TEST_LAYOUT = $layout
$card = "$env:USERPROFILE\Downloads\KK_006325.png"
$psi = [Diagnostics.ProcessStartInfo]::new()
$psi.FileName = 'D:\Project K\cardbridge\build\bin\cardbridge_viewer.exe'
$psi.Arguments = '"--library" "D:/Project K/cardbridge/library_full" "--mods" "D:/Project K/cardbridge/_missing_mods_for_measure" "' + $card + '"'
$psi.UseShellExecute = $false
$p = [Diagnostics.Process]::Start($psi)
try {
  $h = [IntPtr]::Zero
  for ($i=0; $i -lt 150 -and $h -eq [IntPtr]::Zero; $i++) { $p.Refresh(); $h = $p.MainWindowHandle; Start-Sleep -Milliseconds 100 }
  if ($h -eq [IntPtr]::Zero) { throw 'no window' }
  # wait for the first load
  $loaded = $false
  for ($i=0; $i -lt 400 -and -not $loaded; $i++) {
    $r=[IntPtr]::Zero; [void][W]::SendMessageTimeout($h,0x8028,[IntPtr]10,[IntPtr]::Zero,2,1000,[ref]$r)
    if (Test-Path $layout) { $j = Get-Content $layout -Raw | ConvertFrom-Json; $loaded = $j.state.loaded -and -not $j.state.busy }
    Start-Sleep -Milliseconds 200
  }
  "initial load: $loaded"
  $outfits = @()
  for ($k=0; $k -lt 4; $k++) {
    [void][W]::PostMessage($h,0x100,[IntPtr]0x27,[IntPtr]::Zero)  # VK_RIGHT
    [void][W]::PostMessage($h,0x101,[IntPtr]0x27,[IntPtr]::Zero)
    $done = $false
    for ($i=0; $i -lt 300 -and -not $done; $i++) {
      $r=[IntPtr]::Zero
      $ok = [W]::SendMessageTimeout($h,0x8028,[IntPtr]10,[IntPtr]::Zero,2,1500,[ref]$r)
      if ($ok -eq [IntPtr]::Zero) { throw "window stopped answering at switch $k" }
      $j = Get-Content $layout -Raw | ConvertFrom-Json
      $done = $j.state.loaded -and -not $j.state.busy
      Start-Sleep -Milliseconds 200
    }
    if (-not $done) { throw "outfit switch $k never completed" }
    $p.Refresh()
    $outfits += "$($j.outfit)/WS$([Math]::Round($p.WorkingSet64/1MB))/PB$([Math]::Round($p.PrivateMemorySize64/1MB))"
  }
  "outfit switches (outfit/loaded/busy): $($outfits -join ' ')"
  $j = Get-Content $layout -Raw | ConvertFrom-Json
  "final: bones=$($j.state.bones) loaded=$($j.state.loaded) busy=$($j.state.busy)"
  $reloads = @()
  for ($k=0; $k -lt 4; $k++) {
    $r=[IntPtr]::Zero; [void][W]::SendMessageTimeout($h,0x8028,[IntPtr]12,[IntPtr]::Zero,2,1500,[ref]$r)  # reset clothing -> reload same slot
    $done = $false
    for ($i=0; $i -lt 300 -and -not $done; $i++) {
      $r=[IntPtr]::Zero
      $ok=[W]::SendMessageTimeout($h,0x8028,[IntPtr]10,[IntPtr]::Zero,2,1500,[ref]$r)
      if ($ok -eq [IntPtr]::Zero) { throw "window stopped answering at reload $k" }
      $j = Get-Content $layout -Raw | ConvertFrom-Json
      $done = $j.state.loaded -and -not $j.state.busy
      Start-Sleep -Milliseconds 200
    }
    $p.Refresh()
    $reloads += "$($j.outfit)/PB$([Math]::Round($p.PrivateMemorySize64/1MB))"
  }
  "same-slot reloads: $($reloads -join ' ')"
}
finally {
  if ($h -ne [IntPtr]::Zero) { [void][W]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero) }
  if (-not $p.WaitForExit(5000)) { $p.Kill(); $p.WaitForExit(5000) }
}
