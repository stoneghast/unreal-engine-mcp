#include "Bridge.hpp"

#include <windows.h>

#include <cstdarg>
#include <deque>
#include <fstream>

#include <UE4SSProgram.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>

#include "GameThread.hpp"
#include "Server.hpp"

namespace fs = std::filesystem;

namespace ue5mcp::bridge
{
    // ---------------------------------------------------------------- string + log helpers (declared in Common.hpp)

    std::string to_utf8(StringViewType s)
    {
        if (s.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(s.data()), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(s.data()), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
        return out;
    }
    std::string to_utf8(const wchar_t* s)
    {
        return s ? to_utf8(StringViewType{reinterpret_cast<const RC::CharType*>(s)}) : std::string{};
    }
    StringType to_wide(std::string_view s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return StringType{reinterpret_cast<const RC::CharType*>(w.data()), w.size()};
    }

    namespace
    {
        std::mutex g_log_mutex;
        std::deque<std::string> g_log_ring;
        fs::path g_log_file;

        std::string timestamp()
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char buf[32];
            std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            return buf;
        }
    } // namespace

    void log(const std::string& line)
    {
        std::string stamped = timestamp() + " " + line;
        {
            std::lock_guard lock(g_log_mutex);
            g_log_ring.push_back(stamped);
            while (g_log_ring.size() > 5000) g_log_ring.pop_front();
            if (!g_log_file.empty())
            {
                std::ofstream out(g_log_file, std::ios::app | std::ios::binary);
                out << stamped << "\n";
            }
        }
        RC::Output::send<RC::LogLevel::Verbose>(STR("[UE5MCP] {}\n"), to_wide(line));
    }

    void logf(const char* fmt, ...)
    {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        log(buf);
    }

    std::vector<std::string> log_ring(std::size_t max_lines)
    {
        std::lock_guard lock(g_log_mutex);
        std::vector<std::string> out;
        auto start = g_log_ring.size() > max_lines ? g_log_ring.size() - max_lines : 0;
        for (auto i = start; i < g_log_ring.size(); ++i) out.push_back(g_log_ring[i]);
        return out;
    }

    // ---------------------------------------------------------------- BridgeMod

    static fs::path this_dll_path()
    {
        HMODULE h{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&this_dll_path), &h);
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(h, buf, static_cast<DWORD>(std::size(buf)));
        return fs::path(std::wstring(buf, n));
    }

    BridgeMod::BridgeMod() : CppUserModBase()
    {
        ModName = STR("UE5MCP");
        ModVersion = to_wide(UE5MCP_VERSION);
        ModDescription = STR("MCP bridge: lets agents inspect and control the game over loopback JSON-RPC");
        ModAuthors = STR("stoneghast");
        s_instance = this;
        m_started_ms = now_ms();

        load_config();
        g_log_file = m_config.log_file;
        log("bridge starting, version " UE5MCP_VERSION);

        register_system_tools(m_registry);
        register_core_tools(m_registry);
        register_hook_tools(m_registry);
        register_world_tools(m_registry);
        register_asset_tools(m_registry);
        register_lua_tools(m_registry);
        logf("%zu tools registered", m_registry.size());

        // Listen immediately so a host waiting on game_launch can connect; game-thread tools answer 1002 until Unreal is up.
        m_server = std::make_unique<Server>(m_config.port, m_config.max_connections);
        if (!m_server->start()) log("server failed to start; the bridge is unreachable");
    }

    BridgeMod::~BridgeMod()
    {
        if (m_server) m_server->stop();
        s_instance = nullptr;
    }

    void BridgeMod::load_config()
    {
        auto dll = this_dll_path();               // .../Mods/UE5MCP/dlls/main.dll
        m_config.mod_dir = dll.parent_path().parent_path();
        m_config.log_file = m_config.mod_dir / "ue5mcp.log";
        m_config.shots_dir = m_config.mod_dir / "screenshots";
        std::error_code ec;
        fs::create_directories(m_config.shots_dir, ec);

        wchar_t* env = nullptr; size_t env_len = 0;
        if (_wdupenv_s(&env, &env_len, L"UE5MCP_PORT") == 0 && env) { m_config.port = static_cast<std::uint16_t>(_wtoi(env)); free(env); }

        std::ifstream ini(m_config.mod_dir / "ue5mcp.ini");
        std::string line;
        while (std::getline(ini, line))
        {
            auto strip = [](std::string s) {
                auto a = s.find_first_not_of(" \t\r");
                auto b = s.find_last_not_of(" \t\r");
                return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
            };
            line = strip(line);
            if (line.empty() || line[0] == ';' || line[0] == '[' || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = strip(line.substr(0, eq));
            auto val = strip(line.substr(eq + 1));
            try
            {
                if (key == "port") m_config.port = static_cast<std::uint16_t>(std::stoi(val));
                else if (key == "max_connections") m_config.max_connections = std::stoi(val);
                else if (key == "default_timeout_ms") m_config.default_timeout_ms = std::stoi(val);
                else if (key == "ring_capacity") m_config.hook_ring_capacity = static_cast<std::size_t>(std::stoul(val));
                else if (key == "file") m_config.log_file = m_config.mod_dir / val;
            }
            catch (...)
            {
            }
        }
    }

    auto BridgeMod::on_unreal_init() -> void
    {
        GameThread::instance().install();
        log(GameThread::instance().available() ? "unreal initialised, game-thread queue installed" : "unreal initialised but no tick hook is available; game-thread tools will fail with 1002");
    }

    auto BridgeMod::on_update() -> void
    {
        // Nothing periodic yet. Handle pruning happens lazily inside resolve().
    }

    auto BridgeMod::on_lua_start(RC::LuaMadeSimple::Lua& lua, RC::LuaMadeSimple::Lua& /*main_lua*/, RC::LuaMadeSimple::Lua& /*async_lua*/, RC::LuaMadeSimple::Lua* hook_lua) -> void
    {
        // Same-name overload: this is our own Scripts/main.lua starting. Its hook state is the one UE4SS runs game-thread callbacks in.
        m_main_lua = &lua;
        m_hook_lua = hook_lua ? hook_lua : &lua;
        log("lua states captured from the UE5MCP Lua mod");
    }

    auto BridgeMod::on_lua_stop(RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua*) -> void
    {
        m_main_lua = nullptr;
        m_hook_lua = nullptr;
        log("lua states released (mod stopping or reloading)");
    }

    json BridgeMod::dispatch(const json& request)
    {
        json id = request.contains("id") ? request["id"] : json(nullptr);
        if (!request.is_object() || !request.contains("method") || !request["method"].is_string())
            return make_error(id, InvalidRequest, "request must have a string 'method'");
        const std::string method = request["method"].get<std::string>();
        json params = request.contains("params") && request["params"].is_object() ? request["params"] : json::object();

        const ToolEntry* tool = m_registry.find(method);
        if (!tool) return make_error(id, MethodNotFound, "unknown method " + method);

        auto t0 = now_ms();
        try
        {
            json result;
            if (tool->spec.game_thread)
            {
                int timeout = opt<int>(params, "timeout_ms", m_config.default_timeout_ms);
                auto handler = tool->handler;
                result = GameThread::instance().run([handler, params]() { return handler(params); }, timeout);
            }
            else
            {
                result = tool->handler(params);
            }
            auto dt = now_ms() - t0;
            if (dt > 250) logf("%s took %lld ms", method.c_str(), static_cast<long long>(dt));
            return make_result(id, std::move(result));
        }
        catch (const RpcError& e)
        {
            logf("%s failed: %d %s", method.c_str(), e.code, e.message.c_str());
            return make_error(id, e.code, e.message, e.data);
        }
        catch (const std::exception& e)
        {
            logf("%s threw: %s", method.c_str(), e.what());
            return make_error(id, ExecutionException, e.what(), json{{"kind", "cpp"}});
        }
    }
} // namespace ue5mcp::bridge
