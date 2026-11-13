@echo off
rem Puts the MSVC x64 toolchain plus the CMake and Ninja that ship with Visual Studio on PATH.
rem Finds Visual Studio with vswhere, so no install path is hard-coded. Called by build-full.cmd and build-host.cmd.
rem Visual Studio 2022 (17.13+) by default; set UE5MCP_VS_VERSION to another vswhere version range to use a different one.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novswhere
if "%UE5MCP_VS_VERSION%"=="" set "UE5MCP_VS_VERSION=[17.13,18.0)"
set "VSDIR="
set "VSDIR_FILE=%TEMP%\ue5mcp-vsdir-%RANDOM%.txt"
"%VSWHERE%" -latest -version "%UE5MCP_VS_VERSION%" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSDIR_FILE%"
set /p VSDIR=<"%VSDIR_FILE%"
del "%VSDIR_FILE%" >nul 2>&1
if "%VSDIR%"=="" goto :novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%USERPROFILE%\.cargo\bin;%PATH%"
exit /b 0

:novswhere
echo vswhere.exe not found: install Visual Studio 2022 17.13+ with the C++ workload
exit /b 1

:novs
echo no Visual Studio in range %UE5MCP_VS_VERSION% with the C++ x64 tools found
exit /b 1
