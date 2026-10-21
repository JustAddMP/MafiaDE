# One-PC test, step 3: stop the keeper (it would mute again), unmute both games (Windows remembers a per-app mute),
# close them, put the saves back and check every file against the hashes taken by run_start.ps1. Before putting anything
# back it counts what the run changed: a save or profile file (gamesaves\..., profiles\...\*.pf other than the
# settings) must not change in a co-op run, for the host or the guest (both games use the same profile folder on one
# PC): "profile and save files changed by the run: 0". Settings (gamesettings.pf, key bindings, language, audio, video,
# map filter, Saves\videoconfig.cfg which run_start.ps1 itself writes) may change. The games are closed the
# normal way (below), so each writes its session bundle.
. "$PSScriptRoot\common.ps1"
$Settings = @("gamesettings.pf", "gameinputmodule.pf", "profile_language.pf", "profile_audiosettings.pf",
  "profile_videosettings.pf", "map_filter.pf", "videoconfig.cfg", "launcconfig.cfg")
function IsSaveOrProfile([string]$rel) {
  $r = $rel.ToLower(); $name = Split-Path $r -Leaf
  (($r -match "\\gamesaves\\") -or ($r -match "\\profiles\\.*\.pf$")) -and ($Settings -notcontains $name)
}
New-Item -ItemType File "$Work\keeper.stop" -Force | Out-Null
Start-Sleep 2
for ($k = 0; $k -lt 3; $k++) { Say (& "$PSScriptRoot\unmute.ps1"); Start-Sleep 1 }  # the last one must say 0
# closed the normal way (WM_CLOSE: the game quits, ExitProcess, the dll ends the session and writes its bundle,
# logs\bundles\<mod>-<stamp>-<role>.zip); killed only when one is still running after 60 s (then its bundle is a
# -recovered one at the next start)
foreach ($g in (Games)) { $null = $g.CloseMainWindow() }
for ($k = 0; $k -lt 60 -and (Games); $k++) { Start-Sleep 1 }
if (Games) { Say "a game did not close within 60 s: killed (no session bundle for it)"; Stop-Process -Name mafiadefinitiveedition -Force -ErrorAction SilentlyContinue; Start-Sleep 3 }
else { Say "games closed normally" }
$since = (Get-Date).AddMinutes(-3)
for ($k = 0; $k -lt 20; $k++) { $z = @(Get-ChildItem "$Out\logs\bundles\*.zip" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $since -and $_.Name -notmatch 'recovered' }); if ($z.Count -ge 2) { break }; Start-Sleep 1 }
Say "session bundles written at the close: $($z.Count) ($(($z | ForEach-Object { $_.Name }) -join ', '))"
if (Games) { Say "a game is still running"; exit 1 }
$bak = Join-Path $Work "save-backup"
$bad = 0
$changed = 0
foreach ($line in Get-Content "$Work\save-hashes.txt") {
  $hash, $rel = $line.Split(" ", 2)
  $now = Join-Path $GameDocs $rel
  if (-not (Test-Path $now) -or (Get-FileHash $now -Algorithm SHA256).Hash -ne $hash) {
    if (IsSaveOrProfile $rel) { $changed++; Say "CHANGED BY THE RUN (a save or profile file): $rel" }
    Copy-Item -Force (Join-Path $bak $rel) $now
    Say "restored $rel"
  }
  if ((Get-FileHash $now -Algorithm SHA256).Hash -ne $hash) { $bad++; Say "STILL DIFFERENT: $rel" }
}
$extra = Get-ChildItem -Recurse -File $GameDocs | Where-Object { -not (Test-Path (Join-Path $bak $_.FullName.Substring($GameDocs.Length))) }
foreach ($e in $extra) {
  if (IsSaveOrProfile $e.FullName.Substring($GameDocs.Length)) { $changed++ }
  Say "a new file the test made (left alone, check it): $($e.FullName)"
}
Say "profile and save files changed by the run: $changed (must be 0: garage.pf, aslot.pf, save*.sav, ...)"
Say "saves: $bad file(s) differ from the backup"
