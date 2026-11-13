#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <ue5mcp/protocol.hpp>

#include "BridgeClient.hpp"
#include "Config.hpp"
#include "ProcessManager.hpp"
#include "Session.hpp"

namespace ue5mcp::host
{
    // Tools implemented by the host itself (process control, crash capture, persistence, recipes),
    // plus the forwarding path for bridge tools.
    class HostTools
    {
      public:
        HostTools(Config& config, Session& session, BridgeClient& bridge, ProcessManager& process);

        std::vector<ToolSpec> host_tool_specs() const;
        bool is_host_tool(const std::string& name) const { return m_handlers.contains(name); }

        // Returns the result or throws RpcError.
        json call(const std::string& name, const json& args);

        // Bridge tools, live if connected, else the cached copy from disk.
        std::vector<ToolSpec> bridge_tool_specs();

        // Called from the main loop between requests: detects an unexpected game exit and writes the crash report.
        void poll();

        json crash_report_now(const char* reason);

      private:
        Config& m_config;
        Session& m_session;
        BridgeClient& m_bridge;
        ProcessManager& m_process;
        std::map<std::string, std::function<json(const json&)>> m_handlers;
        std::vector<ToolSpec> m_specs;
        json m_last_crash{nullptr};
        json m_recipes{nullptr};

        void add(ToolSpec spec, std::function<json(const json&)> fn);
        void load_recipes();
        std::filesystem::path cache_file() const;
        json run_recipe(const json& args);
        json substitute(const json& templ, const json& args) const;
    };
} // namespace ue5mcp::host
