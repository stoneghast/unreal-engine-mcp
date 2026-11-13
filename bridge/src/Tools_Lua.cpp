// lua_run: execute Lua in the UE4SS hook state on the game thread, with a deadline, print capture and a traceback on error.
#include <algorithm>
#include <atomic>

#include "Bridge.hpp"
#include "GameThread.hpp"
#include "Registry.hpp"

#include <LuaMadeSimple/LuaMadeSimple.hpp>

using namespace ue5mcp::schema;

namespace ue5mcp::bridge
{
    namespace
    {
        std::atomic<std::int64_t> g_lua_deadline{0};

        void deadline_hook(lua_State* L, lua_Debug*)
        {
            if (g_lua_deadline && now_ms() > g_lua_deadline)
            {
                g_lua_deadline = 0;
                luaL_error(L, "ue5mcp: lua_run exceeded its timeout");
            }
        }

        int traceback_handler(lua_State* L)
        {
            const char* msg = lua_tostring(L, 1);
            luaL_traceback(L, L, msg ? msg : "(non-string error)", 1);
            return 1;
        }

        // Collects print() output into the registry-held table at upvalue 1.
        int print_collector(lua_State* L)
        {
            int n = lua_gettop(L);
            std::string line;
            for (int i = 1; i <= n; ++i)
            {
                size_t len = 0;
                const char* s = luaL_tolstring(L, i, &len);
                if (i > 1) line.push_back('\t');
                line.append(s, len);
                lua_pop(L, 1);
            }
            lua_pushvalue(L, lua_upvalueindex(1)); // the collector table
            auto count = static_cast<lua_Integer>(lua_rawlen(L, -1));
            lua_pushlstring(L, line.data(), line.size());
            lua_rawseti(L, -2, count + 1);
            lua_pop(L, 1);
            return 0;
        }

        json lua_to_json(lua_State* L, int idx, int depth)
        {
            idx = lua_absindex(L, idx);
            switch (lua_type(L, idx))
            {
            case LUA_TNIL:
                return nullptr;
            case LUA_TBOOLEAN:
                return lua_toboolean(L, idx) != 0;
            case LUA_TNUMBER:
                if (lua_isinteger(L, idx)) return static_cast<std::int64_t>(lua_tointeger(L, idx));
                return lua_tonumber(L, idx);
            case LUA_TSTRING: {
                size_t len = 0;
                const char* s = lua_tolstring(L, idx, &len);
                return std::string(s, len);
            }
            case LUA_TTABLE: {
                if (depth <= 0) return "<table>";
                // array if keys are 1..n
                auto n = lua_rawlen(L, idx);
                bool is_array = n > 0;
                json obj = json::object();
                std::vector<json> arr(static_cast<size_t>(n));
                lua_pushnil(L);
                while (lua_next(L, idx))
                {
                    json v = lua_to_json(L, -1, depth - 1);
                    if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2) && is_array)
                    {
                        auto k = lua_tointeger(L, -2);
                        if (k >= 1 && static_cast<size_t>(k) <= n) arr[static_cast<size_t>(k) - 1] = v;
                        else is_array = false;
                    }
                    else is_array = false;
                    if (!is_array)
                    {
                        lua_pushvalue(L, -2);
                        size_t len = 0;
                        const char* k = lua_tolstring(L, -1, &len);
                        obj[k ? std::string(k, len) : "?"] = v;
                        lua_pop(L, 1);
                    }
                    lua_pop(L, 1);
                }
                if (is_array) return json(arr);
                if (obj.empty() && n > 0)
                {
                    // mixed table: re-walk into the object form
                    lua_pushnil(L);
                    while (lua_next(L, idx))
                    {
                        lua_pushvalue(L, -2);
                        obj[lua_tostring(L, -1) ? lua_tostring(L, -1) : "?"] = lua_to_json(L, -2, depth - 1);
                        lua_pop(L, 2);
                    }
                }
                return obj;
            }
            case LUA_TUSERDATA:
            case LUA_TLIGHTUSERDATA: {
                // Prefer UE5MCP.tojson (defined in Scripts/main.lua) which knows UE4SS object userdata.
                lua_getglobal(L, "UE5MCP");
                if (lua_istable(L, -1))
                {
                    lua_getfield(L, -1, "tojson");
                    if (lua_isfunction(L, -1))
                    {
                        lua_pushvalue(L, idx);
                        if (lua_pcall(L, 1, 1, 0) == LUA_OK)
                        {
                            json r = lua_to_json(L, -1, depth - 1);
                            lua_pop(L, 2);
                            return r;
                        }
                        lua_pop(L, 1); // error
                    }
                    else lua_pop(L, 1);
                }
                lua_pop(L, 1);
                const char* s = luaL_tolstring(L, idx, nullptr);
                std::string out = s ? s : "<userdata>";
                lua_pop(L, 1);
                return out;
            }
            default: {
                const char* s = luaL_tolstring(L, idx, nullptr);
                std::string out = s ? s : lua_typename(L, lua_type(L, idx));
                lua_pop(L, 1);
                return out;
            }
            }
        }
    } // namespace

    void register_lua_tools(Registry& r)
    {
        r.add({"lua_run",
               "Run Lua on the game thread in the UE4SS hook state (all UE4SS globals available: FindAllOf, StaticFindObject, RegisterHook, UEHelpers...). Returns the chunk's return values as JSON plus captured print() output; on error returns the message and a traceback. A timeout aborts runaway loops.",
               obj({{"code", str("Lua source. Use 'return <expr>' to get values back.")}, {"chunk_name", str("Name shown in tracebacks", std::string("lua_run"))}, {"depth", integer("Table recursion depth for results", 4)}, {"timeout_ms", timeout()}}, {"code"}),
               true},
              [](const json& p) {
                  auto code = need<std::string>(p, "code");
                  auto chunk = opt<std::string>(p, "chunk_name", "lua_run");
                  int depth = opt<int>(p, "depth", 4);
                  int timeout = opt<int>(p, "timeout_ms", BridgeMod::instance()->config().default_timeout_ms);
                  auto* lua = BridgeMod::instance()->hook_lua();
                  if (!lua) throw RpcError(Unavailable, "Lua state not available: the UE5MCP Lua mod has not started (Scripts/main.lua installed and enabled in mods.txt?)");
                  lua_State* L = lua->get_lua_state();
                  int base = lua_gettop(L);
                  auto t0 = now_ms();

                  // print capture
                  lua_newtable(L);
                  int prints_idx = lua_gettop(L);
                  lua_getglobal(L, "print");
                  int old_print_idx = lua_gettop(L);
                  lua_pushvalue(L, prints_idx);
                  lua_pushcclosure(L, &print_collector, 1);
                  lua_setglobal(L, "print");

                  lua_pushcfunction(L, &traceback_handler);
                  int msgh = lua_gettop(L);

                  std::string err;
                  json values = json::array();
                  int status = luaL_loadbuffer(L, code.data(), code.size(), ("=" + chunk).c_str());
                  if (status != LUA_OK)
                  {
                      err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "load error";
                      lua_pop(L, 1);
                  }
                  else
                  {
                      g_lua_deadline = now_ms() + std::max(100, timeout - 400); // raise the Lua error before the RPC wait gives up so the traceback reaches the caller
                      lua_sethook(L, &deadline_hook, LUA_MASKCOUNT, 2000);
                      status = lua_pcall(L, 0, LUA_MULTRET, msgh);
                      lua_sethook(L, nullptr, 0, 0);
                      g_lua_deadline = 0;
                      if (status != LUA_OK)
                      {
                          err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "runtime error";
                          lua_pop(L, 1);
                      }
                      else
                      {
                          int nresults = lua_gettop(L) - msgh;
                          for (int i = 1; i <= nresults; ++i) values.push_back(lua_to_json(L, msgh + i, depth));
                      }
                  }

                  // collect prints, restore print, clean the stack
                  json prints = json::array();
                  auto n = lua_rawlen(L, prints_idx);
                  for (lua_Unsigned i = 1; i <= n; ++i)
                  {
                      lua_rawgeti(L, prints_idx, static_cast<lua_Integer>(i));
                      size_t len = 0;
                      const char* s = lua_tolstring(L, -1, &len);
                      prints.push_back(s ? std::string(s, len) : "");
                      lua_pop(L, 1);
                  }
                  lua_pushvalue(L, old_print_idx);
                  lua_setglobal(L, "print");
                  lua_settop(L, base);

                  if (!err.empty())
                  {
                      auto nl = err.find('\n');
                      std::string message = nl == std::string::npos ? err : err.substr(0, nl);
                      throw RpcError(LuaError, message, json{{"traceback", err}, {"prints", prints}, {"ms", now_ms() - t0}});
                  }
                  return json{{"values", values}, {"prints", prints}, {"ms", now_ms() - t0}};
              });
    }
} // namespace ue5mcp::bridge
