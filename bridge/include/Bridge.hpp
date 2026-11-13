// The UE4SS C++ mod. Owns the TCP server, the tool registry and the bridge configuration.
#pragma once

#include <filesystem>

#include <Mod/CppUserModBase.hpp>

#include "BridgeCommon.hpp"
#include "Registry.hpp"

namespace RC::LuaMadeSimple
{
    class Lua;
}

namespace ue5mcp::bridge
{
    struct BridgeConfig
    {
        std::uint16_t port{kDefaultPort};
        int max_connections{4};
        int default_timeout_ms{kDefaultTimeoutMs};
        std::size_t hook_ring_capacity{20000};
        std::filesystem::path mod_dir;   // ue4ss/Mods/UE5MCP
        std::filesystem::path log_file;  // mod_dir / ue5mcp.log
        std::filesystem::path shots_dir; // mod_dir / screenshots
    };

    class Server;

    class BridgeMod : public RC::CppUserModBase
    {
      public:
        BridgeMod();
        ~BridgeMod() override;

        auto on_unreal_init() -> void override;
        auto on_update() -> void override;
        auto on_lua_start(RC::LuaMadeSimple::Lua& lua, RC::LuaMadeSimple::Lua& main_lua, RC::LuaMadeSimple::Lua& async_lua, RC::LuaMadeSimple::Lua* hook_lua) -> void override;
        auto on_lua_stop(RC::LuaMadeSimple::Lua& lua, RC::LuaMadeSimple::Lua& main_lua, RC::LuaMadeSimple::Lua& async_lua, RC::LuaMadeSimple::Lua* hook_lua) -> void override;

        static BridgeMod* instance() { return s_instance; }
        const BridgeConfig& config() const { return m_config; }
        Registry& registry() { return m_registry; }
        RC::LuaMadeSimple::Lua* hook_lua() const { return m_hook_lua; }
        RC::LuaMadeSimple::Lua* main_lua() const { return m_main_lua; }
        std::int64_t started_ms() const { return m_started_ms; }

        // Dispatch one JSON-RPC request (any thread). Returns the response message.
        json dispatch(const json& request);

      private:
        static inline BridgeMod* s_instance{nullptr};
        BridgeConfig m_config;
        Registry m_registry;
        std::unique_ptr<Server> m_server;
        RC::LuaMadeSimple::Lua* m_hook_lua{nullptr};
        RC::LuaMadeSimple::Lua* m_main_lua{nullptr};
        std::int64_t m_started_ms{0};

        void load_config();
    };
} // namespace ue5mcp::bridge
