# Tools

Names are the MCP tool names. "GT" means the call runs on the game thread. Every GT tool accepts
`timeout_ms`. Object arguments called `ref` accept a handle, a path or a pointer (see ARCHITECTURE.md).

## Core (must have)

| tool | GT | params | result |
|---|---|---|---|
| `lua_run` | yes | `code`, `timeout_ms`, `chunk_name?` | `{values:[...], prints:[...], ms}`; on error 1006 with traceback |
| `object_call` | yes | `ref`, `function` (name or `Class:Func` path for statics), `args` (object by param name, or array positional), `depth?` | `{return, out:{name:value}}` |
| `object_dump` | yes | `ref`, `depth=2`, `filter?` (glob on property name), `include=["properties","components"]`, `max_elements=64` | `{object, class_chain:[...], properties:{...}, components:[{object, relative:{location,rotation,scale}, world:{...}}]}` |
| `object_snapshot` | yes | same as dump | `{snapshot_id, object}` |
| `object_diff` | mixed | `a` (snapshot_id), `b?` (snapshot_id, default: take a new snapshot of the same object now) | `{changes:[{path, before, after}]}` |
| `object_set` | yes | `ref`, `property` (dotted path into structs), `value` | `{ok}` |
| `class_describe` | yes | `class` (path, name, or any ref whose class is used), `parents=true`, `properties=true`, `functions=true`, `cdo=false`, `filter?` | `{class, parent_chain, size, properties:[{name,type,inner,offset,size,flags,array_dim}], functions:[{name,path,flags,native,params:[{name,type,flags}],return}], cdo?}` |
| `hook_add` | yes | `function` (`Class:Func` path, or `ref`+`function`), `pre=true`, `post=true`, `capture_args=true`, `capture_return=true`, `once=false`, `context_filter?` (ref) | `{hook_id, function, signature}`. Native functions get a per-function hook; Blueprint functions are matched inside the engine's ProcessInternal/ProcessLocalScriptFunction paths. Hook the function object that actually runs: if a child Blueprint overrides it, name the child class (`BP_JigCharacter_C:ReceiveTick`, not the parent) |
| `hook_remove` | yes | `hook_id` | `{ok}` |
| `hook_list` | no | | `[{hook_id, function, calls, once, enabled}]` |
| `hook_calls` | no | `hook_id?`, `since=0`, `limit=200`, `function_glob?` | `{events:[{seq, t_ms, hook_id, function, phase:"pre"/"post", context, caller?, args, out, return}], next}` |
| `hook_clear` | no | | `{ok}` |
| `object_find` | yes | `class?` (short name or path), `name_glob?`, `path_prefix?`, `near_player?` (radius cm), `limit=100`, `include_cdo=false`, `with_location=false` | `{objects:[{path,handle,class,location?,distance?}], total}` |
| `log_tail` | no | `source="ue4ss"/"game"/"bridge"`, `cursor?`, `lines=200`, `grep?` | `{lines:[...], cursor, file}` |
| `console_exec` | yes | `command` | `{output:[...], handled_by:"exec_function"/"console"}`. Exec UFunctions (controller, pawn, CheatManager: slomo, god, fly, teleport) run through `ProcessConsoleExec` with a capturing device, so their text comes back. Console variables and engine commands (`stat fps`) fall back to `ExecuteConsoleCommand`; their output is not capturable in a Shipping build |
| `screenshot` | mixed | `path?`, `look_at?` (ref), `restore_view=true`, `method="window"/"highresshot"` | `{path, width, height}` |

## Should have

| tool | GT | params | result |
|---|---|---|---|
| `trace_start` | yes | `classes?:[...]`, `function_glob?`, `duration_ms=5000`, `capture_args=false`, `max_events=5000`, `sources=["ProcessEvent","ProcessInternal","ProcessLocalScriptFunction"]` | `{trace_id}` |
| `trace_stop` | no | `trace_id` | `{events, count, truncated}`. A busy class produces hundreds of events per second; results above the page size come back paginated, so prefer `function_glob`, a short `duration_ms` and `trace_get` with `limit` |
| `trace_get` | no | `trace_id`, `since`, `limit` | `{events, next, running}` |
| `input_key` | no | `key` ("F", "Space", "LeftShift", "F5"), `action="press"/"down"/"up"`, `hold_ms=60` | `{ok}` |
| `input_mouse` | no | `button="left"/"right"/"middle"`, `action="click"/"down"/"up"/"move"`, `x?`,`y?` (client px), `dx?`,`dy?` | `{ok}` |
| `input_text` | no | `text` | `{ok}` |
| `player_info` | yes | | `{controller, pawn, location, rotation, map, world}` |
| `player_teleport` | yes | `location`, `rotation?`, `ref?` (actor other than the pawn) | `{ok, location}` |
| `world_spawn` | yes | `class` (path, loaded with `asset_load` rules), `location`, `rotation?` | `{object}` |
| `world_destroy` | yes | `ref` | `{ok}` |
| `world_info` | yes | | `{map, world, game_mode, game_state, time_seconds, real_time, actors}` |
| `asset_exists` | yes | `path` | `{known, class?, package?}` from the live AssetRegistry |
| `asset_scan` | yes | `paths:[...]`, `force=true` | `{ok}` (`ScanPathsSynchronous`) |
| `asset_list` | yes | `path`, `recursive=true`, `class?`, `limit=500` | `{assets:[{path,class,package}]}` |
| `asset_paks` | no | | `{paks:[{file, size, mtime, dir}]}` from `<Game>/Content/Paks` and `LogicMods` |
| `asset_load` | yes | `path`, `routes=["find","registry","scan","kismet"]` | `{object, route}` or 1003 with the per-route log |
| `datatable_rows` | yes | `ref` (table), `rows?:[names]`, `glob?`, `limit=200`, `depth=3` | `{row_struct, rows:{name:{...}}}` |
| `mods_list` | no | | `[{name,type:"lua"/"cpp",installed,started}]` |
| `mods_reload` | no | `settle_ms=2500` | `{ok, mods, log:[...], errors:[...]}`. Hot-reloads ALL Lua mods through UE4SS's own Ctrl+R path (sent to the game window) and returns the UE4SS log lines emitted meanwhile. Per-mod reload is not offered: calling a mod's uninstall/start directly crashed the game |
| `recipe_run` | host | `name`, `args` | runs a profile recipe (`profiles/<game>/recipes.json`): a Lua snippet or a call sequence. Give-item, time-of-day and save slots live here because they are game specific |

## Host only

| tool | params | result |
|---|---|---|
| `game_launch` | `exe?` (default from config), `args?`, `wait_bridge_ms=60000` | `{pid, bridge_connected}` |
| `game_kill` | | `{ok}` |
| `game_status` | | `{running, pid, uptime_s, bridge_connected, health?: {in_world, map, frame_ms}}` |
| `game_wait_exit` | `timeout_ms` | `{exited, exit_code}` |
| `pak_install` | `src`, `name?`, `logic_mod=false` | copies into `Content/Paks` (or `Paks/LogicMods`); refuses while the game runs |
| `crash_last` | | `{when, exit_code, last_commands:[...], ue4ss_log_tail:[...], dump?}` |
| `session_page` | `result_id`, `offset`, `limit` | a slice of a large earlier result |
| `session_results` | `limit` | the newest result files |
| `help` | `tool?` | this document, or one entry |
