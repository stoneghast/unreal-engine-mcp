<#
.SYNOPSIS
  Build ue5mcp. -Host builds only the MCP host (no UE4SS). -Bridge builds UE4SS + the bridge DLL + host.
.EXAMPLE
  pwsh scripts/build.ps1 -Host
  pwsh scripts/build.ps1 -Bridge
#>
param(
    [switch]$Host,
    [switch]$Bridge,
    [string]$Config = "Game__Shipping__Win64",
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vsenv = Join-Path $PSScriptRoot "vsenv.cmd"

function Invoke-InVsEnv([string]$cmdline) {
    # Run inside the MSVC x64 environment (found with vswhere) so cl/link/rc, CMake and Ninja are on PATH.
    & cmd.exe /c "call `"$vsenv`" && cd /d `"$root`" && $cmdline"
    if ($LASTEXITCODE -ne 0) { throw "command failed ($LASTEXITCODE): $cmdline" }
}

if (-not $Host -and -not $Bridge) { $Host = $true }

if ($Host) {
    $b = "build/host"
    if ($Clean -and (Test-Path "$root/$b")) { Remove-Item -Recurse -Force "$root/$b" }
    Invoke-InVsEnv "cmake -S . -B $b -G Ninja -DCMAKE_BUILD_TYPE=Release -DUE5MCP_BUILD_BRIDGE=OFF"
    Invoke-InVsEnv "cmake --build $b"
    Write-Host "host: $root\$b\host\ue5mcp.exe"
}

if ($Bridge) {
    $pseudo = "$root/external/RE-UE4SS/deps/first/Unreal/CMakeLists.txt"
    if (-not (Test-Path $pseudo)) {
        throw "external/RE-UE4SS/deps/first/Unreal is empty. The UEPseudo submodule needs GitHub access linked to Epic Games; see docs/BUILDING.md"
    }
    $b = "build/full"
    if ($Clean -and (Test-Path "$root/$b")) { Remove-Item -Recurse -Force "$root/$b" }
    Invoke-InVsEnv "cmake -S . -B $b -G Ninja -DCMAKE_BUILD_TYPE=$Config -DUE5MCP_BUILD_BRIDGE=ON -DUE5MCP_BUILD_HOST=ON"
    Invoke-InVsEnv "cmake --build $b --target UE5MCPBridge ue5mcp"
    Write-Host "bridge: $root\$b\bridge\UE5MCPBridge.dll"
}
