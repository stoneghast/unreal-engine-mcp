@echo off
rem Build UE4SS + the bridge DLL + the host. Usage: scripts\build-full.cmd [Game__Shipping__Win64]
setlocal
set CFG=%1
if "%CFG%"=="" set CFG=Game__Shipping__Win64
call "%~dp0vsenv.cmd" || exit /b 1
cd /d "%~dp0.."
if not exist external\RE-UE4SS\deps\first\Unreal\CMakeLists.txt (
  echo UEPseudo submodule missing: see docs\BUILDING.md
  exit /b 1
)
cmake -S . -B build\full -G Ninja -DCMAKE_BUILD_TYPE=%CFG% -DUE5MCP_BUILD_BRIDGE=ON -DUE5MCP_BUILD_HOST=ON || exit /b 1
cmake --build build\full --target UE5MCPBridge ue5mcp || exit /b 1
echo built build\full\bridge\UE5MCPBridge.dll
