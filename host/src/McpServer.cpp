#include "McpServer.hpp"

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <iostream>

namespace ue5mcp::host
{
    McpServer::McpServer(Config& config, Session& session, BridgeClient& bridge, HostTools& tools)
        : m_config(config), m_session(session), m_bridge(bridge), m_tools(tools)
    {
    }

    void McpServer::write_message(const json& msg)
    {
        std::string line = msg.dump();
        line.push_back('\n');
        fwrite(line.data(), 1, line.size(), stdout);
        fflush(stdout);
    }

    json McpServer::tools_list()
    {
        json tools = json::array();
        for (auto& t : m_tools.host_tool_specs())
            tools.push_back(json{{"name", t.name}, {"description", t.description}, {"inputSchema", t.input_schema}});
        for (auto& t : m_tools.bridge_tool_specs())
        {
            std::string desc = t.description;
            if (t.game_thread) desc += " (runs on the game thread; accepts timeout_ms)";
            tools.push_back(json{{"name", t.name}, {"description", desc}, {"inputSchema", t.input_schema}});
        }
        return json{{"tools", tools}};
    }

    json McpServer::call_tool(const std::string& name, const json& args, bool* is_error)
    {
        *is_error = false;
        m_session.record_command(name, args);
        json result;
        try
        {
            if (m_tools.is_host_tool(name))
                result = m_tools.call(name, args);
            else
            {
                int timeout = args.value("timeout_ms", m_config.default_timeout_ms);
                result = m_bridge.call(name, args, timeout);
            }
        }
        catch (const RpcError& e)
        {
            *is_error = true;
            result = json{{"error", {{"code", e.code}, {"message", e.message}, {"data", e.data}}}};
            if (e.code == BridgeDisconnected)
            {
                m_tools.poll();
                result["game_running"] = false;
            }
        }
        catch (const std::exception& e)
        {
            *is_error = true;
            result = json{{"error", {{"code", InternalError}, {"message", e.what()}}}};
        }
        return m_session.store_result(name, result, *is_error);
    }

    json McpServer::handle(const json& msg)
    {
        const std::string method = msg.value("method", "");
        const json id = msg.contains("id") ? msg["id"] : json(nullptr);
        const json params = msg.contains("params") ? msg["params"] : json::object();

        if (method == "initialize")
        {
            std::string requested = params.value("protocolVersion", "2025-06-18");
            static const char* known[] = {"2024-11-05", "2025-03-26", "2025-06-18"};
            for (auto k : known)
                if (requested == k) m_protocol_version = requested;
            return make_result(id, json{{"protocolVersion", m_protocol_version},
                                        {"capabilities", {{"tools", {{"listChanged", true}}}}},
                                        {"serverInfo", {{"name", "ue5mcp"}, {"version", UE5MCP_VERSION}}},
                                        {"instructions",
                                         "ue5mcp controls a running Unreal Engine game through UE4SS. Start with game_status. Object refs are "
                                         "handles 'h:N', full object paths, or '@0x..' pointers; object_find returns them. Game-thread tools take "
                                         "timeout_ms. Large results are paginated: use session_page. Game-specific actions are recipes "
                                         "(recipe_list / recipe_run)."}});
        }
        if (method == "notifications/initialized" || method == "notifications/cancelled" || method.rfind("notifications/", 0) == 0) return nullptr;
        if (method == "ping") return make_result(id, json::object());
        if (method == "tools/list") return make_result(id, tools_list());
        if (method == "resources/list") return make_result(id, json{{"resources", json::array()}});
        if (method == "resources/templates/list") return make_result(id, json{{"resourceTemplates", json::array()}});
        if (method == "prompts/list") return make_result(id, json{{"prompts", json::array()}});
        if (method == "tools/call")
        {
            std::string name = params.value("name", "");
            json args = params.contains("arguments") ? params["arguments"] : json::object();
            bool is_error = false;
            json result = call_tool(name, args, &is_error);
            // MCP content: one text block with the JSON. Compact when large to save tokens.
            std::string text = result.dump(result.dump().size() > 4000 ? -1 : 1);
            return make_result(id, json{{"content", json::array({json{{"type", "text"}, {"text", text}}})}, {"isError", is_error}});
        }
        if (id.is_null()) return nullptr;
        return make_error(id, MethodNotFound, "method not found: " + method);
    }

    int McpServer::run()
    {
        _setmode(_fileno(stdout), _O_BINARY);
        _setmode(_fileno(stdin), _O_BINARY);
        std::string line;
        std::string buffer;
        char chunk[65536];
        for (;;)
        {
            // _read returns as soon as the pipe has data; fread would block until the whole 64 KB chunk arrived.
            int n = _read(_fileno(stdin), chunk, static_cast<unsigned>(sizeof chunk));
            if (n <= 0) break;
            buffer.append(chunk, static_cast<size_t>(n));
            while (take_line(buffer, line))
            {
                if (line.empty()) continue;
                json msg = json::parse(line, nullptr, false);
                if (msg.is_discarded())
                {
                    write_message(make_error(nullptr, ParseError, "parse error"));
                    continue;
                }
                m_tools.poll();
                json reply = handle(msg);
                if (!reply.is_null()) write_message(reply);
            }
        }
        return 0;
    }
} // namespace ue5mcp::host
