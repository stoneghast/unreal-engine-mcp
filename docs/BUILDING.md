# Building

## Prerequisites

- Visual Studio 2022 17.13+ with MSVC toolset 14.43+ (C++23). Tested with MSVC 14.44.
- CMake 3.22+ and Ninja. Both ship with Visual Studio (`<VS install>\Common7\IDE\CommonExtensions\Microsoft\CMake\{CMake\bin,Ninja}`);
  `scripts\vsenv.cmd` finds the install with `vswhere` and puts them on PATH.
- Rust toolchain 1.73+ (UE4SS's patternsleuth is Rust), via rustup.
- Git.

## UE4SS source and the UEPseudo gate

`external/RE-UE4SS` is a submodule pinned to `30afe5ff` (tag `v4.0.0`, UEPseudo `6f8683ff`), the
commit the game's installed UE4SS was built from. C++ mods must be built against the same commit as the
`UE4SS.dll` they load into, or the ABI will not match. To read the commit of an installed DLL, check
`(Get-Item UE4SS.dll).VersionInfo.ProductVersion` (v4.0.0+ embeds it) or the `Git SHA` line in `UE4SS.log`.

To move to a new UE4SS release:

```powershell
git -C external/RE-UE4SS fetch --tags
git -C external/RE-UE4SS checkout <tag or SHA>
git -C external/RE-UE4SS submodule update --init --recursive
scripts\build-full.cmd
```

Then read the release's `docs/upgrade-guide.md` for C++ and Lua API breaks.

UE4SS's `deps/first/Unreal` submodule is the private repository `Re-UE4SS/UEPseudo` (Unreal pseudo
headers derived from Epic source). Access requires:

1. A GitHub account linked to an Epic Games account: https://www.unrealengine.com/ue-on-github
2. Accepting the invitation to the `EpicGames` GitHub organisation that follows.
3. Then `git submodule update --init --recursive` inside `external/RE-UE4SS` succeeds. Maintainers
   recommend SSH; HTTPS with a token also works when the account is authorised
   (`git config --global url."https://github.com/".insteadOf "git@github.com:"` rewrites the SSH URLs).

Without it the bridge cannot compile. The host does not depend on it.

## Configure and build

From a "x64 Native Tools" prompt (or after `call scripts\vsenv.cmd` in cmd):

```bat
rem host only (no UE4SS):
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DUE5MCP_BUILD_BRIDGE=OFF
cmake --build build/host

rem everything (UE4SS + bridge + host), configuration name is UE4SS's:
cmake -S . -B build/full -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64 -DUE5MCP_BUILD_BRIDGE=ON
cmake --build build/full --target UE5MCPBridge ue5mcp
```

`scripts/build.ps1` wraps both (run it from a "x64 Native Tools" environment or let it call
`vcvars64.bat` itself). The first full build compiles UE4SS itself (minutes). The bridge DLL lands in
`build/full/bridge/UE5MCPBridge.dll`.

## Install into a game

```powershell
pwsh scripts/install-bridge.ps1 -GameDir "<game root>" [-Build build/full]
```

Creates `<Game>/Binaries/Win64/ue4ss/Mods/UE5MCP/{dlls/main.dll, Scripts/main.lua, ue5mcp.ini}` and adds
`UE5MCP : 1` above the `Keybinds` line in `mods.txt`.
