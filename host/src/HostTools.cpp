#include "HostTools.hpp"

#include <windows.h>

#include <fstream>
#include <regex>
#include <thread>

namespace fs = std::filesystem;
using namespace ue5mcp::schema;

namespace ue5mcp::host
{
    static std::string narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }
    static std::wstring widen(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    HostTools::HostTools(Config& config, Session& session, BridgeClient& bridge, ProcessManager& process)
        : m_config(config), m_session(session), m_bridge(bridge), m_process(process)
    {
        load_recipes();

        add({"game_launch", "Launch the game executable from the config (or 'exe') and wait for the in-game bridge to come up.",
             obj({{"exe", str("Executable path; default from config")}, {"args", arr(str("argument"), "Extra command line arguments")},
                  {"wait_bridge_ms", integer("How long to wait for the bridge to accept connections", 60000)}}),
             false},
            [this](const json& a) {
                fs::path exe = a.contains("exe") ? fs::path(widen(a["exe"].get<std::string>())) : m_config.game_exe;
                std::vector<std::wstring> args = m_config.game_args;
                if (a.contains("args"))
                    for (auto& x : a["args"]) args.push_back(widen(x.get<std::string>()));
                auto pid = m_process.launch(exe, args);
                int wait_ms = a.value("wait_bridge_ms", 60000);
                auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
                bool connected = false;
                while (std::chrono::steady_clock::now() < deadline)
                {
                    if (!m_process.running()) break;
                    if (m_bridge.ensure_connected())
                    {
                        connected = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                json r{{"pid", pid}, {"running", m_process.running()}, {"bridge_connected", connected}};
                if (!m_process.running()) r["exit_code"] = m_process.exit_code().value_or(0);
                return r;
            });

        add({"game_kill", "Terminate the game process started or attached by this host.", obj({}), false}, [this](const json&) {
            if (!m_process.running()) return json{{"ok", false}, {"reason", "not running"}};
            m_process.kill();
            m_bridge.close();
            return json{{"ok", true}};
        });

        add({"game_status", "Is the game running, is the bridge reachable, and what does the bridge report (map, in world, frame time).",
             obj({}), false},
            [this](const json&) {
                json r{{"running", m_process.running()}, {"pid", m_process.pid()}, {"uptime_s", m_process.uptime_seconds()}};
                bool connected = m_bridge.ensure_connected();
                r["bridge_connected"] = connected;
                if (connected)
                {
                    try
                    {
                        r["health"] = m_bridge.call("bridge_health", json::object(), 3000);
                    }
                    catch (const RpcError& e)
                    {
                        r["health_error"] = e.message;
                    }
                }
                if (!m_process.running() && m_process.pid() != 0) r["exit_code"] = m_process.exit_code().value_or(0);
                return r;
            });

        add({"game_wait_exit", "Block until the game process exits or the timeout passes.",
             obj({{"timeout_ms", integer("Maximum wait", 600000)}}), false},
            [this](const json& a) {
                bool exited = m_process.wait_exit(a.value("timeout_ms", 600000));
                json r{{"exited", exited}};
                if (exited) r["exit_code"] = m_process.exit_code().value_or(0);
                return r;
            });

        add({"game_attach", "Attach to an already running game process by executable name so status/kill/crash capture work.",
             obj({{"exe_name", str("Executable file name, default: file name of config game_exe")}}), false},
            [this](const json& a) {
                std::wstring name = a.contains("exe_name") ? widen(a["exe_name"].get<std::string>()) : m_config.game_exe.filename().wstring();
                bool ok = m_process.attach_by_exe_name(name);
                return json{{"attached", ok}, {"pid", m_process.pid()}, {"bridge_connected", ok && m_bridge.ensure_connected()}};
            });

        add({"pak_install", "Copy a .pak into the game's Content/Paks (or Paks/LogicMods). Refuses while the game is running because the pak would be locked.",
             obj({{"src", str("Source .pak file")}, {"name", str("Destination file name, default: source file name")},
                  {"logic_mod", boolean("Install into Paks/LogicMods instead of Paks", false)}},
                 {"src"}),
             false},
            [this](const json& a) {
                if (m_process.running()) throw RpcError(InvalidParams, "game is running; stop it first (pak files are locked while mounted)");
                if (m_config.paks_dir.empty()) throw RpcError(InvalidParams, "paks_dir unknown; set game_root or paks_dir in the config");
                fs::path src = widen(a["src"].get<std::string>());
                if (!fs::exists(src)) throw RpcError(InvalidParams, "source not found: " + a["src"].get<std::string>());
                fs::path dir = a.value("logic_mod", false) ? m_config.paks_dir / "LogicMods" : m_config.paks_dir;
                std::error_code ec;
                fs::create_directories(dir, ec);
                fs::path dst = dir / (a.contains("name") ? fs::path(widen(a["name"].get<std::string>())) : src.filename());
                fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
                if (ec) throw RpcError(InternalError, "copy failed: " + ec.message());
                return json{{"installed", narrow(dst.wstring())}, {"bytes", fs::file_size(dst, ec)}};
            });

        add({"crash_last", "The most recent crash report: last commands sent, UE4SS log tail, newest crash dump.", obj({}), false},
            [this](const json&) { return m_last_crash.is_null() ? crash_report_now("requested") : m_last_crash; });

        add({"session_page", "Read a slice of a large earlier result by result_id.",
             obj({{"result_id", str("The result_id from a truncated result")}, {"offset", integer("Byte offset", 0)},
                  {"limit", integer("Bytes to return (default page_bytes)", 0)}},
                 {"result_id"}),
             false},
            [this](const json& a) { return m_session.page(a["result_id"].get<std::string>(), a.value("offset", 0u), a.value("limit", 0u)); });

        add({"session_results", "List the newest result files written by this session.", obj({{"limit", integer("Count", 50)}}), false},
            [this](const json& a) { return json{{"dir", m_session.dir().string()}, {"results", m_session.list_results(a.value("limit", 50u))}}; });

        add({"host_config", "Show the host configuration in effect (paths, port, session dir).", obj({}), false},
            [this](const json&) { return m_config.to_json(); });

        add({"recipe_list", "List game-specific recipes from the active profile (profiles/<game>/recipes.json).", obj({}), false},
            [this](const json&) {
                json out = json::array();
                if (m_recipes.is_object())
                    for (auto& [k, v] : m_recipes.items())
                        out.push_back(json{{"name", k}, {"description", v.value("description", "")}, {"kind", v.value("kind", "lua")}, {"args", v.value("args", json::array())}});
                return out;
            });

        add({"recipe_run", "Run a game-specific recipe from the profile: a Lua snippet or a sequence of bridge tool calls with {{arg}} substitution. Give item, set time of day, save slots live here.",
             obj({{"name", str("Recipe name (see recipe_list)")}, {"args", any("Arguments object")}}, {"name"}), false},
            [this](const json& a) { return run_recipe(a); });

        add({"help", "Documentation of the tool surface: all tools, or one tool.", obj({{"tool", str("Tool name")}}), false}, [this](const json& a) {
            auto docs = fs::path(__FILE__).parent_path().parent_path().parent_path() / "docs" / "TOOLS.md";
            std::ifstream in(docs);
            std::string text((std::istreambuf_iterator<char>(in)), {});
            if (a.contains("tool"))
            {
                std::string name = a["tool"].get<std::string>();
                for (auto& t : m_specs)
                    if (t.name == name) return json{{"tool", t.to_json()}};
                for (auto& t : bridge_tool_specs())
                    if (t.name == name) return json{{"tool", t.to_json()}};
                throw RpcError(NotFound, "unknown tool " + name);
            }
            return json{{"markdown", text.empty() ? "docs/TOOLS.md not found next to the source tree" : text}};
        });
    }

    void HostTools::add(ToolSpec spec, std::function<json(const json&)> fn)
    {
        m_handlers[spec.name] = std::move(fn);
        m_specs.push_back(std::move(spec));
    }

    std::vector<ToolSpec> HostTools::host_tool_specs() const
    {
        return m_specs;
    }

    fs::path HostTools::cache_file() const
    {
        return m_config.session_dir / "bridge-tools.json";
    }

    std::vector<ToolSpec> HostTools::bridge_tool_specs()
    {
        if (m_bridge.ensure_connected() && !m_bridge.tools().empty())
        {
            std::error_code ec;
            fs::create_directories(m_config.session_dir, ec);
            std::ofstream out(cache_file(), std::ios::binary);
            out << m_bridge.describe_info().dump(1);
            return m_bridge.tools();
        }
        std::vector<ToolSpec> cached;
        std::ifstream in(cache_file(), std::ios::binary);
        if (in)
        {
            json j = json::parse(in, nullptr, false);
            if (j.is_object())
                for (auto& t : j.value("tools", json::array())) cached.push_back(ToolSpec::from_json(t));
        }
        return cached;
    }

    json HostTools::call(const std::string& name, const json& args)
    {
        auto it = m_handlers.find(name);
        if (it == m_handlers.end()) throw RpcError(MethodNotFound, "unknown host tool " + name);
        return it->second(args.is_object() ? args : json::object());
    }

    void HostTools::poll()
    {
        if (m_process.take_unexpected_exit())
        {
            m_bridge.close();
            m_last_crash = crash_report_now("process exited unexpectedly");
            m_session.write_crash_report(m_last_crash);
        }
    }

    json HostTools::crash_report_now(const char* reason)
    {
        json r{{"when", timestamp_now(false)}, {"reason", reason}, {"pid", m_process.pid()}, {"last_commands", m_session.last_commands()}};
        if (auto code = m_process.exit_code()) r["exit_code"] = *code;
        if (!m_config.ue4ss_dir.empty())
        {
            auto log = m_config.ue4ss_dir / "UE4SS.log";
            r["ue4ss_log"] = narrow(log.wstring());
            r["ue4ss_log_tail"] = tail_file(log, 120);
            auto dmp = newest_file(m_config.ue4ss_dir, L"crash_", L".dmp");
            if (!dmp.empty())
            {
                std::error_code ec;
                auto t = fs::last_write_time(dmp, ec);
                auto age = std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now() - t).count();
                r["dump"] = json{{"file", narrow(dmp.wstring())}, {"age_s", age}, {"fresh", age < 300}};
            }
        }
        return r;
    }

    void HostTools::load_recipes()
    {
        m_recipes = json::object();
        if (m_config.profile_dir.empty()) return;
        std::ifstream in(m_config.profile_dir / "recipes.json");
        if (!in) return;
        json j = json::parse(in, nullptr, false, true);
        if (j.is_object()) m_recipes = j.contains("recipes") ? j["recipes"] : j;
    }

    json HostTools::substitute(const json& templ, const json& args) const
    {
        if (templ.is_string())
        {
            std::string s = templ.get<std::string>();
            // whole-value substitution keeps the argument's JSON type
            if (s.size() > 4 && s.rfind("{{", 0) == 0 && s.ends_with("}}"))
            {
                auto key = s.substr(2, s.size() - 4);
                if (args.contains(key)) return args[key];
            }
            static const std::regex re(R"(\{\{([A-Za-z0-9_]+)\}\})");
            std::string out;
            auto begin = std::sregex_iterator(s.begin(), s.end(), re);
            std::size_t last = 0;
            for (auto it = begin; it != std::sregex_iterator(); ++it)
            {
                out += s.substr(last, static_cast<std::size_t>(it->position()) - last);
                auto key = (*it)[1].str();
                if (args.contains(key))
                {
                    const auto& v = args[key];
                    out += v.is_string() ? v.get<std::string>() : v.dump();
                }
                else out += it->str();
                last = static_cast<std::size_t>(it->position() + it->length());
            }
            out += s.substr(last);
            return out;
        }
        if (templ.is_object())
        {
            json o = json::object();
            for (auto& [k, v] : templ.items()) o[k] = substitute(v, args);
            return o;
        }
        if (templ.is_array())
        {
            json a = json::array();
            for (auto& v : templ) a.push_back(substitute(v, args));
            return a;
        }
        return templ;
    }

    json HostTools::run_recipe(const json& a)
    {
        auto name = a["name"].get<std::string>();
        if (!m_recipes.contains(name)) throw RpcError(NotFound, "no recipe named " + name + " in " + narrow(m_config.profile_dir.wstring()));
        const json& recipe = m_recipes[name];
        json args = a.value("args", json::object());
        for (auto& spec : recipe.value("args", json::array()))
        {
            auto key = spec.value("name", "");
            if (!args.contains(key) && spec.contains("default")) args[key] = spec["default"];
            if (!args.contains(key) && spec.value("required", false)) throw RpcError(InvalidParams, "recipe " + name + " needs argument " + key);
        }
        std::string kind = recipe.value("kind", "lua");
        int timeout = recipe.value("timeout_ms", m_config.default_timeout_ms);
        if (kind == "lua")
        {
            json code = substitute(recipe.value("code", json("")), args);
            return m_bridge.call("lua_run", json{{"code", code}, {"chunk_name", "recipe:" + name}}, timeout);
        }
        if (kind == "calls")
        {
            json results = json::array();
            for (auto& step : recipe.value("steps", json::array()))
            {
                auto tool = step.value("tool", "");
                json params = substitute(step.value("params", json::object()), args);
                m_session.record_command(tool, params);
                json r = is_host_tool(tool) ? call(tool, params) : m_bridge.call(tool, params, timeout);
                results.push_back(json{{"tool", tool}, {"result", r}});
                if (step.contains("sleep_ms")) std::this_thread::sleep_for(std::chrono::milliseconds(step["sleep_ms"].get<int>()));
            }
            return json{{"steps", results}};
        }
        throw RpcError(InvalidParams, "recipe kind must be lua or calls");
    }
} // namespace ue5mcp::host
