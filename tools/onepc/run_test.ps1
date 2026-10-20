# One-PC test, step 2 (both games in a co-op Free Ride, run_start.ps1): the 2026-10-20 checks. Screenshots of the
# game windows only go to $Work\shots, the run's notes to $Work\run.log. See tools/onepc/README.md for what to expect.
. "$PSScriptRoot\common.ps1"
function Both([string]$name) { Lua 0 "$PSScriptRoot\face.lua" | Out-Null; Lua 1 "$PSScriptRoot\face.lua" | Out-Null
  Start-Sleep 1; Say "host: $(Lua 0 "$PSScriptRoot\face.lua")"; Say "guest: $(Lua 1 "$PSScriptRoot\face.lua")"
  Say (Shot 0 $name); Say (Shot 1 $name) }
Say (Fit 0); Say (Fit 1)
Start-Sleep 5
Both "01-spawn"

# on foot: each walks 3 s (W held in its own window), the other must see him follow
Say (Keys 1 @("W") 3000); Start-Sleep 2; Both "02-guest-walked"
Say (Keys 0 @("W") 3000); Start-Sleep 2; Both "03-host-walked"

# a fall: 4 m up, he drops; his puppet must fall with him once (no "put at its player's spot" loop)
Say "host falls: $(Lua 0 "$PSScriptRoot\fall.lua")"; Start-Sleep 4; Both "04-host-fell"
Say "guest falls: $(Lua 1 "$PSScriptRoot\fall.lua")"; Start-Sleep 4; Both "05-guest-fell"

# a motorbike (the 0.16.0 "cars error"): the host gets on one, rides 4 s, the guest must see a bike with his puppet on it
Say "host bike: $(Lua 0 "$PSScriptRoot\bike_spawn.lua")"; Start-Sleep 10
Say (Keys 0 @("W") 4000); Start-Sleep 1; Both "06-host-on-bike"
Say (Keys 0 @("E") 300); Start-Sleep 4; Both "07-host-off-bike"

# the host's Free Ride garage (2026-10-20: the host's garage.pf was written for real at each garage visit): a car added
# by script must give a garage.pf write that is kept out of the profile (run_stop.ps1 then finds garage.pf unchanged)
Say "host garage: $(Lua 0 "$PSScriptRoot\garage_take.lua")"; Start-Sleep 3
$hl = "$Out\logs\mafiacoop-host.log"
Say ("host garage.pf: kept out {0}, written for real {1}" -f @(Select-String -Path $hl -Pattern "kept out of the player's profile \(progress: the cars").Count,
  @(Select-String -Path $hl -Pattern "savesys: written: garage.pf").Count)

# a car: the guest gets into one and drives 4 s
Say "guest car: $(Lua 1 "$PSScriptRoot\car_spawn.lua")"; Start-Sleep 10
Say (Keys 1 @("W") 4000); Start-Sleep 1; Both "08-guest-in-car"
Say (Keys 1 @("F") 300); Start-Sleep 5; Both "09-guest-out-of-car"

# a character change in the world (pause CO-OP panel, CHARACTER one step right), then close the panel
Set-Content "$Out\dev\guest.input.txt" "open pause`nwait 800`ndump`npad RIGHT`nwait 1500`npad B`nwait 500"
Start-Sleep 8; Both "10-guest-new-character"

# the game removes a puppet and its spawner hangs (the field case after the Free Ride garage): back within ~5 s
Say "loss on the guest: $(Lua 1 "$PSScriptRoot\losstest.lua")"; Start-Sleep 10; Both "11-after-loss"

# a minute side by side with traffic and pedestrians around
Start-Sleep 60; Both "12-a-minute-later"
Say "done: now run_stop.ps1, then compare.ps1"
