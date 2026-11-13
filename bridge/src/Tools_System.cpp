// System tools: describe/ping/health, log tail, UE4SS mod list/reload.
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <thread>

#include "Bridge.hpp"
#include "GameThread.hpp"
#include "Handles.hpp"
#include "Registry.hpp"
#include "Server.hpp"
#include "Win32Capture.hpp"

#pragma warning(push)
#pragma warning(disable : 4005)
#include <Mod/CppMod.hpp>
#include <Mod/LuaMod.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealVersion.hpp>
#pragma warning(pop)

using namespace ue5mcp::schema;
namespace fs = std::filesystem;

namespace ue5mcp::bridge
{
    namespace
    {
        struct TailResult
        {
            std::vector<std::string> lines;
            std::uint64_t cursor;
        };

        TailResult tail_file(const fs::path& file, std::uint64_t cursor, std::size_t max_lines, const std::string& grep)
        {
            TailResult out{{}, cursor};
            std::ifstream in(file, std::ios::binary);
            if (!in) return out;
            in.seekg(0, std::ios::end);
            auto size = static_cast<std::uint64_t>(in.tellg());
            std::uint64_t start = cursor > 0 && cursor <= size ? cursor : (size > (1u << 20) ? size - (1u << 20) : 0);
            in.seekg(static_cast<std::streamoff>(start));
            std::string data((std::istreambuf_iterator<char>(in)), {});
            out.cursor = size;
            std::vector<std::string> all;
            std::size_t pos = 0;
            while (pos < data.size())
            {
                auto nl = data.find('\n', pos);
                if (nl == std::string::npos) nl = data.size();
                std::string line = data.substr(pos, nl - pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (grep.empty() || glob_match("*" + grep + "*", line)) all.push_back(std::move(line));
                pos = nl + 1;
            }
            if (all.size() > max_lines) all.erase(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(all.size() - max_lines));
            out.lines = std::move(all);
            return out;
        }

        fs::path ue4ss_log()
        {
            return fs::path(RC::UE4SSProgram::get_program().get_working_directory()) / "UE4SS.log";
        }

        fs::path game_log()
        {
            // Shipping builds write (when logging is enabled at all) to %LOCALAPPDATA%/<Game>/Saved/Logs/<Game>.log
            std::string game = to_utf8(RC::Unreal::UObjectGlobals::GetGameName().wstring().c_str());
            wchar_t* local = nullptr;
            size_t len = 0;
            _wdupenv_s(&local, &len, L"LOCALAPPDATA");
            fs::path base = local ? fs::path(local) : fs::path();
            free(local);
            return base / to_wide(game) / "Saved" / "Logs" / (to_wide(game) + STR(".log"));
        }

        json mod_entry(RC::Mod* mod)
        {
            auto type = dynamic_cast<RC::LuaMod*>(mod) ? "lua" : dynamic_cast<RC::CppMod*>(mod) ? "cpp" : "unknown";
            return json{{"name", to_utf8(mod->get_name())}, {"type", type}, {"installed", mod->is_installed()}, {"started", mod->is_started()}};
        }
    } // namespace

    void register_system_tools(Registry& r)
    {
        r.add({"bridge_describe", "Bridge identity and the full tool list with schemas (what the host turns into MCP tools).", obj({}), false}, [&r](const json&) {
            auto* mod = BridgeMod::instance();
            return json{{"version", UE5MCP_VERSION},
                        {"engine", {{"major", RC::Unreal::Version::Major}, {"minor", RC::Unreal::Version::Minor}}},
                        {"game", GameThread::instance().available() ? to_utf8(RC::Unreal::UObjectGlobals::GetGameName().wstring().c_str()) : ""},
                        {"pid", static_cast<std::uint32_t>(GetCurrentProcessId())},
                        {"port", mod->config().port},
                        {"tools", r.describe_tools()}};
        });

        r.add({"bridge_ping", "Round trip to the bridge without touching the game thread.", obj({}), false}, [](const json&) {
            return json{{"pong", true}, {"t_ms", now_ms()}, {"uptime_ms", now_ms() - BridgeMod::instance()->started_ms()}};
        });

        r.add({"bridge_health", "Liveness: is Unreal initialised, is the game thread ticking, frame time, are we in a world, current map, lua state, hook count.", obj({}), false}, [](const json&) {
            auto& gt = GameThread::instance();
            json out{{"unreal_ready", gt.available()},
                     {"game_thread_ticking", gt.frame_count() > 0},
                     {"frames", gt.frame_count()},
                     {"frame_ms", gt.frame_ms()},
                     {"lua_ready", BridgeMod::instance()->hook_lua() != nullptr},
                     {"uptime_ms", now_ms() - BridgeMod::instance()->started_ms()}};
            if (gt.available() && gt.frame_count() > 0)
            {
                try
                {
                    json w = gt.run(
                            []() -> json {
                                auto* pc = RC::Unreal::UObjectGlobals::FindFirstOf(STR("PlayerController"));
                                json j{{"has_player_controller", pc != nullptr}};
                                if (pc)
                                {
                                    using namespace RC::Unreal;
                                    UObject* pawn = nullptr;
                                    for (UStruct* s = pc->GetClassPrivate(); s && !pawn; s = s->GetSuperStruct())
                                        for (FProperty* p : TFieldRange<FProperty>(s, EFieldIterationFlags::IncludeDeprecated))
                                            if (p->GetName() == STR("Pawn")) { pawn = *p->ContainerPtrToValuePtr<UObject*>(pc); break; }
                                    j["in_world"] = pawn != nullptr; // a possessed pawn: the menu level also has a PlayerController
                                    j["pawn"] = pawn ? Handles::path_of(pawn) : "";
                                    j["controller"] = Handles::path_of(pc);
                                    auto path = Handles::path_of(pc); // "/Game/Maps/X.X:PersistentLevel.PC_0"
                                    auto colon = path.find(':');
                                    j["map"] = colon == std::string::npos ? path : path.substr(0, colon);
                                }
                                return j;
                            },
                            1500);
                    out.update(w);
                }
                catch (const RpcError& e)
                {
                    out["in_world"] = nullptr;
                    out["world_error"] = e.message;
                }
            }
            return out;
        });

        r.add({"log_sources", "Which log files log_tail can read.", obj({}), false}, [](const json&) {
            auto& cfg = BridgeMod::instance()->config();
            std::error_code ec;
            auto entry = [&](const char* name, const fs::path& p) { return json{{"source", name}, {"file", to_utf8(p.wstring())}, {"exists", fs::exists(p, ec)}, {"bytes", fs::exists(p, ec) ? fs::file_size(p, ec) : 0}}; };
            return json::array({entry("ue4ss", ue4ss_log()), entry("bridge", cfg.log_file), entry("game", game_log())});
        });

        r.add({"log_tail",
               "Tail a log with a cursor. source: ue4ss (UE4SS.log: Lua prints, hook errors, UE4SS output), bridge (this mod), game (the engine log if the build writes one; Shipping builds usually do not, so Blueprint 'Accessed None' warnings only appear if logging is compiled in). Pass the returned cursor next time to get only new lines.",
               obj({{"source", str("ue4ss | bridge | game", std::string("ue4ss"))}, {"cursor", integer("Byte offset from the previous call", 0)}, {"lines", integer("Max lines", 200)}, {"grep", str("Substring filter (case-insensitive)")}}),
               false},
              [](const json& p) {
                  auto source = opt<std::string>(p, "source", "ue4ss");
                  fs::path file = source == "bridge" ? BridgeMod::instance()->config().log_file : source == "game" ? game_log() : ue4ss_log();
                  auto t = tail_file(file, opt<std::uint64_t>(p, "cursor", 0), opt<std::size_t>(p, "lines", 200), opt<std::string>(p, "grep", ""));
                  return json{{"source", source}, {"file", to_utf8(file.wstring())}, {"lines", t.lines}, {"cursor", t.cursor}};
              });

        r.add({"mods_list", "UE4SS mods (Lua and C++) with installed/started state.", obj({}), false}, [](const json&) {
            json out = json::array();
            for (auto& mod : RC::UE4SSProgram::get_program().m_mods)
                if (mod) out.push_back(mod_entry(mod.get()));
            return out;
        });

        r.add({"mods_reload",
               "Hot-reload all Lua mods through UE4SS (sends Ctrl+R to the game window, the same path as the UE4SS keybind) and return the UE4SS log lines emitted meanwhile, with error lines extracted. Per-mod reload is not available.",
               obj({{"name", str("Mod name; omit for all Lua mods")}, {"settle_ms", integer("How long to wait for the reload to log", 1500)}}),
               false},
              [](const json& p) {
                  auto name = opt<std::string>(p, "name", "");
                  auto settle = opt<int>(p, "settle_ms", 2500);
                  if (!name.empty())
                      throw RpcError(Unavailable, "UE4SS has no safe per-mod reload entry point for C++ mods (calling uninstall/start_mod directly crashed the game); omit name to hot-reload all Lua mods via UE4SS Ctrl+R",
                                     json{{"reason", "per-mod reload unsupported"}});
                  auto log_file = ue4ss_log();
                  std::error_code ec;
                  std::uint64_t cursor = fs::exists(log_file, ec) ? fs::file_size(log_file, ec) : 0;
                  // UE4SS hot reload is bound to Ctrl+R (HotReloadMods in UE4SS-settings.ini). Drive it through the same input path
                  // a user would, so UE4SS runs its own reinstall sequence on its own thread.
                  if (!win32::focus_game_window()) throw RpcError(Unavailable, "could not focus the game window to send Ctrl+R");
                  win32::key_action("LeftCtrl", "down", 0);
                  win32::key_action("R", "press", 80);
                  win32::key_action("LeftCtrl", "up", 0);
                  std::this_thread::sleep_for(std::chrono::milliseconds(settle));
                  auto t = tail_file(log_file, cursor, 400, "");
                  json errors = json::array();
                  bool reloaded = false;
                  for (auto& l : t.lines)
                  {
                      if (glob_match("*error*", l) || glob_match("*exception*", l) || glob_match("*failed*", l)) errors.push_back(l);
                      if (glob_match("*Starting Lua mod*", l) || glob_match("*hot*reload*", l)) reloaded = true;
                  }
                  json mods = json::array();
                  for (auto& mod : RC::UE4SSProgram::get_program().m_mods)
                      if (mod && dynamic_cast<RC::LuaMod*>(mod.get())) mods.push_back(mod_entry(mod.get()));
                  return json{{"ok", reloaded}, {"mods", mods}, {"log", t.lines}, {"errors", errors}};
              });
    }
} // namespace ue5mcp::bridge
