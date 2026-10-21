# One-PC test, the longer scenarios of docs/host-authority.md section 6 that run_test.ps1 does not cover, scripted so
# they repeat: run after run_test.ps1 (both games in the co-op Free Ride, run_start.ps1), before run_stop.ps1.
#   drive     each drives his own car (the guest's car copy on the host, the host's on the guest)
#   ride      ride along both ways: the guest beside the host (front seat), then behind him (back seat), then the host
#             beside and behind the guest; the other one drives meanwhile
#   leashcar  the leash with the guest at the wheel: 450 m away in his car, the countdown, the pull (road / beside)
#   down      the guest far from the host (200 m) hurt down to the downed health: downed, respawned next to the host
#   rejoin    the guest leaves the co-op (pause CO-OP panel: LEAVE CO-OP), goes to the main menu
#             (game.hud:QuitToMainMenu), joins again by IP and joins the host's game from the lobby
#   crime     the guest becomes wanted near the host (crime.lua: his own police zone, HUNT): the host's police take
#             him on (PoliceChase), the guest's game spawns no police of its own
# -Steps picks some (comma separated, in this order). Screenshots of the game windows only ($Work\shots\2x-...), notes
# in $Work\run.log. See tools/onepc/README.md "run_more.ps1" for what to look for.
param([string]$Steps = "drive,ride,leashcar,down,rejoin,crime")
. "$PSScriptRoot\common.ps1"
$want = $Steps -split ","
function Step([string]$n) { $want -contains $n }
function Both([string]$name) { Lua 0 "$PSScriptRoot\face.lua" | Out-Null; Lua 1 "$PSScriptRoot\face.lua" | Out-Null
  Start-Sleep 1; Say "host: $(Lua 0 "$PSScriptRoot\face.lua")"; Say "guest: $(Lua 1 "$PSScriptRoot\face.lua")"
  Say (Shot 0 $name); Say (Shot 1 $name) }
function Seats([string]$what) { Say "$what host sees: $(Lua 0 "$PSScriptRoot\seat_probe.lua")"; Say "$what guest sees: $(Lua 1 "$PSScriptRoot\seat_probe.lua")" }
function Count([string]$tag, [string]$pattern) { @(Select-String -Path "$Out\logs\mafiacoop-$tag.log" -Pattern $pattern).Count }
Say (Fit 0); Say (Fit 1)
# four-seat cars (the ride along takes the back seat too), one after the other: two cars of one model spawned at the
# same moment 3 m apart are taken for one car (player_sync.lua M.FindLocalCar adopts a free car of that model)
foreach ($i in 0, 1) { Lua $i -code "MafiaCoop.tcarModel = 'shubert_e_six_taxi'; return 'ok'" | Out-Null }

if (Step "drive") {
  # each in a car of his own, then each drives a few seconds (keys go to the window in front, one at a time)
  Say "host car: $(Lua 0 "$PSScriptRoot\car_spawn.lua")"; Start-Sleep 8; Say "guest car: $(Lua 1 "$PSScriptRoot\car_spawn.lua")"; Start-Sleep 10
  Seats "in their cars:"
  Say (Keys 0 @("W") 5000); Say (Keys 1 @("W") 5000); Say (Keys 0 @("W") 4000); Say (Keys 1 @("W") 4000)
  Start-Sleep 1; Both "20-drove"
}

if (Step "ride") {
  # the guest beside the host, then behind him: out of his car, into the host's (our copy of it), the host drives
  foreach ($seat in 1, 2) {
    Say (Keys 1 @("E") 300); Start-Sleep 5
    Lua 1 -code "MafiaCoop.rideSeat = $seat; return 'seat $seat'" | Out-Null
    Say "guest rides with the host (seat $seat): $(Lua 1 "$PSScriptRoot\ridealong.lua")"; Start-Sleep 5
    Seats "guest in the host's car seat ${seat}:"
    Say (Keys 0 @("W") 5000); Start-Sleep 1; Both "21-guest-rides-seat$seat"
    Seats "after the host drove (guest seat $seat):"
  }
  Say (Keys 1 @("E") 300); Start-Sleep 5
  # the host beside / behind the guest: the guest gets a new car of his own, the host leaves his and gets in
  Say "guest car: $(Lua 1 "$PSScriptRoot\car_spawn.lua")"; Start-Sleep 10
  Say (Keys 0 @("E") 300); Start-Sleep 5
  foreach ($seat in 1, 2) {
    Lua 0 -code "MafiaCoop.rideSeat = $seat; return 'seat $seat'" | Out-Null
    Say "host rides with the guest (seat $seat): $(Lua 0 "$PSScriptRoot\ridealong.lua")"; Start-Sleep 5
    Seats "host in the guest's car seat ${seat}:"
    Say (Keys 1 @("W") 5000); Start-Sleep 1; Both "22-host-rides-seat$seat"
    Seats "after the guest drove (host seat $seat):"
    Say (Keys 0 @("E") 300); Start-Sleep 5
  }
}

if (Step "leashcar") {
  # the guest at the wheel (his car from the ride step, else a new one): 450 m away with his car, pulled back in it
  if (-not ((Lua 1 "$PSScriptRoot\seat_probe.lua") -match "we: car \S+ seat 0")) {
    Say "guest car: $(Lua 1 "$PSScriptRoot\car_spawn.lua")"; Start-Sleep 10
  }
  $p0 = Count "guest" "leash: pulled back beside the host"
  Say "guest leash in a car: $(Lua 1 "$PSScriptRoot\leash_test.lua")"; Start-Sleep 3; Both "23-leash-car-countdown"
  Start-Sleep 13; Both "24-leash-car-pulled"
  Say "guest leash pulls in a car: $((Count 'guest' 'leash: pulled back beside the host') - $p0) ($(@(Select-String -Path "$Out\logs\mafiacoop-guest.log" -Pattern 'leash: pulled back beside the host' | Select-Object -Last 1).Line))"
  Seats "after the car pull:"
  Say (Keys 1 @("E") 300); Start-Sleep 5
}

if (Step "down") {
  $d0 = Count "guest" "respawned next to the host"
  Say "guest down far: $(Lua 1 "$PSScriptRoot\down_far.lua")"; Start-Sleep 6; Both "25-guest-down-far"
  Start-Sleep 8; Both "26-guest-respawned"
  Say "guest down: $(Lua 1 -code "local d = MafiaCoop.downTest or {}; return string.format('steps %s health %s', tostring(d.steps), tostring(d.h))")"
  Say ("guest downed {0}, respawned next to the host {1}, died {2}" -f (Count "guest" "we are down \(health"),
    ((Count "guest" "respawned next to the host") - $d0), (Count "guest" "we died"))
}

if (Step "rejoin") {
  # leave: the pause CO-OP panel's LEAVE CO-OP (the guest's window in front: our panels take its pad)
  Front 1
  $rows = PanelRows "guest" "open pause"
  Say "guest pause panel: $(($rows | ForEach-Object { $_.label }) -join ' | ')"
  Say "guest leave: $(PickRow 'guest' $rows 'LEAVE CO-OP')"; Start-Sleep 5
  Say "guest left: $(LogHas 'guest' 'coop: leave requested')"
  # to the main menu (the game's own quit; its quit save is kept out until the menu is up)
  Say "guest to the main menu: $(Lua 1 -code "return tostring(pcall(game.hud.QuitToMainMenu, game.hud))")"
  $polled = $false
  for ($k = 0; $k -lt 40; $k++) { Start-Sleep 1; Front 1; if ((Count "guest" "out of the co-op world \(main menu\)") -gt 0 -or $k -ge 25) { break } }
  Start-Sleep 5; Say (Shot 1 "27-guest-main-menu")
  # join again by IP: the box emptied (it holds the last address typed there, none after a --join start) and
  # 127.0.0.1 typed (core test input `text`), then CONNECT
  $j0 = Count "guest" "handshake complete"
  $rows = PanelRows "guest" ("open joinip`nwait 500`n" + ((1..24 | ForEach-Object { "key 0x08" }) -join "`n") + "`ntext 127.0.0.1")
  Say "guest join by IP: $(PickRow 'guest' $rows 'CONNECT')"
  for ($k = 0; $k -lt 20 -and (Count "guest" "handshake complete") -le $j0; $k++) { Start-Sleep 1 }
  Say "guest connected again: $((Count 'guest' 'handshake complete') -gt $j0)"
  Front 1; Start-Sleep 2
  $rows = PanelRows "guest" "open lobby"
  Say "guest lobby: $(($rows | ForEach-Object { $_.label }) -join ' | ')"
  $s0 = Count "guest" "menu: starting 'free_ride'"
  Say "guest join game: $(PickRow 'guest' $rows 'JOIN GAME')"
  for ($k = 0; $k -lt 15 -and (Count "guest" "menu: starting 'free_ride'") -le $s0; $k++) { Start-Sleep 1 }
  if ((Count "guest" "menu: starting 'free_ride'") -le $s0) {
    Say (Shot 1 "28-guest-no-join"); Say "the guest's main menu did not start the Free Ride: stop here (run_stop.ps1)"; exit 1
  }
  # its loading screen waits for a key (only now: the load is the Free Ride, never the story)
  $w0 = Count "guest" "local player in world"
  for ($k = 0; $k -lt 12 -and (Count "guest" "local player in world") -le $w0; $k++) { Start-Sleep 7; if ((Count "guest" "local player in world") -le $w0) { Say (Keys 1 @("ENTER")) } }
  Say "guest in the world again: $((Count 'guest' 'local player in world') -gt $w0)"; Say (Fit 1)
  Start-Sleep 10; Both "29-guest-rejoined"
  # a minute together after the rejoin (each walks a little): the second session's foot.* numbers
  Say (Keys 1 @("W") 3000); Start-Sleep 2; Say (Keys 0 @("W") 3000); Start-Sleep 45; Both "29b-a-minute-after-rejoin"
}

if (Step "crime") {
  $c0 = Count "host" "our police take him on"
  Say "guest crime: $(Lua 1 "$PSScriptRoot\crime.lua")"; Start-Sleep 10; Both "30-guest-wanted"
  Start-Sleep 20; Both "31-guest-chased"
  Say ("host police on the guest: {0}; guest chase lines {1}; guest own police shown {2}" -f ((Count "host" "our police take him on") - $c0),
    (Count "guest" "police chase|PoliceChase|the host's police"), (Count "guest" "own police"))
}
Say "run_more done: now run_stop.ps1, then compare.ps1"
