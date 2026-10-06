param([string]$Dir = "peggy_test", [string]$Keys = "", [string]$mask = "127", [string]$File = "", [int]$Wait = 6, [string]$Tag = "p", [int]$Post = 0, [switch]$Close)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public class W2 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Auto)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Auto)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string cls, string t);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  public static IntPtr FindButton(IntPtr dlg) {
    IntPtr found = IntPtr.Zero;
    EnumChildWindows(dlg, (h, x) => { var c = new StringBuilder(64); GetClassName(h, c, 64); if (found == IntPtr.Zero && c.ToString() == "Button") found = h; return true; }, IntPtr.Zero);
    return found;
  }
  public static List<IntPtr> ForPid(uint pid) {
    var l = new List<IntPtr>();
    EnumWindows((h, x) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) l.Add(h); return true; }, IntPtr.Zero);
    return l;
  }
}
"@
$env:SLOT_MASK = $mask
$dir = Join-Path $PSScriptRoot $Dir
if ($File) { $p = Start-Process "$dir\peggypro.exe" -ArgumentList "`"$File`"" -PassThru -WorkingDirectory $dir } else { $p = Start-Process "$dir\peggypro.exe" -PassThru -WorkingDirectory $dir }
Start-Sleep -Seconds $Wait
# press OK on the standard trial-reminder dialog (same as a user clicking OK)
for ($round = 0; $round -lt 3; $round++) { foreach ($h in [W2]::ForPid([uint32]$p.Id)) {
  $c = New-Object System.Text.StringBuilder 256; [W2]::GetClassName($h, $c, 256) | Out-Null
  if ($c.ToString() -eq "#32770" -and [W2]::IsWindowVisible($h)) { $bt = [W2]::FindButton($h); if ($bt -ne [IntPtr]::Zero) { [W2]::SendMessage($bt, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }; [W2]::PostMessage($h, 0x100, [IntPtr]0x0D, [IntPtr]0x1C0001) | Out-Null; [W2]::PostMessage($h, 0x101, [IntPtr]0x0D, [IntPtr]([int]0xC01C0001)) | Out-Null }
} Start-Sleep -Seconds 2 }
if ($Keys) {
  Add-Type -AssemblyName System.Windows.Forms
  Add-Type @"
using System; using System.Runtime.InteropServices;
public class FG { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); }
"@
  foreach ($h in [W2]::ForPid([uint32]$p.Id)) { $c = New-Object System.Text.StringBuilder 256; [W2]::GetClassName($h,$c,256) | Out-Null; if ($c.ToString() -eq "PeggyProWindowClass") { [FG]::SetForegroundWindow($h) | Out-Null } }
  Start-Sleep -Milliseconds 800
  foreach ($k in $Keys.Split("|")) { [System.Windows.Forms.SendKeys]::SendWait($k); Start-Sleep -Milliseconds 1500 }
  Start-Sleep -Seconds 1
}
Start-Sleep -Seconds (1 + $Post)
"exited=$($p.HasExited)"
$i = 0
foreach ($h in [W2]::ForPid([uint32]$p.Id)) {
  $t = New-Object System.Text.StringBuilder 256; $c = New-Object System.Text.StringBuilder 256
  [W2]::GetWindowText($h, $t, 256) | Out-Null; [W2]::GetClassName($h, $c, 256) | Out-Null
  $v = [W2]::IsWindowVisible($h)
  $r = New-Object W2+RECT; [W2]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $ht = $r.B - $r.T
  "win $h class=$c title='$t' visible=$v ${w}x${ht}"
  if ($v -and $w -gt 50 -and $ht -gt 50) {
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [W2]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc)
    $out = Join-Path $PSScriptRoot "out\${Tag}_$i.png"; $bmp.Save($out); "  saved $out"; $i++
  }
}
if ($Close -and -not $p.HasExited) { $p.CloseMainWindow() | Out-Null; $p.WaitForExit(8000) | Out-Null }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
