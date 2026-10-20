# One-PC test, step 3: stop the keeper (it would mute again), unmute both games (Windows remembers a per-app mute),
# close them, put the saves back and check every file against the hashes taken by run_start.ps1.
. "$PSScriptRoot\common.ps1"
New-Item -ItemType File "$Work\keeper.stop" -Force | Out-Null
Start-Sleep 2
for ($k = 0; $k -lt 3; $k++) { Say (& "$PSScriptRoot\unmute.ps1"); Start-Sleep 1 }  # the last one must say 0
Stop-Process -Name mafiadefinitiveedition -Force -ErrorAction SilentlyContinue
Start-Sleep 3
if (Games) { Say "a game is still running"; exit 1 }
$bak = Join-Path $Work "save-backup"
$bad = 0
foreach ($line in Get-Content "$Work\save-hashes.txt") {
  $hash, $rel = $line.Split(" ", 2)
  $now = Join-Path $GameDocs $rel
  if (-not (Test-Path $now) -or (Get-FileHash $now -Algorithm SHA256).Hash -ne $hash) {
    Copy-Item -Force (Join-Path $bak $rel) $now
    Say "restored $rel"
  }
  if ((Get-FileHash $now -Algorithm SHA256).Hash -ne $hash) { $bad++; Say "STILL DIFFERENT: $rel" }
}
$extra = Get-ChildItem -Recurse -File $GameDocs | Where-Object { -not (Test-Path (Join-Path $bak $_.FullName.Substring($GameDocs.Length))) }
foreach ($e in $extra) { Say "a new file the test made (left alone, check it): $($e.FullName)" }
Say "saves: $bad file(s) differ from the backup"
