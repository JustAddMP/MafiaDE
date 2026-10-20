# Keeps the two test games' windows inside the test area (host top, guest below, 1296x759; a window the game made
# bigger is shrunk at once) without activating them, and mutes their audio sessions, four times a second, until the
# stop file exists. Stop it (run_stop.ps1) BEFORE unmuting, or it mutes again.
param([string]$StopFile = "$env:TEMP\mafiacoop-onepc\keeper.stop", [int]$X = 2916, [int]$W = 1317, [int]$H = 696, [int]$GuestY = 633, [switch]$Once)
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public static class GW {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint f);
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr h, int c);
  public struct RECT { public int L, T, R, B; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public static IntPtr Find(uint pid) { IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(64); GetClassName(h, sb, 64);
      if (p == pid && sb.ToString() == "Scotch" && IsWindowVisible(h)) { found = h; return false; } return true; }, IntPtr.Zero); return found; }
  // returns true when moved
  public static bool Place(IntPtr h, int x, int y, int w, int hh) {
    RECT r; if (!GetWindowRect(h, out r)) return false;
    if (r.R - r.L > 1317 || r.B - r.T > 800) { SetWindowPos(h, IntPtr.Zero, x, y, 1296, 759, 0x0004 | 0x0010); return true; }
    hh = r.B - r.T;
    if (r.L == x && r.T == y) return false;
    if (IsZoomed(h) || IsIconic(h)) ShowWindowAsync(h, 4 /*SW_SHOWNOACTIVATE*/);
    SetWindowPos(h, IntPtr.Zero, x, y, 0, 0, 0x0001 | 0x0004 | 0x0010 | 0x4000); // NOSIZE|NOZORDER|NOACTIVATE|ASYNC
    return true;
  }
}
[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator { int EnumAudioEndpoints(int flow, int mask, out IMMDeviceCollection c); }
[Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceCollection { int GetCount(out int n); int Item(int i, out IMMDevice d); }
[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice { int Activate(ref Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o); }
[Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionManager2 { int GetAudioSessionControl(IntPtr a, int b, out IntPtr c); int GetSimpleAudioVolume(IntPtr a, int b, out IntPtr c);
  int GetSessionEnumerator(out IAudioSessionEnumerator e); }
[Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionEnumerator { int GetCount(out int n); int GetSession(int i, [MarshalAs(UnmanagedType.IUnknown)] out object s); }
[Guid("bfb7ff88-7239-4fc9-8fa2-07c950be9c6d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionControl2 { int GetState(out int s); int GetDisplayName(out IntPtr n); int SetDisplayName(string n, ref Guid g);
  int GetIconPath(out IntPtr p); int SetIconPath(string p, ref Guid g); int GetGroupingParam(out Guid g); int SetGroupingParam(ref Guid a, ref Guid b);
  int RegisterAudioSessionNotification(IntPtr n); int UnregisterAudioSessionNotification(IntPtr n);
  int GetSessionIdentifier(out IntPtr s); int GetSessionInstanceIdentifier(out IntPtr s); int GetProcessId(out uint pid); }
[Guid("87CE5498-68D6-44E5-9215-6DA47EF883D8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface ISimpleAudioVolume { int SetMasterVolume(float v, ref Guid g); int GetMasterVolume(out float v); int SetMute(bool m, ref Guid g); int GetMute(out bool m); }
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumeratorCls { }
public static class Mute {
  // mutes every audio session of the given pids; returns how many sessions were newly muted
  public static int Apply(uint[] pids) { int n = 0; var set = new HashSet<uint>(pids);
    var en = (IMMDeviceEnumerator)(new MMDeviceEnumeratorCls()); IMMDeviceCollection col; en.EnumAudioEndpoints(0, 1, out col);
    int cnt; col.GetCount(out cnt);
    for (int i = 0; i < cnt; i++) { IMMDevice d; col.Item(i, out d); object o; Guid iid = typeof(IAudioSessionManager2).GUID;
      if (d.Activate(ref iid, 23, IntPtr.Zero, out o) != 0) continue; var mgr = (IAudioSessionManager2)o;
      IAudioSessionEnumerator se; if (mgr.GetSessionEnumerator(out se) != 0) continue; int sc; se.GetCount(out sc);
      for (int j = 0; j < sc; j++) { object s; se.GetSession(j, out s); var c2 = (IAudioSessionControl2)s; uint pid; c2.GetProcessId(out pid);
        if (!set.Contains(pid)) continue; var v = (ISimpleAudioVolume)s; bool m; v.GetMute(out m);
        if (!m) { Guid g = Guid.Empty; v.SetMute(true, ref g); n++; } } }
    return n; }
}
"@
$log = "$env:TEMP\mafiacoop-onepc\keeper.log"
while ($true) {
  if (Test-Path $StopFile) { break }
  $procs = @(Get-Process mafiadefinitiveedition -ErrorAction SilentlyContinue | Sort-Object StartTime)
  $i = 0
  foreach ($p in $procs) {
    if ($i -ge 2) { break }
    $h = [GW]::Find([uint32]$p.Id)
    if ($h -ne [IntPtr]::Zero) {
      if ([GW]::Place($h, $X, $i * $GuestY, $W, $H)) { Add-Content $log "$(Get-Date -f HH:mm:ss.fff) placed pid $($p.Id) slot $i" }
    }
    $i++
  }
  if ($procs.Count -gt 0) {
    try { $m = [Mute]::Apply([uint32[]]($procs | ForEach-Object { $_.Id })); if ($m -gt 0) { Add-Content $log "$(Get-Date -f HH:mm:ss.fff) muted $m session(s)" } }
    catch { Add-Content $log "mute error: $_" }
  }
  if ($Once) { break }
  Start-Sleep -Milliseconds 250
}
