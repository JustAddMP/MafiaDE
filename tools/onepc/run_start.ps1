# One-PC test, step 1: back up the saves, start the keeper, both games (host, then guest joining 127.0.0.1) and get
# them into a co-op Free Ride through the lobby. See tools/onepc/README.md. Needs a build in out\.
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
Start-Process powershell -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "$PSScriptRoot\keeper.ps1", "-X", $AreaX, "-StopFile", "$Work\keeper.stop" -WindowStyle Hidden | Out-Null
Start-Sleep 1

function Launch([string]$role) {
  # the window's first place and client size come from Saves\videoconfig.cfg (x y w h ...; restored afterwards)
  $y = if ($role -eq "guest") { $GuestY } else { 0 }
  [IO.File]::WriteAllText("$GameDocs\Saves\videoconfig.cfg", "$AreaX $y 1280 720 1 0 0 0 0")
  $a = @("--log", $role, "--debug-overlay", "--test_input", "1", "--test_pad", "1")
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
for ($k = 0; $k -lt 4 -and -not (LogHas "host" "first script tick"); $k++) { Say (Keys 0 @("ENTER")); Start-Sleep 4 }
for ($k = 0; $k -lt 4 -and -not (LogHas "guest" "lobby panel opened"); $k++) { Say (Keys 1 @("ENTER")); Start-Sleep 4 }
Set-Content "$Out\dev\guest.input.txt" "pad X`nwait 500`ndump"  # X = Ready (the lobby now has an OUTFIT row: DOWN + A hit it)
Start-Sleep 3
Set-Content "$Out\dev\host.input.txt" "open lobby`nwait 800`npad X`nwait 500"
for ($k = 0; $k -lt 20 -and -not (LogHas "host" "lobby start: FREE RIDE"); $k++) { Start-Sleep 1 }
Say "lobby start: $(LogHas 'host' 'lobby start: FREE RIDE')"

# 4. the loading screens wait for a key ("Continue"); a "SAVING FAILED" box on one of them takes one more Enter
for ($k = 0; $k -lt 12; $k++) {
  $h = LogHas "host" "local player in world"; $g = LogHas "guest" "local player in world"
  if ($h -and $g) { break }
  Start-Sleep 8
  if (-not $h) { Say (Keys 0 @("ENTER")) }
  if (-not $g) { Say (Keys 1 @("ENTER")) }
}
Say "in the world: host $(LogHas 'host' 'local player in world'), guest $(LogHas 'guest' 'local player in world')"
Say (Fit 0); Say (Fit 1)
