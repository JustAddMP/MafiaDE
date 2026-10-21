# Shared settings and helpers for the one-PC two-instance test (tools/onepc/README.md). Dot-source it.
param()
$Repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$Out = Join-Path $Repo "out"
# Where the game keeps saves and Saves\videoconfig.cfg (OneDrive-redirected on the test PC)
$GameDocs = if ($env:MC_GAME_DOCS) { $env:MC_GAME_DOCS } else { Join-Path ([Environment]::GetFolderPath("MyDocuments")) "My Games\Mafia Definitive Edition" }
# The screen area the windows must stay in (the test PC: x 2916-4233; windows 1296x759, host on top, guest below)
$AreaX = if ($env:MC_AREA_X) { [int]$env:MC_AREA_X } else { 2916 }
$AreaR = if ($env:MC_AREA_R) { [int]$env:MC_AREA_R } else { 4233 }
$GuestY = 633
$Work = if ($env:MC_ONEPC_WORK) { $env:MC_ONEPC_WORK } else { Join-Path $env:TEMP "mafiacoop-onepc" }
New-Item -ItemType Directory -Force $Work, "$Work\shots" | Out-Null

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Drawing; using System.Drawing.Imaging;
public static class OnePc {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int hh, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  public struct RECT { public int L, T, R, B; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public static IntPtr Find(uint pid) { IntPtr found = IntPtr.Zero; int best = 0;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(64); GetClassName(h, sb, 64);
      if (p == pid && sb.ToString() == "Scotch" && IsWindowVisible(h)) { RECT r; GetWindowRect(h, out r); int a = (r.R - r.L) * (r.B - r.T); if (a > best) { best = a; found = h; } } return true; }, IntPtr.Zero); return found; }
  public static void Key(byte vk, int hold) { byte sc = (byte)MapVirtualKey(vk, 0);
    keybd_event(vk, sc, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(hold); keybd_event(vk, sc, 2, UIntPtr.Zero); }
  public static void Shot(IntPtr h, string path) { RECT r; GetWindowRect(h, out r);
    using (var bmp = new Bitmap(r.R - r.L, r.B - r.T)) { using (var g = Graphics.FromImage(bmp)) g.CopyFromScreen(r.L, r.T, 0, 0, bmp.Size);
      bmp.Save(path, ImageFormat.Png); } }
}
"@

function Games { @(Get-Process mafiadefinitiveedition -ErrorAction SilentlyContinue | Sort-Object StartTime) }
function GameWindow([int]$i) { $g = Games; if ($g.Count -le $i) { return [IntPtr]::Zero }; [OnePc]::Find([uint32]$g[$i].Id) }
function Rect([IntPtr]$h) { $r = New-Object OnePc+RECT; [OnePc]::GetWindowRect($h, [ref]$r) | Out-Null; $r }
# Puts window i (0 host, 1 guest) inside the area; returns a line for the log
function Fit([int]$i) {
  $h = GameWindow $i; if ($h -eq [IntPtr]::Zero) { return "game $i has no window" }
  $r = Rect $h; $y = $i * $GuestY
  if ($r.L -lt $AreaX -or $r.R -gt $AreaR -or $r.T -ne $y -or ($r.R - $r.L) -gt 1300) {
    [OnePc]::SetWindowPos($h, [IntPtr]::Zero, $AreaX, $y, 1296, 759, 0x0004 -bor 0x0010) | Out-Null
    $n = Rect $h
    return "game $i window was $($r.L),$($r.T)-$($r.R),$($r.B): moved to $($n.L),$($n.T)-$($n.R),$($n.B)"
  }
  "game $i window ok $($r.L),$($r.T)-$($r.R),$($r.B)"
}
$Vk = @{ ENTER = 0x0D; ESC = 0x1B; W = 0x57; S = 0x53; A = 0x41; D = 0x44; E = 0x45; F = 0x46; SPACE = 0x20 }
# Real key presses go only to a game window that is in front (aborts otherwise)
function Keys([int]$i, [string[]]$keys, [int]$holdMs = 150, [int]$gapMs = 600) {
  $h = GameWindow $i; if ($h -eq [IntPtr]::Zero) { return "no window" }
  [OnePc]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [OnePc]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
  [OnePc]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300
  foreach ($k in $keys) {
    if ([OnePc]::GetForegroundWindow() -ne $h) { return "game $i is not in front: keys not sent" }
    [OnePc]::Key([byte]$Vk[$k], $holdMs); Start-Sleep -Milliseconds $gapMs
  }
  "sent $($keys -join ',') to game $i"
}
# A screenshot of game window i only (it is brought to the front first; its rect on the screen is copied)
function Shot([int]$i, [string]$name) {
  $h = GameWindow $i; if ($h -eq [IntPtr]::Zero) { return "no window" }
  [OnePc]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 700
  $f = Join-Path "$Work\shots" "$name-$(@('host','guest')[$i]).png"; [OnePc]::Shot($h, $f); $f
}
# Runs a Lua file or chunk in game i through the dev console (out\dev\<tag>.in.lua)
function Lua([int]$i, [string]$file, [string]$code) {
  $tag = @('host', 'guest')[$i]
  if ($file) { & "$Repo\tools\devlua.ps1" -Tag $tag -File $file } else { & "$Repo\tools\devlua.ps1" -Tag $tag -Code $code }
}
function LogHas([string]$tag, [string]$pattern) { (Get-Content "$Out\logs\mafiacoop-$tag.log" -Raw -ErrorAction SilentlyContinue) -match $pattern }
# (another reader of run.log, e.g. a tail, makes Add-Content fail now and then: tried again)
function Say([string]$s) { $line = "$(Get-Date -f HH:mm:ss) $s"; $line
  for ($k = 0; $k -lt 10; $k++) { try { Add-Content "$Work\run.log" $line -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 100 } } }
# The rows of one of our panels on game $tag: test_input's dump after a marker line (index, selected, label)
function PanelRows([string]$tag, [string]$open) {
  $mark = "rows-$(Get-Random)"
  Set-Content "$Out\dev\$tag.input.txt" "$open`nwait 800`nlog $mark`ndump"
  Start-Sleep 3
  $lines = Get-Content "$Out\logs\mafiacoop-$tag.log"
  $at = -1
  for ($i = $lines.Count - 1; $i -ge 0; $i--) { if ($lines[$i] -match [regex]::Escape($mark)) { $at = $i; break } }
  $rows = @()
  if ($at -ge 0) {
    for ($i = $at + 1; $i -lt $lines.Count; $i++) {
      if ($lines[$i] -match "test_input:   \[(\d+)\](\*?)\s+(.+?)  \(") { $rows += [pscustomobject]@{ i = [int]$Matches[1]; sel = $Matches[2] -eq "*"; label = $Matches[3] } }
    }
  }
  $rows
}
# Selects the row whose label starts with $label on the open panel and presses A (pad DOWN / UP from the selected row)
function PickRow([string]$tag, $rows, [string]$label) {
  $target = $rows | Where-Object { $_.label -like "$label*" } | Select-Object -First 1
  $cur = $rows | Where-Object { $_.sel } | Select-Object -First 1
  if (-not $target) { return "no row '$label' (rows: $(($rows | ForEach-Object { $_.label }) -join ' | '))" }
  $from = if ($cur) { $cur.i } else { 0 }
  $d = $target.i - $from
  $cmd = @()
  for ($k = 0; $k -lt [Math]::Abs($d); $k++) { $cmd += $(if ($d -gt 0) { "pad DOWN" } else { "pad UP" }); $cmd += "wait 250" }
  $cmd += "dump"; $cmd += "pad A"; $cmd += "wait 500"
  Set-Content "$Out\dev\$tag.input.txt" ($cmd -join "`n")
  Start-Sleep 3
  "picked '$($target.label)' (row $($target.i), from $from)"
}
function Front([int]$i) {
  $h = GameWindow $i; if ($h -eq [IntPtr]::Zero) { return }
  [OnePc]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [OnePc]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
  [OnePc]::SetForegroundWindow($h) | Out-Null; Start-Sleep 1
}
