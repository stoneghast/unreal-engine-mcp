# Architecture

## Why a UE4SS C++ mod and not an engine plugin

Shipped games are monolithic Shipping builds: engine, game module and plugins in one exe, no module
loading, no exports, no plugin discovery. A UE 5.x plugin DLL cannot be loaded into one. UE4SS injects
through the `dwmapi.dll` proxy, finds `GUObjectArray`, `FName` pools and the reflection structures by
signature scanning, and exposes typed C++ access to them. We build on that and do not write a scanner.

The same tool surface could later get a second backend as a real engine plugin for projects we own
(editor or source builds). Nothing in the host or protocol assumes UE4SS.

## Processes and threads

```
host (ue5mcp.exe)                        game process
  stdio MCP loop (main thread)             UE4SS loads main.dll (bridge)
  BridgeClient: one TCP connection,          listener thread: accept on 127.0.0.1:<port>
    request/response, reconnects             per-connection reader thread: parse line, dispatch
  ProcessManager: launch/kill/wait,            "immediate" methods run on the reader thread
    exit watcher -> crash report               "game thread" methods are queued and the reader
  Session: results on disk, pagination           waits (with timeout) for completion
                                             game thread: EngineTick pre-callback drains the queue
                                               (fallback: ProcessEvent pre-callback)
                                             hook callbacks: run wherever the engine calls the
                                               function (normally game thread), write to ring buffer
```

Rules:
- Every UObject access happens on the game thread. The queue is drained from
  `Hook::RegisterEngineTickPreCallback`; if the engine tick hook is unavailable in a game,
  `Hook::RegisterProcessEventPreCallback` drains it instead (deduplicated per frame).
- A request that is not picked up within `timeout_ms` returns error 1001. The job is marked abandoned
  and its result is dropped when it eventually runs; it is never cancelled mid-flight because that is
  not safe.
- Each game-thread job runs inside a structured exception handler. An access violation inside a
  reflection call becomes error 1004 with the faulting address instead of a crash. The process state
  may be damaged after such an error; the response says so and the host records it.
- Lua jobs get a `lua_sethook` count hook that raises an error once the deadline passes, so a runaway
  script returns a traceback instead of freezing the game.

## Object identity

A `ref` is a string and is accepted in three forms:
- `h:<n>`: a handle from an earlier result. Handles are weak object pointers kept in the bridge for the
  session; a dead object resolves to error 1003. Handles are reused: the same live object always maps
  to the same handle.
- a full object path, as `GetFullName()` without the class prefix, for example
  `/Game/Maps/Pre_Alpha_Map.Pre_Alpha_Map:PersistentLevel.BP_JigCharacter_C_0`. Resolved with
  `StaticFindObject`.
- `@0x<hex>`: a raw pointer. Only accepted if it is currently a live entry in `GUObjectArray`.

Every object the bridge returns carries `{"path": ..., "handle": "h:n", "class": ...}`.

## Values

Property values go over the wire as JSON:

| FProperty | JSON |
|---|---|
| Bool | true/false |
| Int8..Int64, UInt16..UInt64, Float, Double | number |
| Str, Name, Text | string |
| Enum, Byte with enum | enum entry name (`"NewEnumerator23"` or display name when the UEnum has one); numeric accepted on input |
| Object, Class, WeakObject, SoftObject, Interface | `{"path","handle","class"}` or null; on input a `ref` string |
| Struct | object with field names; recursion bounded by `depth` |
| Array, Set | list |
| Map | list of `{"key","value"}` |
| Delegate, MulticastDelegate, FieldPath, anything else | string from `ExportTextItem` |

Input marshalling uses typed writers for the common cases and falls back to `FProperty::ImportText`
with UE's text format for the rest, so `"(X=1,Y=2,Z=3)"` is accepted anywhere a struct is expected.

## Hooks and tracing

`hook_add` uses `UFunction::RegisterPreHook` / `RegisterPostHook` (the same mechanism as Lua
`RegisterHook`). It sees calls that go through the UFunction (Blueprint calls, ProcessEvent, RPCs).
Native code calling native code directly is invisible to it; that is a property of the engine, not of
the tool.

`trace_start` uses the global ProcessEvent, ProcessInternal and ProcessLocalScriptFunction callbacks,
filtered by class or function glob, for a time window. It catches script-to-script calls that
ProcessEvent never sees. Both write to one ring buffer with a monotonic sequence number so a client
can poll with `since`.

## Lua

The bridge does not create its own Lua state. UE4SS gives C++ mods every Lua mod's states through
`on_lua_start`, and a mod folder can hold both `dlls/` and `Scripts/`. The bridge ships a Lua mod of
the same name (`UE5MCP/Scripts/main.lua`) and captures its `hook_lua` state, which is the one UE4SS
itself uses for game-thread callbacks. `lua_run` executes in that state on the game thread, so every
UE4SS Lua global (`FindAllOf`, `StaticFindObject`, `RegisterHook`, `UEHelpers`) is available and the
semantics match what a Lua mod would see.

## Persistence

The host writes every tool result to `<session_dir>/<yyyymmdd-hhmmss>-<seq>-<tool>.json`. Results above
`page_bytes` return the first page plus a `result_id`; `session_page` fetches the rest. On game exit
that the host did not request, it writes `crash-<ts>.json` with the last 50 commands, the UE4SS log
tail and the newest `crash_*.dmp` path from the UE4SS folder.
