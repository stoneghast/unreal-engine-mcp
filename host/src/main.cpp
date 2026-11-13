// ue5mcp host: MCP stdio server + game process owner.
//
//   ue5mcp serve [--config ue5mcp.json]        run as an MCP server over stdio
//   ue5mcp call <tool> [json-args] [--config]  call one tool and print the result (for testing)
//   ue5mcp tools [--config]                    list tools (host + bridge if reachable)
//   ue5mcp ping [--config]                     check the bridge
#include <windows.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "BridgeClient.hpp"
#include "Config.hpp"
#include "HostTools.hpp"
#include "McpServer.hpp"
#include "ProcessManager.hpp"
#include "Session.hpp"

using namespace ue5mcp;
using namespace ue5mcp::host;

static int usage()
{
    fprintf(stderr, "usage: ue5mcp serve|call <tool> [json]|tools|ping [--config file.json] [--port N]\n");
    return 2;
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::wstring> positional;
    std::filesystem::path config_file = "ue5mcp.json";
    int port_override = 0;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--config" && i + 1 < argc) config_file = argv[++i];
        else if (a == L"--port" && i + 1 < argc) port_override = _wtoi(argv[++i]);
        else positional.push_back(a);
    }
    if (positional.empty()) return usage();

    Config config;
    try
    {
        config = Config::load(config_file);
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "ue5mcp: bad config %ls: %s\n", config_file.c_str(), e.what());
        return 2;
    }
    if (port_override) config.bridge_port = static_cast<std::uint16_t>(port_override);

    Session session(config.session_dir, config.page_bytes);
    BridgeClient bridge(config.bridge_port, config.connect_timeout_ms);
    ProcessManager process;
    if (!config.game_exe.empty()) process.attach_by_exe_name(config.game_exe.filename().wstring());
    HostTools tools(config, session, bridge, process);
    McpServer server(config, session, bridge, tools);

    const std::wstring& cmd = positional[0];
    if (cmd == L"serve")
    {
        fprintf(stderr, "ue5mcp %s: serving MCP on stdio, bridge 127.0.0.1:%u, session %s\n", UE5MCP_VERSION, config.bridge_port, session.dir().string().c_str());
        return server.run();
    }
    if (cmd == L"ping")
    {
        bool ok = bridge.ensure_connected();
        printf("%s\n", ok ? bridge.describe_info().dump(1).c_str() : "bridge not reachable");
        return ok ? 0 : 1;
    }
    if (cmd == L"tools")
    {
        for (auto& t : tools.host_tool_specs()) printf("%-22s host    %s\n", t.name.c_str(), t.description.c_str());
        for (auto& t : tools.bridge_tool_specs()) printf("%-22s bridge%s %s\n", t.name.c_str(), t.game_thread ? "*" : " ", t.description.c_str());
        return 0;
    }
    if (cmd == L"call")
    {
        if (positional.size() < 2) return usage();
        std::string name;
        for (wchar_t c : positional[1]) name.push_back(static_cast<char>(c)); // tool names are ASCII
        json args = json::object();
        if (positional.size() >= 3)
        {
            std::wstring w = positional[2];
            int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
            args = json::parse(s, nullptr, false);
            if (args.is_discarded())
            {
                fprintf(stderr, "ue5mcp: arguments are not valid JSON\n");
                return 2;
            }
        }
        bool is_error = false;
        json r = server.call_tool(name, args, &is_error);
        printf("%s\n", r.dump(1).c_str());
        return is_error ? 1 : 0;
    }
    return usage();
}
