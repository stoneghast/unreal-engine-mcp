#pragma once

#include <string>

#include <ue5mcp/protocol.hpp>

#include "HostTools.hpp"

namespace ue5mcp::host
{
    // MCP over stdio (newline-delimited JSON-RPC). Serves host tools and forwards bridge tools.
    class McpServer
    {
      public:
        McpServer(Config& config, Session& session, BridgeClient& bridge, HostTools& tools);

        int run(); // blocks until stdin closes

        // Exposed for the CLI 'call' mode.
        json call_tool(const std::string& name, const json& args, bool* is_error);

      private:
        Config& m_config;
        Session& m_session;
        BridgeClient& m_bridge;
        HostTools& m_tools;
        std::string m_protocol_version{"2025-06-18"};

        void write_message(const json& msg);
        json handle(const json& msg);
        json tools_list();
    };
} // namespace ue5mcp::host
