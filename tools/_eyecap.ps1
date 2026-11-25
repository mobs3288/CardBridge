param([string]$Out='D:\Project K\cardbridge\out\eyecap')
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class E {
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h,uint m,IntPtr w,IntPtr l,uint f,uint t,out IntPtr r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 public static IntPtr L(int x,int y){return (IntPtr)(((y&0xFFFF)<<16)|(x&0xFFFF));}
 public static void Shot(IntPtr h,string p){RECT r;GetWindowRect(h,out r);using(Bitmap b=new Bitmap(r.Right-r.Left,r.Bottom-r.Top,PixelFormat.Format32bppArgb)){using(Graphics g=Graphics.FromImage(b)){IntPtr dc=g.GetHdc();PrintWindow(h,dc,2);g.ReleaseHdc(dc);}b.Save(p,ImageFormat.Png);}}
}
'@
$psi=New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName='D:\Project K\cardbridge\build\bin\cardbridge_viewer.exe'
$psi.Arguments='--library "D:/Project K/cardbridge/library_full" --mods "E:/Game/[ScrewThisNoise] Koikatsu BetterRepack RX21/mods" --outfit 0 "C:\Users\pepsi\Downloads\KK_296927.png"'
$psi.UseShellExecute=$false
$psi.EnvironmentVariables['CARDBRIDGE_TEST_HOOKS']='1'
$lp=Join-Path $Out 'layout.json'; $psi.EnvironmentVariables['CARDBRIDGE_TEST_LAYOUT']=$lp
$p=[System.Diagnostics.Process]::Start($psi)
$h=[IntPtr]::Zero
for($i=0;$i -lt 150 -and $h -eq [IntPtr]::Zero;$i++){$p.Refresh();$h=$p.MainWindowHandle;Start-Sleep -Milliseconds 200}
function Layout(){Remove-Item $lp -Force -ErrorAction SilentlyContinue;$r=[IntPtr]::Zero;[void][E]::SendMessageTimeout($h,0x8028,10,[IntPtr]::Zero,2,10000,[ref]$r);for($i=0;$i -lt 30 -and -not (Test-Path $lp);$i++){Start-Sleep -Milliseconds 100};if(Test-Path $lp){return (Get-Content $lp -Raw -Encoding UTF8|ConvertFrom-Json)}else{return $null}}
for($i=0;$i -lt 500;$i++){$l=Layout;if($l -and $l.state.expOk -and $l.state.showExp -and -not $l.state.busy){break};Start-Sleep -Milliseconds 300}
Write-Host "loaded=$($l.state.loaded) expOk=$($l.state.expOk) i=$i"
[E]::Shot($h,(Join-Path $Out '01_mmd.png'))
[void][E]::PostMessage($h,0x100,[IntPtr][int][char]'X',[IntPtr]::Zero);Start-Sleep -Seconds 4
[E]::Shot($h,(Join-Path $Out '02_lit.png'))
[void][E]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero);Start-Sleep -Milliseconds 500
if(-not $p.HasExited){$p|Stop-Process -Force}
