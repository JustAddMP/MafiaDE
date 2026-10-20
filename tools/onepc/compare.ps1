# One-PC test, step 4: the run's newest host and guest traces through sync_compare --check, plus the log lines that
# must not be there. Output: $Work\compare.txt.
. "$PSScriptRoot\common.ps1"
$host_ = Get-ChildItem "$Out\logs\trace\mafiacoop-host-*.trace" | Sort-Object LastWriteTime | Select-Object -Last 1
$guest = Get-ChildItem "$Out\logs\trace\mafiacoop-guest-*.trace" | Sort-Object LastWriteTime | Select-Object -Last 1
python -I "$Repo\tools\sync_compare.py" $host_.FullName $guest.FullName --check | Set-Content "$Work\compare.txt"
foreach ($tag in "host", "guest") {
  $log = "$Out\logs\mafiacoop-$tag.log"
  foreach ($p in "cars error", "did not load in", "is gone \(removed by the game", "spawner is stuck", "rides bike",
      "bike copy ready", "put at its player's spot", "sits in car", "savesys: written: (garage|my2k|profile_data|.slot)", "savesys: written: save", "kept out of the player's profile", "save write #\d+ kept out",
      "has no driver \(for 1 s", "is driven again: our crew gets back in", "keeps leaving seat", "the game streams drivers out") {
    Add-Content "$Work\compare.txt" ("{0,-6} {1,-40} {2}" -f $tag, $p, @(Select-String -Path $log -Pattern $p).Count)
  }
}
# guest_world (docs/host-authority.md WP-A): guest: "are off" >= 1 per world, "is back" 1 at the end, no fallback
# line unless the budget call failed; host: "we host: our own world stays on" once per world
foreach ($tag in "host", "guest") {
  $log = "$Out\logs\mafiacoop-$tag.log"
  foreach ($p in "guest world: our own traffic, police and pedestrians are off", "guest world: our own world is back",
      "SetMaxHumanElements failed", "guest world: we host: our own world stays on", "in the world, still shown") {
    Add-Content "$Work\compare.txt" ("{0,-6} {1,-40} {2}" -f $tag, $p, @(Select-String -Path $log -Pattern $p).Count)
  }
}
Get-Content "$Work\compare.txt" | Select-String "FAIL|PASS|shown:|removed by the game|puppet spawns|height|^(host|guest) "
Get-Content "$Work\run.log" -ErrorAction SilentlyContinue | Select-String "host garage|changed by the run|saves: "
