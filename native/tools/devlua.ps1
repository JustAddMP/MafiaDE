# Runs Lua inside a running game through the dll's dev console and prints the result.
#   tools\devlua.ps1 -Tag host -Code 'return game.game:GetActivePlayer():GetPos()'
#   tools\devlua.ps1 -Tag guest -File my_probe.lua
# The dll polls out\dev\<tag>.in.lua twice a second (tag = the --log tag of that game, "default" without one),
# runs it on GameMainThread, deletes it and writes out\dev\<tag>.out.txt ("ok"/"error" line, then the result).
param(
    [string]$Tag = 'default',
    [string]$Code,
    [string]$File,
    [int]$TimeoutS = 20
)
$ErrorActionPreference = 'Stop'
$dev = Join-Path (Join-Path $PSScriptRoot '..\out') 'dev'
New-Item -ItemType Directory -Force $dev | Out-Null
$in = Join-Path $dev "$Tag.in.lua"
$out = Join-Path $dev "$Tag.out.txt"
if ($File) { $Code = [IO.File]::ReadAllText((Resolve-Path $File)) }
if (-not $Code) { throw 'pass -Code or -File' }
Remove-Item $out -ErrorAction SilentlyContinue
[IO.File]::WriteAllText("$in.tmp", $Code)
Move-Item -Force "$in.tmp" $in
$deadline = (Get-Date).AddSeconds($TimeoutS)
while ((Get-Date) -lt $deadline) {
    if (Test-Path $out) { Get-Content -Raw $out; exit 0 }
    Start-Sleep -Milliseconds 200
}
Write-Error "no answer within $TimeoutS s (is the game in a state that ticks scripts? see README-dev.md)"
exit 1
