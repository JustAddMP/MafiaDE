# One-PC test, step 1: back up the saves, start the keeper, both games (host, then guest joining 127.0.0.1) and get
# them into a co-op Free Ride through the lobby. See tools/onepc/README.md. Needs a build in out\.
# -NetSim: the simulated lag both games get (core net/net_sim.h, --net-sim: what each game sends over UDP waits this
# long; 8 ms +-2 on both sides: a 20-30 ms round trip in game, the games add ~8 ms of their own); "off": none.
# -Chapter: a lobby CHAPTER instead of the Free Ride (its name as the MODE row shows it, e.g. "TAXI": CHAPTER 2: TAXI,
# mm_20_taxi from its own start save, never the player's story): the MODE row is stepped until it shows it, START only
# then.
param([string]$NetSim = "latency=8,jitter=2,loss=0,dup=0", [string]$Chapter = "")
. "$PSScriptRoot\common.ps1"
Remove-Item "$Work\run.log", "$Work\keeper.stop" -ErrorAction SilentlyContinue
if (Games) { Say "a game is already running: close it first"; exit 1 }

# 1. saves: a copy and its hashes (run_stop.ps1 puts back what differs and checks every hash)
$bak = Join-Path $Work "save-backup"
if (Test-Path $bak) { Remove-Item -Recurse -Force $bak }
Copy-Item -Recurse $GameDocs $bak
Get-ChildItem -Recurse -File $bak | ForEach-Object { "{0} {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.FullName.Substring($bak.Length) } |
  Set-Content "$Work\save-hashes.txt"
Say "saves backed up: $((Get-Content "$Work\save-hashes.txt").Count) files"

# 2. the keeper first, so a window is moved / shrunk the moment it appears
Remove-Item "$Work\keeper.log" -ErrorAction SilentlyContinue
Start-Process powershell -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "$PSScriptRoot\keeper.ps1", "-X", $AreaX, "-R", $AreaR, "-StopFile", "$Work\keeper.stop" -WindowStyle Hidden | Out-Null
Start-Sleep 1

Say "simulated lag: $NetSim"
function Launch([string]$role) {
  # the window's place and client size come from Saves\videoconfig.cfg (x y w h fullscreen ...; restored afterwards):
  # windowed (the fifth number 0; 1 made it a fullscreen window over the whole primary monitor for ~0.3 s, the
  # 2026-10-20 22:00 run's "0,0-1280,720"); --window-area makes the game's first window (its 480x481 start window,
  # which comes up in the middle of the primary monitor) inside the strip from the start (core dll/window_place.h)
  $y = if ($role -eq "guest") { $GuestY } else { 0 }
  [IO.File]::WriteAllText("$GameDocs\Saves\videoconfig.cfg", "$AreaX $y 1280 720 0 0 0 0 0")
  $a = @("--log", $role, "--debug-overlay", "--test_input", "1", "--test_pad", "1", "--window-area", "$AreaX,$AreaR,$y")
  if ($NetSim -and $NetSim -ne "off") { $a += @("--net-sim", $NetSim) }
  $a = if ($role -eq "host") { @("--host") + $a } else { @("--join", "127.0.0.1") + $a }
  Start-Process -FilePath "$Out\MafiaCoop.exe" -ArgumentList $a -WorkingDirectory $Out | Out-Null
  Say "started $role"
}
function WaitWindow([int]$i) {
  for ($k = 0; $k -lt 120; $k++) { $h = GameWindow $i; if ($h -ne [IntPtr]::Zero) { break }; Start-Sleep -Milliseconds 250 }
  $r = Rect $h; Say "game $i window first seen at $($r.L),$($r.T)-$($r.R),$($r.B)"
  Start-Sleep 3; Say (Fit $i)
}
Launch "host"; WaitWindow 0
for ($k = 0; $k -lt 60 -and -not (LogHas "host" "hosting over"); $k++) { Start-Sleep 1 }
Launch "guest"; WaitWindow 1
for ($k = 0; $k -lt 90 -and -not (LogHas "guest" "handshake complete"); $k++) { Start-Sleep 1 }
if (-not (LogHas "guest" "handshake complete")) { Say "the guest did not connect"; exit 1 }
Say "connected"

# 3. past the title screens (real Enter to the window in front), then the lobby: guest READY, host START FREE RIDE
Start-Sleep 8
# Enter only until the game's scripts run (its main menu is up): an Enter at the main menu is CONTINUE, the player's
# own story (the 2026-10-21 01:23 run: the guest's lobby panel came late, two more Enters loaded his story checkpoint)
for ($k = 0; $k -lt 8 -and -not (LogHas "host" "first script tick"); $k++) { Say (Keys 0 @("ENTER")); Start-Sleep 4 }
for ($k = 0; $k -lt 8 -and -not (LogHas "guest" "first script tick"); $k++) { Say (Keys 1 @("ENTER")); Start-Sleep 4 }
for ($k = 0; $k -lt 15 -and -not (LogHas "guest" "lobby panel opened"); $k++) { Start-Sleep 1 }
if (-not (LogHas "guest" "lobby panel opened")) {
  Set-Content "$Out\dev\guest.input.txt" "open lobby`nwait 800"; Start-Sleep 3
  Say "guest lobby panel: opened by test_input ($(LogHas 'guest' 'lobby panel opened'))"
}
if (LogHas "guest" "local player in world") { Say "the guest is in a world before the lobby started: stop (run_stop.ps1)"; exit 1 }
# the main menu movie must have been patched (the CO-OP entry; the lobby's FREE RIDE start goes through its poll): a
# game whose menu hooks came too late stays at its menu (the 2026-10-21 03:51 run: "the main menu last polled never")
foreach ($tag in "host", "guest") {
  if (-not (LogHas $tag "patched /gui/main_menu_main")) { Say "the $tag's main menu was not patched (its menu hooks came too late): stop (run_stop.ps1)"; exit 1 }
}
Set-Content "$Out\dev\guest.input.txt" "pad X`nwait 500`ndump"  # X = Ready (the lobby now has an OUTFIT row: DOWN + A hit it)
Start-Sleep 3
# the host's window in front first: a windowed game's main menu does not tick while another window has the focus, and
# the Free Ride start waits for it (menu.cpp AtMainMenu; the 2026-10-21 01:38 run: "lobby start" but no load)
$hw = GameWindow 0
if ($hw -ne [IntPtr]::Zero) {
  [OnePc]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [OnePc]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
  [OnePc]::SetForegroundWindow($hw) | Out-Null; Start-Sleep 1
}
if ($Chapter) {
  # the MODE row stepped right until it shows the chapter (dumped and checked at every step; never STORY at START)
  $rows = PanelRows "host" "open lobby"
  $mode = $rows | Where-Object { $_.label -like "MODE =*" } | Select-Object -First 1
  $cur = $rows | Where-Object { $_.sel } | Select-Object -First 1
  if (-not $mode) { Say "no MODE row in the host's lobby: stop (run_stop.ps1)"; exit 1 }
  $d = $mode.i - $(if ($cur) { $cur.i } else { 0 })
  $nav = @(); for ($k = 0; $k -lt [Math]::Abs($d); $k++) { $nav += $(if ($d -gt 0) { "pad DOWN" } else { "pad UP" }); $nav += "wait 250" }
  if ($nav) { Set-Content "$Out\dev\host.input.txt" ($nav -join "`n"); Start-Sleep 2 }
  for ($k = 0; $k -lt 25; $k++) {
    $rows = PanelRows "host" "wait 100"
    $mode = $rows | Where-Object { $_.label -like "MODE =*" } | Select-Object -First 1
    if ($mode -and $mode.label -like "*$Chapter*") { break }
    Set-Content "$Out\dev\host.input.txt" "pad RIGHT`nwait 300"; Start-Sleep 1
  }
  $start = $rows | Where-Object { $_.label -like "START*" } | Select-Object -First 1
  Say "host lobby: $($mode.label) / $($start.label)"
  if (-not ($mode -and $mode.label -like "*$Chapter*" -and $mode.label -notlike "*STORY*" -and $start.label -like "START CHAPTER*$Chapter*")) {
    Say "the host's lobby does not show the chapter ${Chapter}: stop (run_stop.ps1)"; exit 1
  }
  Say "host start: $(PickRow 'host' $rows 'START CHAPTER')"
  for ($k = 0; $k -lt 20 -and -not (LogHas "host" "lobby start: CHAPTER"); $k++) { Start-Sleep 1 }
  Say "lobby start: $(LogHas 'host' 'lobby start: CHAPTER') ($(@(Select-String -Path "$Out\logs\mafiacoop-host.log" -Pattern 'mission: loading' | Select-Object -Last 1).Line))"
} else {
  Set-Content "$Out\dev\host.input.txt" "open lobby`nwait 800`npad X`nwait 500"
  for ($k = 0; $k -lt 20 -and -not (LogHas "host" "lobby start: FREE RIDE"); $k++) { Start-Sleep 1 }
  Say "lobby start: $(LogHas 'host' 'lobby start: FREE RIDE')"
}
# the host's main menu must take the start ("menu: starting 'free_ride'"); if it does not, an Enter below would be
# CONTINUE on his own story (the 2026-10-21 01:28 run): stop instead, with a screenshot of each window
for ($k = 0; $k -lt 10 -and -not (LogHas "host" "menu: starting 'free_ride'"); $k++) { Start-Sleep 1 }
if (-not (LogHas "host" "menu: starting 'free_ride'")) {
  Say (Shot 0 "00-no-free-ride"); Say (Shot 1 "00-no-free-ride")
  Say "the host's main menu did not start the Free Ride: stop (run_stop.ps1)"; exit 1
}
# the guest's main menu takes the same start only while his window is in front (the 2026-10-21 02:12 run: it waited,
# the Enter below was CONTINUE and loaded his story checkpoint 500 m away)
$gw = GameWindow 1
if ($gw -ne [IntPtr]::Zero) {
  [OnePc]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [OnePc]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
  [OnePc]::SetForegroundWindow($gw) | Out-Null
}
for ($k = 0; $k -lt 15 -and -not (LogHas "guest" "menu: starting 'free_ride'"); $k++) { Start-Sleep 1 }
if (-not (LogHas "guest" "menu: starting 'free_ride'")) {
  Say (Shot 1 "00-guest-no-free-ride")
  Say "the guest's main menu did not start the Free Ride: stop (run_stop.ps1)"; exit 1
}

# 4. the loading screens wait for a key ("Continue"); a "SAVING FAILED" box on one of them takes one more Enter
for ($k = 0; $k -lt 12; $k++) {
  $h = LogHas "host" "local player in world"; $g = LogHas "guest" "local player in world"
  if ($h -and $g) { break }
  Start-Sleep 8
  if (-not $h) { Say (Keys 0 @("ENTER")) }
  if (-not $g) { Say (Keys 1 @("ENTER")) }
}
Say "in the world: host $(LogHas 'host' 'local player in world'), guest $(LogHas 'guest' 'local player in world')"
if ($Chapter) {
  # a chapter's guest loads the host's checkpoint once more after its start (MissionSync: "following the host: loading
  # his checkpoint"), and that loading screen waits for a key too (the 07:43 run: 50 s on it). Only co-op loads here
  # (mission: loading ... through the chapter loader), never the story: Enter while its loading screen is up, 40 s.
  for ($k = 0; $k -lt 8; $k++) {
    Start-Sleep 5
    foreach ($i in 0, 1) {
      $tag = @('host', 'guest')[$i]
      $ans = & "$Repo\tools\devlua.ps1" -Tag $tag -Code "local g, up = pcall(game.hud.IsLoadingScreenUp, game.hud) return (g and up) and 'loading' or 'in world'" -TimeoutS 4 2>$null
      if ("$ans" -notmatch "in world") { Say "$tag on a loading screen after the chapter start (a follow load): $(Keys $i @('ENTER'))" }
    }
  }
}
Say (Fit 0); Say (Fit 1)
