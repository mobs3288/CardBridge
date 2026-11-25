param(
  [string]$Exe = 'D:\Project K\cardbridge\build\bin\cardbridge_viewer.exe',
  [string]$Card = 'C:\Users\pepsi\Downloads\KK_137244.png'
)
$ErrorActionPreference='Stop'
$library='D:\Project K\cardbridge\library_full'; $mods='E:/Game/[ScrewThisNoise] Koikatsu BetterRepack RX21/mods'
$out='D:\Project K\cardbridge\out\morph_visual'; New-Item -ItemType Directory -Force -Path $out|Out-Null
$layoutPath=Join-Path $out 'layout.json'
$WM_MOUSEMOVE=0x200;$WM_LBUTTONDOWN=0x201;$WM_LBUTTONUP=0x202;$WM_TEST=0x8028;$WM_CLOSE=0x10;$MK_LBUTTON=1
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;using System.Drawing;using System.Runtime.InteropServices;
public static class MV{
 [DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]public static extern IntPtr SendMessageTimeout(IntPtr h,uint m,IntPtr w,IntPtr l,uint f,uint t,out IntPtr r);
 [DllImport("user32.dll")]public static extern bool IsWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out RC r);
 [StructLayout(LayoutKind.Sequential)]public struct RC{public int L,T,R,B;}
 public static IntPtr L(int x,int y){return (IntPtr)(((y&0xFFFF)<<16)|(x&0xFFFF));}
 public static int Shot(IntPtr h,string p){RC r;GetWindowRect(h,out r);using(Bitmap b=new Bitmap(r.R-r.L,r.B-r.T)){using(Graphics g=Graphics.FromImage(b)){IntPtr dc=g.GetHdc();PrintWindow(h,dc,2);g.ReleaseHdc(dc);}b.Save(p,System.Drawing.Imaging.ImageFormat.Png);}return 1;}
 [DllImport("user32.dll")]public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 public static double Diff(string a,string b,int left){using(Bitmap x=new Bitmap(a))using(Bitmap y=new Bitmap(b)){int w=Math.Min(x.Width,y.Width),h=Math.Min(y.Height,x.Height),n=0,d=0;for(int j=0;j<h;j+=3)for(int i=left;i<w;i+=3){Color p=x.GetPixel(i,j),q=y.GetPixel(i,j);n++;if(Math.Abs(p.R-q.R)+Math.Abs(p.G-q.G)+Math.Abs(p.B-q.B)>48)d++;}return n==0?0:(double)d/n;}}
}
'@
$psi=New-Object System.Diagnostics.ProcessStartInfo; $psi.FileName=$Exe
$psi.Arguments=('--library "{0}" --mods "{1}" "{2}"' -f $library,$mods,$Card); $psi.UseShellExecute=$false
$psi.EnvironmentVariables['CARDBRIDGE_TEST_HOOKS']='1'; $psi.EnvironmentVariables['CARDBRIDGE_TEST_LAYOUT']=$layoutPath
$proc=[System.Diagnostics.Process]::Start($psi); $hwnd=[IntPtr]::Zero
function Hook([int]$cmd,[int]$t=8000){$r=[IntPtr]::Zero;return ([MV]::SendMessageTimeout($hwnd,$WM_TEST,[IntPtr]$cmd,[IntPtr]0,2,$t,[ref]$r) -ne [IntPtr]::Zero)}
function Layout(){if(Test-Path $layoutPath){Remove-Item $layoutPath -Force};if(-not(Hook 10)){return $null};for($i=0;$i -lt 20 -and -not(Test-Path $layoutPath);$i++){Start-Sleep -Milliseconds 100};if(-not(Test-Path $layoutPath)){return $null};return (Get-Content $layoutPath -Raw -Encoding UTF8|ConvertFrom-Json)}
try{
 $t0=Get-Date; while(((Get-Date)-$t0).TotalSeconds -lt 60 -and $hwnd -eq [IntPtr]::Zero){$proc.Refresh();$hwnd=$proc.MainWindowHandle;Start-Sleep -Milliseconds 200}
 if($hwnd -eq [IntPtr]::Zero){throw 'no window'}
 $lay=$null; $t0=Get-Date
 while(((Get-Date)-$t0).TotalSeconds -lt 240){$lay=Layout;if($null -ne $lay -and $lay.state.expOk -and $lay.state.showExp){break};Start-Sleep -Milliseconds 1000}
 if($null -eq $lay -or -not $lay.state.expOk){throw 'export preview bake did not finish in time'}
 Write-Host ("export preview ready (gpu={0}, showExp={1})" -f $lay.state.gpu,$lay.state.showExp)
 $tab=[int]$lay.tabs[4][0];$tabY=[int]$lay.tabs[4][1]
 [void][MV]::PostMessage($hwnd,$WM_LBUTTONDOWN,[IntPtr]$MK_LBUTTON,[MV]::L($tab,$tabY));Start-Sleep -Milliseconds 40
 [void][MV]::PostMessage($hwnd,$WM_LBUTTONUP,[IntPtr]0,[MV]::L($tab,$tabY));Start-Sleep -Milliseconds 400
 $lay=Layout
 # scroll until a visible big morph row is found
 $row=$null
 for($i=0;$i -lt 120 -and $null -eq $row;$i++){
   foreach($want in @('Eyes 0 closed','Mouth 0 open','Eye Happy')){$row=$lay.rows|Where-Object{$_.kind -eq 1 -and $_.y -ge 0 -and $_.label -eq $want}|Select-Object -First 1;if($null -ne $row){break}}
   if($null -ne $row){break}
   $wr=New-Object MV+RC;[void][MV]::GetWindowRect($hwnd,[ref]$wr)
   [void][MV]::PostMessage($hwnd,0x20A,[IntPtr](-120*65536),[MV]::L($wr.L+150,$wr.T+400));Start-Sleep -Milliseconds 120
   $lay=Layout
 }
 if($null -eq $row){throw 'no big morph row visible'}
 Write-Host ("found morph '{0}' at y={1}" -f $row.label,$row.y)
 $before=Join-Path $out 'before.png';$after=Join-Path $out 'after.png';$reset=Join-Path $out 'reset.png'
 [void][MV]::Shot($hwnd,$before)
 $x0=[int]$lay.track[0]+1;$x1=[int]($lay.track[0]+0.95*($lay.track[1]-$lay.track[0]));$y=[int]$row.y
 [void][MV]::PostMessage($hwnd,$WM_LBUTTONDOWN,[IntPtr]$MK_LBUTTON,[MV]::L($x0,$y));Start-Sleep -Milliseconds 60
 for($k=1;$k -le 12;$k++){[void][MV]::PostMessage($hwnd,$WM_MOUSEMOVE,[IntPtr]$MK_LBUTTON,[MV]::L([int]($x0+($x1-$x0)*$k/12),$y));Start-Sleep -Milliseconds 45}
 [void][MV]::PostMessage($hwnd,$WM_LBUTTONUP,[IntPtr]0,[MV]::L($x1,$y));Start-Sleep -Milliseconds 700
 Hook 10 | Out-Null
 [void][MV]::Shot($hwnd,$after)
 $d=[MV]::Diff($before,$after,[int]$lay.panel)
 Write-Host ("view diff after drag: {0:P3}" -f $d)
 [void][MV]::PostMessage($hwnd,0x204,[IntPtr]0,[MV]::L(60,$y));Start-Sleep -Milliseconds 80
 [void][MV]::PostMessage($hwnd,0x205,[IntPtr]0,[MV]::L(60,$y));Start-Sleep -Milliseconds 700
 [void][MV]::Shot($hwnd,$reset)
 $d2=[MV]::Diff($before,$reset,[int]$lay.panel)
 Write-Host ("view diff after reset: {0:P3}" -f $d2)
 Write-Host ("RESULT: changed={0} reset_ok={1} showExp={2}" -f ($d -gt 0.00005),($d2 -lt 0.002),((Layout).state.showExp))
} finally {
 if($hwnd -ne [IntPtr]::Zero -and [MV]::IsWindow($hwnd)){[void][MV]::PostMessage($hwnd,$WM_CLOSE,[IntPtr]0,[IntPtr]0)}
 if(-not $proc.HasExited){[void]$proc.WaitForExit(5000)}
}
