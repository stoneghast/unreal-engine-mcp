# ue5mcp

An MCP server that lets coding agents see and control a running Unreal Engine game.
Built for mod development: dump objects, call UFunctions with real parameter marshalling,
hook functions and read the call order, run Lua, inject input, take screenshots, and drive
the game process itself, all from the agent's tool list.

It is game-agnostic. Anything UE4SS can attach to (UE 4.7 through 5.8) works. The Parasites
(UE 5.7 Shipping) is the first target and lives in `profiles/theparasites/`.

## Two parts

```
 Claude / agent  <-- MCP (stdio) -->  ue5mcp.exe (host)  <-- JSON-RPC, TCP 127.0.0.1 -->  main.dll (bridge, UE4SS C++ mod)
                                       owns the game process,                            runs inside the game, executes on
                                       crash capture, result files                       the game thread via UE4SS
```

- **bridge/**: a UE4SS C++ mod (`CppUserModBase`). Listens on localhost only, no auth. Every reflection
  call runs on the game thread with a timeout. This is where features 1 to 16 live.
- **host/**: a plain Win32 C++ executable. Speaks MCP over stdio to the agent, forwards tool calls to the
  bridge, and owns everything that must survive a game crash: launch, kill, health, pak install, crash
  reports, result persistence and pagination (features 17 to 19).
- **common/**: the wire protocol shared by both.
- **profiles/<game>/**: game-specific recipes (give item, set time of day, save slots) expressed as Lua
  or call sequences on top of the generic tools. Nothing game-specific is compiled in.

See `docs/ARCHITECTURE.md`, `docs/TOOLS.md`, `docs/PROTOCOL.md`, `docs/BUILDING.md`.

## Status

- Host: compiles and runs standalone (`ue5mcp.exe serve`), works without the game for the process tools.
- Bridge: compiles against UE4SS `v4.0.0` (`30afe5ff`, the build installed in the game) and is installed into
  The Parasites. Building needs the `Re-UE4SS/UEPseudo` submodule, which
  requires a GitHub account linked to Epic Games. See `docs/BUILDING.md`.

## Quick start (once built)

```powershell
# install the bridge into the game (copies main.dll + Scripts + ini, adds the mods.txt line)
pwsh scripts/install-bridge.ps1 -GameDir "<game folder>"

# register the host with Claude Code (stdio MCP server); <repo> is where you cloned this
claude mcp add ue5mcp -- <repo>\build\host\host\ue5mcp.exe serve --config <repo>\ue5mcp.json
```
