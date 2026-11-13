// World/session tools: player info and teleport, spawn/destroy, world info, screenshot, input injection.
#include <thread>

#include "Bridge.hpp"
#include "GameThread.hpp"
#include "Handles.hpp"
#include "Marshal.hpp"
#include "Registry.hpp"
#include "Win32Capture.hpp"

#pragma warning(push)
#pragma warning(disable : 4005)
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/World.hpp>
#pragma warning(pop)

using namespace ue5mcp::schema;

namespace ue5mcp::bridge
{
    using namespace RC::Unreal;

    namespace
    {
        json call_named(UObject* obj, const char* fn_name, const json& args = json::object(), int depth = 3)
        {
            auto* fn = find_function(fn_name, obj);
            if (!fn) throw RpcError(NotFound, std::string("function not found: ") + fn_name + " on " + Handles::class_name_of(obj));
            MarshalOptions o;
            o.depth = depth;
            return call_function(obj, fn, args, o);
        }

        UObject* player_controller()
        {
            auto* pc = UObjectGlobals::FindFirstOf(STR("PlayerController"));
            if (!pc) throw RpcError(Unavailable, "no PlayerController: not in a world (menu or loading)");
            return pc;
        }

        UObject* player_pawn()
        {
            auto* pc = player_controller();
            json r = call_named(pc, "K2_GetPawn");
            if (!r["return"].is_object()) throw RpcError(Unavailable, "player controller has no pawn (not possessing)");
            return Handles::instance().resolve(r["return"]["handle"].get<std::string>());
        }

        json vec(const json& v, const char* what)
        {
            if (!v.is_object()) throw RpcError(InvalidParams, std::string(what) + " must be {x,y,z}");
            return json{{"X", v.value("x", v.value("X", 0.0))}, {"Y", v.value("y", v.value("Y", 0.0))}, {"Z", v.value("z", v.value("Z", 0.0))}};
        }
        json rot(const json& v)
        {
            if (!v.is_object()) return json{{"Pitch", 0.0}, {"Yaw", 0.0}, {"Roll", 0.0}};
            return json{{"Pitch", v.value("pitch", v.value("Pitch", 0.0))}, {"Yaw", v.value("yaw", v.value("Yaw", 0.0))}, {"Roll", v.value("roll", v.value("Roll", 0.0))}};
        }

        UWorld* world_of(UObject* actor)
        {
            if (!actor || !actor->IsA<AActor>()) return nullptr;
            return static_cast<AActor*>(actor)->GetWorld();
        }

        UClass* load_class(const std::string& spec)
        {
            if (auto* c = find_class(spec)) return c;
            // Fall back to the asset_load ladder through the registry of tools (keeps one implementation).
            auto* tool = BridgeMod::instance()->registry().find("asset_load");
            json r = tool->handler(json{{"path", spec}});
            if (r.contains("object") && r["object"].is_object())
            {
                auto* obj = Handles::instance().resolve(r["object"]["handle"].get<std::string>());
                if (obj->IsA<UClass>()) return static_cast<UClass*>(obj);
                // A Blueprint asset path may resolve to the UBlueprint; prefer its generated class if present
                if (auto* gen = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, to_wide(spec + "_C"))) return gen;
            }
            throw RpcError(NotFound, "class not loadable: " + spec, r);
        }
    } // namespace

    void register_world_tools(Registry& r)
    {
        r.add({"player_info", "The local player: controller, pawn, location, rotation, control rotation, current world/map.", obj({{"timeout_ms", timeout()}}), true}, [](const json&) {
            auto* pc = player_controller();
            json out{{"controller", Handles::instance().object_json(pc)}};
            json pawn = call_named(pc, "K2_GetPawn")["return"];
            out["pawn"] = pawn;
            out["control_rotation"] = call_named(pc, "GetControlRotation")["return"];
            if (pawn.is_object())
            {
                auto* pawn_obj = Handles::instance().resolve(pawn["handle"].get<std::string>());
                out["location"] = call_named(pawn_obj, "K2_GetActorLocation")["return"];
                out["rotation"] = call_named(pawn_obj, "K2_GetActorRotation")["return"];
                out["velocity"] = call_named(pawn_obj, "GetVelocity")["return"];
                if (auto* w = world_of(pawn_obj)) out["world"] = Handles::instance().object_json(w);
            }
            else if (auto* w = world_of(pc)) out["world"] = Handles::instance().object_json(w);
            return out;
        });

        r.add({"player_teleport", "Teleport the player pawn (or another actor via ref) to a location, optionally with a rotation.",
               obj({{"location", vec3("Target location in cm")}, {"rotation", rot("Target rotation (default: keep)")}, {"ref", ref("Actor to move instead of the pawn")}, {"timeout_ms", timeout()}}, {"location"}), true},
              [](const json& p) {
                  UObject* actor = p.contains("ref") && !p["ref"].is_null() ? Handles::instance().resolve(p["ref"].get<std::string>()) : player_pawn();
                  json cur_rot = call_named(actor, "K2_GetActorRotation")["return"];
                  json target_rot = p.contains("rotation") ? rot(p["rotation"]) : cur_rot;
                  json res = call_named(actor, "K2_TeleportTo", json{{"DestLocation", vec(p["location"], "location")}, {"DestRotation", target_rot}});
                  return json{{"ok", res["return"]}, {"location", call_named(actor, "K2_GetActorLocation")["return"]}, {"actor", Handles::instance().object_json(actor)}};
              });

        r.add({"world_spawn", "Spawn an actor of a class at a transform in the player's world. The class may be a loaded class name/path or an asset path (loaded through the asset_load routes).",
               obj({{"class", str("Class short name, class path, or asset path")}, {"location", vec3("Spawn location")}, {"rotation", rot("Spawn rotation")}, {"timeout_ms", timeout()}}, {"class", "location"}), true},
              [](const json& p) {
                  auto* klass = load_class(need<std::string>(p, "class"));
                  auto* pc = player_controller();
                  auto* world = world_of(pc);
                  if (!world) throw RpcError(Unavailable, "player controller has no world");
                  json l = vec(p["location"], "location");
                  json rr = rot(p.value("rotation", json::object()));
                  FVector loc{l["X"].get<double>(), l["Y"].get<double>(), l["Z"].get<double>()};
                  FRotator rotation{rr["Pitch"].get<double>(), rr["Yaw"].get<double>(), rr["Roll"].get<double>()};
                  AActor* actor = world->SpawnActor(klass, &loc, &rotation);
                  if (!actor) throw RpcError(ExecutionException, "SpawnActor returned null (collision, abstract class, or wrong world)");
                  return json{{"object", Handles::instance().object_json(actor)}};
              });

        r.add({"world_destroy", "Destroy an actor.", obj({{"ref", ref()}, {"timeout_ms", timeout()}}, {"ref"}), true}, [](const json& p) {
            auto* actor = Handles::instance().resolve(need<std::string>(p, "ref"));
            if (!actor->IsA<AActor>()) throw RpcError(InvalidParams, "ref is not an actor");
            call_named(actor, "K2_DestroyActor");
            return json{{"ok", true}};
        });

        r.add({"world_info", "Current world: map name, game mode/state, time, actor count.", obj({{"timeout_ms", timeout()}}), true}, [](const json&) {
            auto* pc = player_controller();
            auto* world = world_of(pc);
            if (!world) throw RpcError(Unavailable, "no world");
            json out{{"world", Handles::instance().object_json(world)}, {"map", to_utf8(world->GetName())}};
            MarshalOptions o;
            o.depth = 1;
            auto read = [&](const char* name) -> json {
                for (UStruct* s : super_chain(world->GetClassPrivate()))
                    for (FProperty* prop : TFieldRange<FProperty>(s, EFieldIterationFlags::IncludeDeprecated))
                        if (prop->GetName() == to_wide(name)) return property_to_json(prop, world, o, 1);
                return nullptr;
            };
            out["game_mode"] = read("AuthorityGameMode");
            out["game_state"] = read("GameState");
            // UWorld time members are not reflected; GameplayStatics exposes them as statics taking a world context.
            for (auto [key, fn] : {std::pair{"time_seconds", "GetTimeSeconds"}, std::pair{"real_time_seconds", "GetRealTimeSeconds"}, std::pair{"delta_time", "GetWorldDeltaSeconds"}})
            {
                try
                {
                    auto* f = find_function(std::string("/Script/Engine.GameplayStatics:") + fn, nullptr);
                    out[key] = f ? call_function(nullptr, f, json{{"WorldContextObject", Handles::instance().object_json(world)}}, o)["return"] : json(nullptr);
                }
                catch (const RpcError&)
                {
                    out[key] = nullptr;
                }
            }
            int actors = 0;
            UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
                if (object && object->IsA<AActor>() && !object->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)) && static_cast<AActor*>(object)->GetWorld() == world) ++actors;
                return RC::LoopAction::Continue;
            });
            out["actors"] = actors;
            out["frame_ms"] = GameThread::instance().frame_ms();
            return out;
        });

        r.add({"screenshot",
               "Capture the game window to a PNG. Optionally turn the player's view toward an object first (look_at) and restore it after. method 'window' grabs the client area via Win32 (works windowed/borderless); 'highresshot' issues the engine console command instead.",
               obj({{"path", str("Output PNG path; default <mod>/screenshots/<timestamp>.png")}, {"look_at", ref("Object to aim the camera at before capturing")},
                    {"restore_view", boolean("Restore the control rotation afterwards", true)}, {"settle_ms", integer("Wait after turning before capturing", 120)},
                    {"method", str("window | highresshot", std::string("window"))}, {"timeout_ms", timeout()}}),
               false},
              [](const json& p) {
                  auto& cfg = BridgeMod::instance()->config();
                  std::filesystem::path out = p.contains("path") ? std::filesystem::path(to_wide(p["path"].get<std::string>())) : cfg.shots_dir / (std::to_string(now_ms()) + ".png");
                  int timeout = opt<int>(p, "timeout_ms", cfg.default_timeout_ms);
                  json saved_rot;
                  if (p.contains("look_at") && !p["look_at"].is_null())
                  {
                      std::string target = p["look_at"].get<std::string>();
                      saved_rot = GameThread::instance().run(
                              [&]() -> json {
                                  auto* pc = player_controller();
                                  auto* pawn = player_pawn();
                                  auto* obj = Handles::instance().resolve(target);
                                  json from = call_named(pawn, "K2_GetActorLocation")["return"];
                                  json to = obj->IsA<AActor>() ? call_named(obj, "K2_GetActorLocation")["return"] : call_named(obj, "K2_GetComponentLocation")["return"];
                                  json prev = call_named(pc, "GetControlRotation")["return"];
                                  auto* lib = find_function("/Script/Engine.KismetMathLibrary:FindLookAtRotation", nullptr);
                                  if (!lib) throw RpcError(NotFound, "KismetMathLibrary.FindLookAtRotation not found");
                                  MarshalOptions o;
                                  json look = call_function(nullptr, lib, json{{"Start", from}, {"Target", to}}, o)["return"];
                                  call_named(pc, "SetControlRotation", json{{"NewRotation", look}});
                                  return prev;
                              },
                              timeout);
                      std::this_thread::sleep_for(std::chrono::milliseconds(opt<int>(p, "settle_ms", 120)));
                  }
                  json result;
                  if (opt<std::string>(p, "method", "window") == "highresshot")
                  {
                      auto* tool = BridgeMod::instance()->registry().find("console_exec");
                      result = GameThread::instance().run([&]() { return tool->handler(json{{"command", "HighResShot 1"}}); }, timeout);
                      result["note"] = "written by the engine under <Game>/Saved/Screenshots (LocalAppData for Shipping builds)";
                  }
                  else
                  {
                      void* hwnd = win32::game_window();
                      if (!hwnd) throw RpcError(Unavailable, "game window not found");
                      result = win32::capture_window_png(hwnd, out);
                  }
                  if (!saved_rot.is_null() && opt<bool>(p, "restore_view", true))
                  {
                      GameThread::instance().run(
                              [&]() -> json {
                                  call_named(player_controller(), "SetControlRotation", json{{"NewRotation", saved_rot}});
                                  return nullptr;
                              },
                              timeout);
                  }
                  return result;
              });

        r.add({"input_key", "Press a keyboard key in the game window (SendInput with scan codes). Names: letters, digits, F1-F24, Space, Enter, Escape, Tab, LeftShift, LeftCtrl, LeftAlt, Up/Down/Left/Right, etc.",
               obj({{"key", str("Key name")}, {"action", str("press | down | up", std::string("press"))}, {"hold_ms", integer("Hold time for press", 60)}}, {"key"}), false},
              [](const json& p) {
                  if (!win32::focus_game_window()) throw RpcError(Unavailable, "could not focus the game window");
                  if (!win32::key_action(need<std::string>(p, "key"), opt<std::string>(p, "action", "press"), opt<int>(p, "hold_ms", 60))) throw RpcError(InvalidParams, "unknown key " + need<std::string>(p, "key"));
                  return json{{"ok", true}};
              });

        r.add({"input_mouse", "Click, hold or move the mouse in the game window. x/y are client-area pixels; dx/dy are relative motion (for look).",
               obj({{"button", str("left | right | middle", std::string("left"))}, {"action", str("click | down | up | move", std::string("click"))}, {"x", integer("Client x")}, {"y", integer("Client y")},
                    {"dx", integer("Relative x motion", 0)}, {"dy", integer("Relative y motion", 0)}, {"hold_ms", integer("Hold for click", 60)}}),
               false},
              [](const json& p) {
                  if (!win32::focus_game_window()) throw RpcError(Unavailable, "could not focus the game window");
                  std::optional<int> x, y;
                  if (p.contains("x")) x = p["x"].get<int>();
                  if (p.contains("y")) y = p["y"].get<int>();
                  win32::mouse_action(opt<std::string>(p, "button", "left"), opt<std::string>(p, "action", "click"), x, y, opt<int>(p, "dx", 0), opt<int>(p, "dy", 0), opt<int>(p, "hold_ms", 60));
                  return json{{"ok", true}};
              });

        r.add({"input_text", "Type text into the focused game window (unicode).", obj({{"text", str("Text")}}, {"text"}), false}, [](const json& p) {
            if (!win32::focus_game_window()) throw RpcError(Unavailable, "could not focus the game window");
            win32::type_text(need<std::string>(p, "text"));
            return json{{"ok", true}};
        });
    }
} // namespace ue5mcp::bridge
