// Core tools: object_find, object_dump, object_snapshot, object_diff, object_set, object_call, class_describe, console_exec.
#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "Bridge.hpp"
#include "GameThread.hpp"
#include "Handles.hpp"
#include "Marshal.hpp"
#include "Registry.hpp"

#pragma warning(push)
#pragma warning(disable : 4005)
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FOutputDevice.hpp>
#include <Unreal/UActorComponent.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>
#pragma warning(pop)

using namespace ue5mcp::schema;

namespace ue5mcp::bridge
{
    using namespace RC::Unreal;

    namespace
    {
        MarshalOptions options_from(const json& p, int default_depth = 2)
        {
            MarshalOptions o;
            o.depth = opt<int>(p, "depth", default_depth);
            o.max_elements = opt<int>(p, "max_elements", 64);
            o.enum_names = opt<bool>(p, "enum_names", true);
            return o;
        }

        FProperty* find_property_in_chain(UStruct* type, const std::string& name)
        {
            auto wide = to_wide(name);
            for (UStruct* s : super_chain(type))
                for (FProperty* p : TFieldRange<FProperty>(s, EFieldIterationFlags::IncludeDeprecated))
                    if (p->GetName() == wide) return p;
            return nullptr;
        }

        // Call a named function on an object and return its "return" value (helper for location queries).
        json quick_call(UObject* obj, const char* fn_name, const json& args = json::object())
        {
            auto* fn = find_function(fn_name, obj);
            if (!fn) throw RpcError(NotFound, std::string("function not found on object: ") + fn_name);
            MarshalOptions o;
            o.depth = 3;
            return call_function(obj, fn, args, o)["return"];
        }

        UObject* player_controller()
        {
            return UObjectGlobals::FindFirstOf(STR("PlayerController"));
        }

        UObject* player_pawn()
        {
            auto* pc = player_controller();
            if (!pc) return nullptr;
            auto* prop = find_property_in_chain(pc->GetClassPrivate(), "Pawn");
            if (!prop) return nullptr;
            return *static_cast<UObject**>(prop->ContainerPtrToValuePtr<void>(pc));
        }

        std::optional<std::array<double, 3>> actor_location(UObject* actor)
        {
            if (!actor || !actor->IsA<AActor>()) return std::nullopt;
            json v = quick_call(actor, "K2_GetActorLocation");
            if (!v.is_object()) return std::nullopt;
            return std::array<double, 3>{v.value("X", 0.0), v.value("Y", 0.0), v.value("Z", 0.0)};
        }

        struct Snapshot
        {
            std::string ref;
            json params;
            json data;
            std::int64_t taken_ms;
        };
        std::mutex g_snap_mutex;
        std::map<std::uint64_t, Snapshot> g_snapshots;
        std::uint64_t g_next_snapshot{1};

        json take_dump(const json& p, UObject* obj)
        {
            auto o = options_from(p);
            std::string filter = opt<std::string>(p, "filter", "");
            std::vector<std::string> include = opt<std::vector<std::string>>(p, "include", {"properties", "components"});
            auto has = [&](const char* k) { return std::find(include.begin(), include.end(), k) != include.end(); };

            json out{{"object", Handles::instance().object_json(obj)}};
            json chain = json::array();
            for (UStruct* s : super_chain(obj->GetClassPrivate())) chain.push_back(Handles::path_of(s));
            out["class_chain"] = chain;
            out["outer"] = Handles::instance().object_json(obj->GetOuterPrivate());
            if (has("properties")) out["properties"] = dump_properties(obj, o, filter);
            if (has("components") && obj->IsA<AActor>())
            {
                json comps = json::array();
                UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
                    if (object && object->GetOuterPrivate() == obj && object->IsA<UActorComponent>())
                    {
                        json c{{"object", Handles::instance().object_json(object)}};
                        if (auto* rl = find_property_in_chain(object->GetClassPrivate(), "RelativeLocation"))
                        {
                            MarshalOptions t;
                            t.depth = 2;
                            c["relative"] = json{{"location", property_to_json(rl, object, t, 2)}};
                            if (auto* rr = find_property_in_chain(object->GetClassPrivate(), "RelativeRotation")) c["relative"]["rotation"] = property_to_json(rr, object, t, 2);
                            if (auto* rs = find_property_in_chain(object->GetClassPrivate(), "RelativeScale3D")) c["relative"]["scale"] = property_to_json(rs, object, t, 2);
                            try
                            {
                                c["world"] = json{{"location", quick_call(object, "K2_GetComponentLocation")}, {"rotation", quick_call(object, "K2_GetComponentRotation")}, {"scale", quick_call(object, "K2_GetComponentScale")}};
                            }
                            catch (const RpcError&)
                            {
                            }
                        }
                        comps.push_back(std::move(c));
                    }
                    return RC::LoopAction::Continue;
                });
                out["components"] = comps;
            }
            return out;
        }
    } // namespace

    void register_core_tools(Registry& r)
    {
        r.add({"object_find",
               "Find live objects by class (short name or path), name glob, path prefix, or distance from the player. Returns stable handles. CDOs and archetypes are skipped unless include_cdo.",
               obj({{"class", str("Class short name (Actor, BP_JigCharacter_C) or path (/Script/Engine.Actor); subclasses included")},
                    {"name_glob", str("Glob on the object name, e.g. BP_Camp*")},
                    {"path_prefix", str("Only objects whose full path starts with this")},
                    {"near_player", num("Only actors within this many cm of the player pawn; results sorted by distance")},
                    {"with_location", boolean("Include actor locations (costs one call per actor)", false)},
                    {"include_cdo", boolean("Include class default objects and archetypes", false)},
                    {"limit", integer("Maximum results", 100)},
                    {"timeout_ms", timeout()}}),
               true},
              [](const json& p) {
                  std::string cls_name = opt<std::string>(p, "class", "");
                  std::string name_glob = opt<std::string>(p, "name_glob", "");
                  std::string prefix = opt<std::string>(p, "path_prefix", "");
                  bool include_cdo = opt<bool>(p, "include_cdo", false);
                  bool with_loc = opt<bool>(p, "with_location", false) || p.contains("near_player");
                  double radius = opt<double>(p, "near_player", 0.0);
                  int limit = opt<int>(p, "limit", 100);

                  UClass* klass = nullptr;
                  if (!cls_name.empty())
                  {
                      klass = find_class(cls_name);
                      if (!klass) throw RpcError(NotFound, "class not found: " + cls_name);
                  }
                  std::optional<std::array<double, 3>> player;
                  if (p.contains("near_player")) player = actor_location(player_pawn());

                  struct Hit
                  {
                      UObject* obj;
                      double dist;
                      std::optional<std::array<double, 3>> loc;
                  };
                  std::vector<Hit> hits;
                  int total = 0;
                  auto wide_glob = name_glob;
                  UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
                      if (!object) return RC::LoopAction::Continue;
                      if (klass && !object->IsA(klass)) return RC::LoopAction::Continue;
                      if (!include_cdo && object->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject))) return RC::LoopAction::Continue;
                      if (!name_glob.empty() && !glob_match(name_glob, to_utf8(object->GetName()))) return RC::LoopAction::Continue;
                      if (!prefix.empty() && Handles::path_of(object).rfind(prefix, 0) != 0) return RC::LoopAction::Continue;
                      ++total;
                      Hit h{object, 0.0, std::nullopt};
                      if (with_loc && object->IsA<AActor>())
                      {
                          h.loc = actor_location(object);
                          if (player && h.loc)
                          {
                              double dx = (*h.loc)[0] - (*player)[0], dy = (*h.loc)[1] - (*player)[1], dz = (*h.loc)[2] - (*player)[2];
                              h.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                              if (radius > 0 && h.dist > radius) return RC::LoopAction::Continue;
                          }
                      }
                      hits.push_back(h);
                      if (!player && static_cast<int>(hits.size()) >= limit) return RC::LoopAction::Break;
                      return RC::LoopAction::Continue;
                  });
                  if (player) std::sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.dist < b.dist; });
                  json out = json::array();
                  for (auto& h : hits)
                  {
                      if (static_cast<int>(out.size()) >= limit) break;
                      json e = Handles::instance().object_json(h.obj);
                      if (h.loc) e["location"] = json{{"x", (*h.loc)[0]}, {"y", (*h.loc)[1]}, {"z", (*h.loc)[2]}};
                      if (player) e["distance"] = h.dist;
                      out.push_back(std::move(e));
                  }
                  return json{{"objects", out}, {"total", total}, {"class", klass ? Handles::path_of(klass) : ""}};
              });

        r.add({"object_dump",
               "Dump an object: class chain, every property as JSON (enums as names, object refs as {path,handle,class}, structs and arrays expanded to depth), and for actors the components with relative and world transforms.",
               obj({{"ref", ref()}, {"depth", integer("Struct/array recursion depth", 2)}, {"filter", str("Glob on property names")},
                    {"include", arr(str("properties | components"), "Sections to include")}, {"max_elements", integer("Max elements per array/map", 64)},
                    {"timeout_ms", timeout()}},
                   {"ref"}),
               true},
              [](const json& p) { return take_dump(p, Handles::instance().resolve(need<std::string>(p, "ref"))); });

        r.add({"object_snapshot", "Take a snapshot of an object's dump for later object_diff (\"what changed when I pressed F\").",
               obj({{"ref", ref()}, {"depth", integer("Recursion depth", 2)}, {"filter", str("Glob on property names")}, {"timeout_ms", timeout()}}, {"ref"}), true},
              [](const json& p) {
                  auto ref_s = need<std::string>(p, "ref");
                  auto* obj = Handles::instance().resolve(ref_s);
                  json dump = take_dump((p.contains("include") ? p : [&] { json q = p; q["include"] = json::array({"properties"}); return q; }()), obj);
                  std::lock_guard lock(g_snap_mutex);
                  auto id = g_next_snapshot++;
                  g_snapshots[id] = Snapshot{Handles::path_of(obj), p, dump, now_ms()};
                  return json{{"snapshot_id", id}, {"object", dump["object"]}};
              });

        r.add({"object_diff",
               "Compare two snapshots, or a snapshot against the object's current state (default). Returns the changed property paths with before/after values.",
               obj({{"a", integer("snapshot_id taken earlier")}, {"b", integer("second snapshot_id; omitted = snapshot the same object now")}, {"timeout_ms", timeout()}}, {"a"}),
               true},
              [](const json& p) {
                  auto a_id = need<std::uint64_t>(p, "a");
                  Snapshot a;
                  {
                      std::lock_guard lock(g_snap_mutex);
                      auto it = g_snapshots.find(a_id);
                      if (it == g_snapshots.end()) throw RpcError(NotFound, "no snapshot " + std::to_string(a_id));
                      a = it->second;
                  }
                  json b_data;
                  std::int64_t b_time;
                  if (p.contains("b"))
                  {
                      std::lock_guard lock(g_snap_mutex);
                      auto it = g_snapshots.find(p["b"].get<std::uint64_t>());
                      if (it == g_snapshots.end()) throw RpcError(NotFound, "no snapshot b");
                      b_data = it->second.data;
                      b_time = it->second.taken_ms;
                  }
                  else
                  {
                      auto* obj = Handles::instance().resolve(a.ref);
                      b_data = take_dump(a.params, obj);
                      b_time = now_ms();
                  }
                  return json{{"object", a.ref}, {"elapsed_ms", b_time - a.taken_ms}, {"changes", json_diff(a.data, b_data)}};
              });

        r.add({"object_set", "Set a property on an object. 'property' may be a dotted path into structs (CharacterMovement.MaxWalkSpeed is NOT followed through object refs; use the component's ref). Values follow the same JSON rules as object_call args.",
               obj({{"ref", ref()}, {"property", str("Property name or dotted struct path, e.g. RelativeLocation.Z")}, {"value", any("New value")}, {"timeout_ms", timeout()}}, {"ref", "property", "value"}), true},
              [](const json& p) {
                  auto* obj = Handles::instance().resolve(need<std::string>(p, "ref"));
                  auto path = need<std::string>(p, "property");
                  if (!p.contains("value")) throw RpcError(InvalidParams, "missing 'value'");
                  void* container = obj;
                  UStruct* type = obj->GetClassPrivate();
                  FProperty* prop = nullptr;
                  std::size_t start = 0;
                  while (true)
                  {
                      auto dot = path.find('.', start);
                      auto part = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
                      prop = find_property_in_chain(type, part);
                      if (!prop) throw RpcError(NotFound, "property not found: " + part + " on " + to_utf8(type->GetName()));
                      if (dot == std::string::npos) break;
                      if (!prop->IsA<FStructProperty>()) throw RpcError(InvalidParams, part + " is not a struct; cannot descend");
                      container = prop->ContainerPtrToValuePtr<void>(container);
                      type = static_cast<FStructProperty*>(prop)->GetStruct();
                      start = dot + 1;
                  }
                  void* value_ptr = prop->ContainerPtrToValuePtr<void>(container);
                  json before = value_to_json(prop, value_ptr, MarshalOptions{}, 2);
                  json_to_value(prop, value_ptr, p["value"], obj);
                  return json{{"ok", true}, {"before", before}, {"after", value_to_json(prop, value_ptr, MarshalOptions{}, 2)}};
              });

        r.add({"object_call",
               "Call any UFunction with real parameter marshalling. 'function' is a name looked up on the object's class chain, or a 'Class:Func' path for statics (ref optional then). 'args' is an object keyed by parameter name (or a positional array). Structs may be objects or UE text like (X=1,Y=2,Z=3); enums by name; objects by ref. Returns the return value and every out parameter.",
               obj({{"ref", ref("Context object; omit for static functions given as Class:Func")}, {"function", str("Function name or Class:Func / full path")},
                    {"args", any("Arguments by name {\"Param\": value} or positional [..]")}, {"depth", integer("Recursion depth for returned structs", 3)}, {"timeout_ms", timeout()}},
                   {"function"}),
               true},
              [](const json& p) {
                  auto fn_spec = need<std::string>(p, "function");
                  UObject* ctx = p.contains("ref") && !p["ref"].is_null() ? Handles::instance().resolve(p["ref"].get<std::string>()) : nullptr;
                  auto* fn = find_function(fn_spec, ctx);
                  if (!fn) throw RpcError(NotFound, "function not found: " + fn_spec + (ctx ? " on " + Handles::path_of(ctx) : ""), json{{"hint", "use class_describe with functions=true to list names"}});
                  json args = p.contains("args") ? p["args"] : json::object();
                  json result = call_function(ctx, fn, args, options_from(p, 3));
                  result["function"] = Handles::path_of(fn);
                  return result;
              });

        r.add({"class_describe",
               "Describe a class or struct at runtime: parent chain, size, every property with type/offset/flags, every function with its signature and flags, and optionally the CDO defaults. Replaces offline FModel JSON for anything live.",
               obj({{"class", str("Class/struct path or short name, or any object ref (its class is described)")}, {"parents", boolean("Include inherited members", true)},
                    {"properties", boolean("List properties", true)}, {"functions", boolean("List functions", true)}, {"cdo", boolean("Include class default object values", false)},
                    {"filter", str("Glob applied to property and function names")}, {"timeout_ms", timeout()}},
                   {"class"}),
               true},
              [](const json& p) {
                  auto spec = need<std::string>(p, "class");
                  std::string filter = opt<std::string>(p, "filter", "");
                  bool parents = opt<bool>(p, "parents", true);
                  UStruct* type = nullptr;
                  if (auto* obj = Handles::instance().try_resolve(spec))
                  {
                      if (obj->IsA<UClass>() || obj->IsA<UScriptStruct>()) type = static_cast<UStruct*>(obj);
                      else type = obj->GetClassPrivate();
                  }
                  if (!type) type = find_class(spec);
                  if (!type) throw RpcError(NotFound, "class or struct not found: " + spec);

                  json chain = json::array();
                  for (UStruct* s : super_chain(type)) chain.push_back(Handles::path_of(s));
                  json out{{"class", Handles::instance().object_json(type)}, {"parent_chain", chain}, {"size", type->GetPropertiesSize()}, {"is_struct", type->IsA<UScriptStruct>()}};
                  UStruct* scope = parents ? type : type; // when !parents we filter by owner below
                  if (opt<bool>(p, "properties", true))
                  {
                      json props = describe_struct_properties(scope, filter);
                      if (!parents)
                      {
                          json own = json::array();
                          auto name = to_utf8(type->GetName());
                          for (auto& e : props)
                              if (e["owner"] == name) own.push_back(e);
                          props = own;
                      }
                      out["properties"] = props;
                  }
                  if (opt<bool>(p, "functions", true) && type->IsA<UClass>())
                  {
                      json fns = describe_functions(static_cast<UClass*>(type), filter);
                      if (!parents)
                      {
                          json own = json::array();
                          auto name = to_utf8(type->GetName());
                          for (auto& e : fns)
                              if (e["owner"] == name) own.push_back(e);
                          fns = own;
                      }
                      out["functions"] = fns;
                  }
                  if (opt<bool>(p, "cdo", false) && type->IsA<UClass>())
                  {
                      UObject* cdo = static_cast<UClass*>(type)->GetClassDefaultObject();
                      if (cdo) out["cdo"] = dump_properties(cdo, options_from(p, 1), filter);
                  }
                  return out;
              });

        r.add({"console_exec",
               "Run a console command and capture its output. Exec UFUNCTIONs (player controller, pawn, cheat manager: slomo, god, teleport, ...) go through ProcessConsoleExec with a capturing output device so their text comes back; anything they do not handle (console variables and engine commands such as 'stat fps') falls back to ExecuteConsoleCommand, whose output only reaches the screen or log.",
               obj({{"command", str("Console command")}, {"timeout_ms", timeout()}}, {"command"}), true},
              [](const json& p) {
                  auto cmd = need<std::string>(p, "command");
                  auto* pc = player_controller();
                  if (!pc) throw RpcError(Unavailable, "no PlayerController (not in a world yet)");
                  // UE4SS's FOutputDevice is meant to be subclassed: the engine calls Virtual_Serialize on devices we hand it.
                  struct CaptureDevice : FOutputDevice
                  {
                      std::vector<std::string> lines;
                      CaptureDevice()
                      {
                          bSuppressEventTag = true;
                          bAutoEmitLineTerminator = true;
                      }
                      void Virtual_Serialize(const TCHAR* v, ELogVerbosity::Type, const FName&) override
                      {
                          if (v) lines.push_back(to_utf8(reinterpret_cast<const wchar_t*>(v)));
                      }
                      void Virtual_Serialize(const TCHAR* v, ELogVerbosity::Type, const FName&, const double) override
                      {
                          if (v) lines.push_back(to_utf8(reinterpret_cast<const wchar_t*>(v)));
                      }
                  } device;
                  auto wide = to_wide(cmd);
                  // Shipping builds compile the CheatManager forwarding out of APlayerController::ProcessConsoleExec, so ask
                  // each exec target directly: controller, its CheatManager (created by UE4SS's enabler mod), then the pawn.
                  std::vector<std::pair<const char*, UObject*>> targets{{"controller", pc}};
                  if (auto* cm_prop = find_property_in_chain(pc->GetClassPrivate(), "CheatManager"))
                      if (auto* cm = *cm_prop->ContainerPtrToValuePtr<UObject*>(pc)) targets.emplace_back("cheat_manager", cm);
                  if (auto* pawn = player_pawn()) targets.emplace_back("pawn", pawn);
                  bool handled = false;
                  std::string handled_by = "console";
                  for (auto& [name, target] : targets)
                  {
                      if (target->ProcessConsoleExec(reinterpret_cast<const TCHAR*>(wide.c_str()), device, pc))
                      {
                          handled = true;
                          handled_by = name;
                          break;
                      }
                  }
                  json out{{"command", cmd}, {"handled_by", handled_by}, {"output", device.lines}};
                  if (!handled)
                  {
                      auto* fn = find_function("/Script/Engine.KismetSystemLibrary:ExecuteConsoleCommand", nullptr);
                      if (!fn) throw RpcError(NotFound, "KismetSystemLibrary.ExecuteConsoleCommand not found");
                      MarshalOptions o;
                      call_function(nullptr, fn, json{{"WorldContextObject", Handles::instance().object_json(pc)}, {"Command", cmd}, {"SpecificPlayer", nullptr}}, o);
                      out["note"] = "not an exec function; sent to the console, output is not capturable in this build";
                  }
                  return out;
              });
    }
} // namespace ue5mcp::bridge
