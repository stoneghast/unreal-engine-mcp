// Tool registry: name -> spec + handler. Handlers marked game_thread are run through GameThread::run.
#pragma once

#include <map>

#include "BridgeCommon.hpp"

namespace ue5mcp::bridge
{
    using ToolHandler = std::function<json(const json& params)>;

    struct ToolEntry
    {
        ToolSpec spec;
        ToolHandler handler;
    };

    class Registry
    {
      public:
        void add(ToolSpec spec, ToolHandler handler)
        {
            // Take the key first: in `m[spec.name] = ToolEntry{std::move(spec), ...}` the right-hand side is
            // sequenced before the subscript, so spec would already be moved-from when the key is read.
            std::string name = spec.name;
            m_tools[name] = ToolEntry{std::move(spec), std::move(handler)};
        }
        const ToolEntry* find(const std::string& name) const
        {
            auto it = m_tools.find(name);
            return it == m_tools.end() ? nullptr : &it->second;
        }
        json describe_tools() const
        {
            json out = json::array();
            for (auto& [name, t] : m_tools) out.push_back(t.spec.to_json());
            return out;
        }
        std::size_t size() const { return m_tools.size(); }

      private:
        std::map<std::string, ToolEntry> m_tools;
    };

    // Each Tools_*.cpp exposes one of these.
    void register_core_tools(Registry&);
    void register_hook_tools(Registry&);
    void register_world_tools(Registry&);
    void register_asset_tools(Registry&);
    void register_lua_tools(Registry&);
    void register_system_tools(Registry&);
} // namespace ue5mcp::bridge
