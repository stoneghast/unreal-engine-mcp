<#
.SYNOPSIS
  Install the UE5MCP bridge into a game's UE4SS Mods folder.
.PARAMETER GameDir
  Game install root: the folder that holds <Project>\Binaries\Win64\ue4ss\UE4SS.dll.
.PARAMETER Build
  Build directory holding <Config>\bin\UE5MCPBridge.dll (default build/full).
.PARAMETER Port
  Loopback port the bridge listens on (default 19777).
#>
param(
    [Parameter(Mandatory)] [string]$GameDir,
    [string]$Build = "build/full",
    [string]$Config = "Game__Shipping__Win64",
    [int]$Port = 19777,
    [switch]$NoDll
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$ue4ss = Get-ChildItem -Path $GameDir -Directory | ForEach-Object { Join-Path $_.FullName "Binaries\Win64\ue4ss" } | Where-Object { Test-Path (Join-Path $_ "UE4SS.dll") } | Select-Object -First 1
if (-not $ue4ss) { throw "no */Binaries/Win64/ue4ss/UE4SS.dll under $GameDir" }

$mod = Join-Path $ue4ss "Mods\UE5MCP"
New-Item -ItemType Directory -Force -Path "$mod\dlls", "$mod\Scripts" | Out-Null

if (-not $NoDll) {
    $dll = Join-Path $root "$Build\bridge\UE5MCPBridge.dll"
    if (-not (Test-Path $dll)) { throw "bridge DLL not built: $dll (run scripts/build.ps1 -Bridge)" }
    Copy-Item $dll "$mod\dlls\main.dll" -Force
}
Copy-Item (Join-Path $root "bridge\lua\main.lua") "$mod\Scripts\main.lua" -Force
@"
[server]
port = $Port
; the bridge only ever binds 127.0.0.1; this is the port
max_connections = 4
default_timeout_ms = 5000

[hooks]
ring_capacity = 20000

[log]
file = ue5mcp.log
"@ | Set-Content -Path "$mod\ue5mcp.ini" -Encoding ascii

$modsTxt = Join-Path $ue4ss "Mods\mods.txt"
$lines = Get-Content $modsTxt
if (-not ($lines | Where-Object { $_ -match '^\s*UE5MCP\s*:' })) {
    $idx = [Array]::FindIndex($lines, [Predicate[string]] { param($l) $l -match 'Keybinds' -and $l -match ';' })
    if ($idx -lt 0) { $idx = $lines.Length }
    $new = @(); $new += $lines[0..($idx-1)]; $new += "UE5MCP : 1"; if ($idx -lt $lines.Length) { $new += $lines[$idx..($lines.Length-1)] }
    Set-Content -Path $modsTxt -Value $new -Encoding ascii
    Write-Host "mods.txt: added 'UE5MCP : 1'"
}
Write-Host "installed to $mod"
