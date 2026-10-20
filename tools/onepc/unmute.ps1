# Unmutes every audio session of the running Mafia DE process (Windows remembers the per-app mute from the test).
$src = Get-Content "$PSScriptRoot\keeper.ps1" -Raw
$cs = $src.Substring($src.IndexOf('@"') + 2, $src.IndexOf('"@') - $src.IndexOf('@"') - 2)
$cs = $cs.Replace('if (!m) { Guid g = Guid.Empty; v.SetMute(true, ref g); n++; }', 'if (m) { Guid g = Guid.Empty; v.SetMute(false, ref g); n++; }')
$cs = $cs.Replace('public static class Mute', 'public static class UnmuteD').Replace('public static class GW', 'public static class GWD')
Add-Type -TypeDefinition $cs
$pids = [uint32[]](Get-Process mafiadefinitiveedition | ForEach-Object { $_.Id })
"unmuted sessions: " + [UnmuteD]::Apply($pids)
