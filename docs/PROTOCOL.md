# Host to bridge protocol

Transport: TCP, bridge listens on `127.0.0.1` only (configurable port, default 19777). The bridge
binds to the loopback address and nothing else; there is no authentication by design.

Framing: one JSON-RPC 2.0 message per line, UTF-8, `\n` terminated. Requests carry an integer `id`.
The bridge may answer out of order; `id` is the correlation key.

```json
{"jsonrpc":"2.0","id":7,"method":"object_dump","params":{"ref":"h:12","depth":2}}
{"jsonrpc":"2.0","id":7,"result":{...}}
{"jsonrpc":"2.0","id":7,"error":{"code":1003,"message":"object not found: h:12","data":{...}}}
```

Common params accepted by every game-thread method: `timeout_ms` (default 5000).

## Error codes

| code | meaning |
|---|---|
| -32700 | parse error |
| -32601 | unknown method |
| -32602 | invalid params (message names the field) |
| 1001 | timeout waiting for the game thread |
| 1002 | game thread not available (no tick hook yet, engine not initialised) |
| 1003 | object / class / function not found |
| 1004 | exception inside the call. `data.kind` is `seh` (access violation etc., with `data.code` and `data.address`) or `cpp` (std::exception). After `seh` the process may be unstable |
| 1005 | unsupported property type for marshalling (`data.property`, `data.type`) |
| 1006 | Lua error (`data.traceback`) |
| 1007 | feature unavailable in this game / engine version (`data.reason`) |
| 1008 | host only: bridge not connected (game not running or mod not loaded) |

## Discovery

`bridge_describe` returns

```json
{ "version": "0.1.0", "ue4ss": "v4.0.0 #30afe5ff", "engine": {"major": 5, "minor": 7},
  "game": "TheParasites", "pid": 1234,
  "tools": [ {"name": "...", "description": "...", "inputSchema": {...}, "gameThread": true} ] }
```

The host turns that list into MCP tools verbatim and caches it on disk for when the game is down.

## Methods

See `TOOLS.md`. Method names equal MCP tool names (lower snake case).
