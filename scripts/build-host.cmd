@echo off
rem Build the host only (no UE4SS). Usage: scripts\build-host.cmd [Release|Debug]
setlocal
set CFG=%1
if "%CFG%"=="" set CFG=Release
call "%~dp0vsenv.cmd" || exit /b 1
cd /d "%~dp0.."
cmake -S . -B build\host -G Ninja -DCMAKE_BUILD_TYPE=%CFG% -DUE5MCP_BUILD_BRIDGE=OFF || exit /b 1
cmake --build build\host || exit /b 1
echo built build\host\host\ue5mcp.exe
