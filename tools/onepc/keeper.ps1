# Keeps the two test games' windows inside the test area (host top, guest below, 1296x759; a window the game made
# bigger is shrunk at once) without activating them, and mutes their audio sessions, until the stop file exists. Stop
# it (run_stop.ps1) BEFORE unmuting, or it mutes again.
# Two ways at once: a WinEvent hook (SetWinEventHook, out of context) sees every top-level window of the game and of
# MafiaCoop.exe the moment it is created, shown or moved and puts one outside x X..R inside at once (a window not
# shown yet is moved before anyone sees it); and a poll of all their windows every 50 ms (the first 60 s and while no
# game runs, then every 250 ms) as the fallback. Every window seen outside the strip is logged in keeper.log
# ("OUTSIDE ... was ... (VISIBLE / not shown yet)", then "inside again N ms after ..."), and the totals at the end.
param([string]$StopFile = "$env:TEMP\mafiacoop-onepc\keeper.stop", [int]$X = 2916, [int]$R = 4233, [int]$W = 1317, [int]$H = 696, [int]$GuestY = 633, [switch]$Once)
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
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
using System.Diagnostics; using System.Linq; using System.Threading; using System.IO;
public static class WinKeep {
  public delegate void WinEventDelegate(IntPtr hook, uint ev, IntPtr hwnd, int idObject, int idChild, uint thread, uint time);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern IntPtr SetWinEventHook(uint min, uint max, IntPtr mod, WinEventDelegate cb, uint pid, uint tid, uint flags);
  [DllImport("user32.dll")] static extern int GetMessage(out MSG m, IntPtr h, uint a, uint b);
  [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG m);
  [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref MSG m);
  [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint f);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RC r);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint f);
  [DllImport("user32.dll")] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("kernel32.dll")] static extern uint GetTickCount();
  [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr h; public uint msg; public IntPtr w; public IntPtr l; public uint time; public int x; public int y; }
  public struct RC { public int L, T, R, B; }
  public static int X = 2916, Right = 4233, GuestY = 633; public static string LogPath;
  static WinEventDelegate keep; static readonly object lk = new object();
  static Dictionary<uint, string> names = new Dictionary<uint, string>();
  static Dictionary<IntPtr, uint> outAt = new Dictionary<IntPtr, uint>();  // hwnd -> tick first seen outside
  public static int Outside = 0;   // windows seen outside the strip while shown
  public static int Hidden = 0;    // windows moved in before they were shown
  static void Log(string s) { lock (lk) { try { File.AppendAllText(LogPath, DateTime.Now.ToString("HH:mm:ss.fff") + " " + s + "\r\n"); } catch { } } }
  static string NameOf(uint pid) { lock (lk) { string n; if (names.TryGetValue(pid, out n)) return n;
    try { n = Process.GetProcessById((int)pid).ProcessName.ToLowerInvariant(); } catch { n = ""; } names[pid] = n; return n; } }
  static bool Watched(uint pid) { string n = NameOf(pid); return n == "mafiadefinitiveedition" || n == "mafiacoop"; }
  static int SlotOf(uint pid) {
    try { var g = Process.GetProcessesByName("mafiadefinitiveedition").OrderBy(p => p.StartTime).Select(p => (uint)p.Id).ToList();
      int i = g.IndexOf(pid); return i < 0 ? 0 : Math.Min(i, 1); } catch { return 0; } }
  public static void Start() { var t = new Thread(Run); t.IsBackground = true; t.Start(); }
  static void Run() {
    keep = Cb;
    // EVENT_OBJECT_CREATE .. EVENT_OBJECT_LOCATIONCHANGE, every process, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    if (SetWinEventHook(0x8000, 0x800B, IntPtr.Zero, keep, 0, 0, 0x0000 | 0x0002) == IntPtr.Zero) Log("hook: SetWinEventHook failed");
    else Log("hook: on");
    MSG m; while (GetMessage(out m, IntPtr.Zero, 0, 0) > 0) { TranslateMessage(ref m); DispatchMessage(ref m); } }
  static void Cb(IntPtr hook, uint ev, IntPtr hwnd, int idObject, int idChild, uint thread, uint time) {
    if (idObject != 0 || idChild != 0 || hwnd == IntPtr.Zero) return;
    if (ev != 0x8000 && ev != 0x8002 && ev != 0x800B) return;  // create, show, location change
    try { if (GetAncestor(hwnd, 2) != hwnd) return; uint pid; GetWindowThreadProcessId(hwnd, out pid);
      if (!Watched(pid)) return; Fix(hwnd, pid, ev == 0x8000 ? "create" : ev == 0x8002 ? "show" : "move", time, 0); }
    catch (Exception e) { Log("hook error: " + e.Message); } }
  // One window: outside the strip (or wider than it) -> moved in (shrunk to 1296x759 when wider). True when moved.
  public static bool Fix(IntPtr h, uint pid, string why, uint evTick, int depth) {
    RC r; if (!GetWindowRect(h, out r)) return false;
    int w = r.R - r.L, hh = r.B - r.T;
    if (w <= 0 || hh <= 0 || IsIconic(h)) return false;
    bool vis = IsWindowVisible(h);
    bool outside = r.L < X || r.R > Right;
    uint now = GetTickCount();
    if (!outside) {
      uint t0; bool was; lock (lk) { was = outAt.TryGetValue(h, out t0); if (was) outAt.Remove(h); }
      if (was) Log(string.Format("window {0:X} pid {1}: inside again {2} ms after it was first seen outside ({3},{4}-{5},{6}, {7})",
        h.ToInt64(), pid, now - t0, r.L, r.T, r.R, r.B, why));
      return false; }
    var sb = new StringBuilder(64); GetClassName(h, sb, 64);
    bool first;
    lock (lk) { first = !outAt.ContainsKey(h); if (first) { outAt[h] = evTick != 0 ? evTick : now; if (vis) Outside++; else Hidden++; } }
    if (depth > 2) { Log(string.Format("window {0:X} pid {1}: could not be moved inside ({2},{3}-{4},{5})", h.ToInt64(), pid, r.L, r.T, r.R, r.B)); return false; }
    int y = SlotOf(pid) * GuestY;
    bool big = w > Right - X;
    uint flags = 0x0004 | 0x0010 | (big ? 0u : 0x0001u);  // NOZORDER | NOACTIVATE (| NOSIZE)
    SetWindowPos(h, IntPtr.Zero, X, y, big ? 1296 : 0, big ? 759 : 0, flags);
    RC n; GetWindowRect(h, out n);
    bool inside = n.L >= X && n.R <= Right;
    Log(string.Format("OUTSIDE window {0:X} pid {1} ({2}) class {3} on {4}: was {5},{6}-{7},{8} ({9}), moved to {10},{11}-{12},{13}{14}",
      h.ToInt64(), pid, NameOf(pid), sb, why, r.L, r.T, r.R, r.B, vis ? "VISIBLE" : "not shown yet", n.L, n.T, n.R, n.B,
      inside ? "" : " - STILL OUTSIDE"));
    if (inside) Fix(h, pid, "after the move", 0, depth + 1); else Fix(h, pid, "retry", 0, depth + 1);
    return true; }
  // the poll: every top-level window of the watched processes
  public static void Poll() {
    EnumWindows((h, l) => { uint pid; GetWindowThreadProcessId(h, out pid); if (Watched(pid)) Fix(h, pid, "poll", 0, 0); return true; }, IntPtr.Zero); }
}
"@
$log = "$env:TEMP\mafiacoop-onepc\keeper.log"
[WinKeep]::X = $X; [WinKeep]::Right = $R; [WinKeep]::GuestY = $GuestY; [WinKeep]::LogPath = $log
if (-not $Once) { [WinKeep]::Start() }
$t0 = Get-Date
while ($true) {
  if (Test-Path $StopFile) { break }
  [WinKeep]::Poll()
  $procs = @(Get-Process mafiadefinitiveedition -ErrorAction SilentlyContinue | Sort-Object StartTime)
  $i = 0
  foreach ($p in $procs) {
    if ($i -ge 2) { break }
    $h = [GW]::Find([uint32]$p.Id)
    if ($h -ne [IntPtr]::Zero) {
      $r = New-Object GW+RECT; [GW]::GetWindowRect($h, [ref]$r) | Out-Null
      if ([GW]::Place($h, $X, $i * $GuestY, $W, $H)) { Add-Content $log "$(Get-Date -f HH:mm:ss.fff) placed pid $($p.Id) slot $i (was $($r.L),$($r.T)-$($r.R),$($r.B))" }
    }
    $i++
  }
  if ($procs.Count -gt 0) {
    try { $m = [Mute]::Apply([uint32[]]($procs | ForEach-Object { $_.Id })); if ($m -gt 0) { Add-Content $log "$(Get-Date -f HH:mm:ss.fff) muted $m session(s)" } }
    catch { Add-Content $log "mute error: $_" }
  }
  if ($Once) { break }
  Start-Sleep -Milliseconds $(if (((Get-Date) - $t0).TotalSeconds -lt 60 -or $procs.Count -eq 0) { 50 } else { 250 })
}
Add-Content $log "$(Get-Date -f HH:mm:ss.fff) keeper stopped; windows seen outside the strip while shown: $([WinKeep]::Outside), moved in before they were shown: $([WinKeep]::Hidden)"
