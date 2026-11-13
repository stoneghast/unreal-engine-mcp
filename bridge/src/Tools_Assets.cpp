// Asset registry tools, the load-route ladder, and live DataTable reads.
#include <filesystem>

#include "Bridge.hpp"
#include "Handles.hpp"
#include "Marshal.hpp"
#include "Registry.hpp"

#pragma warning(push)
#pragma warning(disable : 4005)
#include <UE4SSProgram.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Engine/UDataTable.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UAssetRegistry.hpp>
#include <Unreal/UAssetRegistryHelpers.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealVersion.hpp>
#pragma warning(pop)

using namespace ue5mcp::schema;
namespace fs = std::filesystem;

namespace ue5mcp::bridge
{
    using namespace RC::Unreal;

    namespace
    {
        UAssetRegistry* registry()
        {
            auto* reg = static_cast<UAssetRegistry*>(UAssetRegistryHelpers::GetAssetRegistry().ObjectPointer);
            if (!reg) throw RpcError(Unavailable, "AssetRegistry not available");
            return reg;
        }

        // The UObject behind the registry interface, for ProcessEvent-based calls (ScanPathsSynchronous, GetAssetsByPath).
        UObject* registry_object()
        {
            return static_cast<UObject*>(static_cast<void*>(registry()));
        }

        json call_on(UObject* obj, const char* fn_name, const json& args, int depth = 3)
        {
            auto* fn = find_function(fn_name, obj);
            if (!fn) throw RpcError(NotFound, std::string("function not found: ") + fn_name);
            MarshalOptions o;
            o.depth = depth;
            o.max_elements = 2000;
            return call_function(obj, fn, args, o);
        }

        struct RegistryLookup
        {
            bool known{false};
            std::string package;
            UObject* loaded{nullptr};
        };

        RegistryLookup registry_lookup(const std::string& path, bool load)
        {
            RegistryLookup out;
            auto* reg = registry();
            FAssetData data = reg->GetAssetByObjectPath(FName(to_wide(path), FNAME_Add));
            bool known = (Version::IsAtMost(5, 0) && data.ObjectPath().GetComparisonIndex()) || data.PackageName().GetComparisonIndex();
            out.known = known;
            if (known)
            {
                out.package = to_utf8(data.PackageName().ToString());
                if (load) out.loaded = UAssetRegistryHelpers::GetAsset(data);
            }
            return out;
        }

        std::string package_dir(const std::string& object_path)
        {
            // "/Game/Mods/ValetStand/BP_ValetStand.BP_ValetStand_C" -> "/Game/Mods/ValetStand"
            auto dot = object_path.find('.');
            auto pkg = dot == std::string::npos ? object_path : object_path.substr(0, dot);
            auto slash = pkg.rfind('/');
            return slash == std::string::npos ? pkg : pkg.substr(0, slash);
        }

        fs::path paks_dir()
        {
            // <root>/<Project>/Binaries/Win64 -> <root>/<Project>/Content/Paks
            fs::path exe_dir = RC::UE4SSProgram::get_program().get_game_executable_directory();
            return exe_dir.parent_path().parent_path() / "Content" / "Paks";
        }
    } // namespace

    void register_asset_tools(Registry& r)
    {
        r.add({"asset_exists", "Is this object path known to the live AssetRegistry (i.e. will LoadAsset find it)?", obj({{"path", str("Object path /Game/Pkg/Asset.Asset")}, {"timeout_ms", timeout()}}, {"path"}), true},
              [](const json& p) {
                  auto path = need<std::string>(p, "path");
                  auto l = registry_lookup(path, false);
                  json out{{"path", path}, {"known", l.known}};
                  if (l.known) out["package"] = l.package;
                  if (auto* obj = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, to_wide(path))) out["loaded"] = Handles::instance().object_json(obj);
                  return out;
              });

        r.add({"asset_scan", "Make the AssetRegistry scan mount paths synchronously (what DLC/pak mounting does), so new packages in a _P pak become loadable.",
               obj({{"paths", arr(str("mount path, e.g. /Game/Mods/ValetStand"), "Paths to scan")}, {"force", boolean("bForceRescan", true)}, {"timeout_ms", timeout()}}, {"paths"}), true},
              [](const json& p) {
                  auto paths = need<std::vector<std::string>>(p, "paths");
                  json res = call_on(registry_object(), "/Script/AssetRegistry.AssetRegistry:ScanPathsSynchronous", json{{"InPaths", paths}, {"bForceRescan", opt<bool>(p, "force", true)}, {"bIgnoreDenyListScanFilters", false}});
                  return json{{"ok", true}, {"paths", paths}, {"result", res}};
              });

        r.add({"asset_list", "List assets under a package path from the live AssetRegistry.",
               obj({{"path", str("Package path, e.g. /Game/Mods")}, {"recursive", boolean("Recurse", true)}, {"class", str("Glob on asset class name")}, {"limit", integer("Max results", 500)}, {"timeout_ms", timeout()}}, {"path"}),
               true},
              [](const json& p) {
                  auto path = need<std::string>(p, "path");
                  json res = call_on(registry_object(), "/Script/AssetRegistry.AssetRegistry:GetAssetsByPath", json{{"PackagePath", path}, {"bRecursive", opt<bool>(p, "recursive", true)}, {"bIncludeOnlyOnDiskAssets", false}}, 2);
                  std::string cls_glob = opt<std::string>(p, "class", "");
                  int limit = opt<int>(p, "limit", 500);
                  json out = json::array();
                  int total = 0;
                  for (auto& a : res["out"].value("OutAssetData", json::array()))
                  {
                      if (!a.is_object()) continue;
                      ++total;
                      std::string cls = a.contains("AssetClassPath") && a["AssetClassPath"].is_object() ? a["AssetClassPath"].value("AssetName", "") : a.value("AssetClass", "");
                      if (!cls_glob.empty() && !glob_match("*" + cls_glob + "*", cls)) continue;
                      if (static_cast<int>(out.size()) >= limit) continue;
                      out.push_back(json{{"package", a.value("PackageName", "")}, {"name", a.value("AssetName", "")}, {"class", cls}, {"path", a.value("PackageName", "") + "." + a.value("AssetName", "")}});
                  }
                  return json{{"assets", out}, {"total", total}};
              });

        r.add({"asset_paks", "List .pak files in the game's Content/Paks and Paks/LogicMods with size and modification time.", obj({}), false}, [](const json&) {
            json out = json::array();
            std::error_code ec;
            for (auto dir : {paks_dir(), paks_dir() / "LogicMods"})
            {
                if (!fs::is_directory(dir, ec)) continue;
                for (auto& e : fs::directory_iterator(dir, ec))
                {
                    if (e.path().extension() != ".pak") continue;
                    auto t = std::chrono::duration_cast<std::chrono::seconds>(e.last_write_time(ec).time_since_epoch()).count();
                    out.push_back(json{{"file", to_utf8(e.path().filename().wstring())}, {"dir", to_utf8(dir.wstring())}, {"size", e.file_size(ec)}, {"mtime", t}});
                }
            }
            return json{{"paks", out}, {"paks_dir", to_utf8(paks_dir().wstring())}};
        });

        r.add({"asset_load",
               "Load an asset/class by path trying each route in order: find (already loaded), registry (AssetRegistry + GetAsset), scan (ScanPathsSynchronous on the package folder, then registry again), kismet (KismetSystemLibrary blocking load). Returns which route worked.",
               obj({{"path", str("Object path, e.g. /Game/Mods/X/BP_X.BP_X_C")}, {"routes", arr(str("find | registry | scan | kismet"), "Routes to try, in order")}, {"timeout_ms", timeout()}}, {"path"}), true},
              [](const json& p) {
                  auto path = need<std::string>(p, "path");
                  auto routes = opt<std::vector<std::string>>(p, "routes", {"find", "registry", "scan", "kismet"});
                  json attempts = json::array();
                  UObject* found = nullptr;
                  std::string route_used;
                  for (auto& route : routes)
                  {
                      json a{{"route", route}};
                      try
                      {
                          if (route == "find")
                          {
                              found = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, to_wide(path));
                          }
                          else if (route == "registry")
                          {
                              auto l = registry_lookup(path, true);
                              a["known"] = l.known;
                              found = l.loaded;
                          }
                          else if (route == "scan")
                          {
                              auto dir = package_dir(path);
                              call_on(registry_object(), "/Script/AssetRegistry.AssetRegistry:ScanPathsSynchronous", json{{"InPaths", json::array({dir})}, {"bForceRescan", true}, {"bIgnoreDenyListScanFilters", false}});
                              auto l = registry_lookup(path, true);
                              a["scanned"] = dir;
                              a["known_after_scan"] = l.known;
                              found = l.loaded;
                              if (!found) found = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, to_wide(path));
                          }
                          else if (route == "kismet")
                          {
                              auto* lib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
                              if (!lib) throw RpcError(NotFound, "KismetSystemLibrary CDO not found");
                              bool is_class = path.ends_with("_C");
                              json res = call_on(lib, is_class ? "LoadClassAsset_Blocking" : "LoadAsset_Blocking", json{{is_class ? "AssetClass" : "Asset", path}});
                              if (res["return"].is_object()) found = Handles::instance().try_resolve(res["return"]["handle"].get<std::string>());
                          }
                          else throw RpcError(InvalidParams, "unknown route " + route);
                      }
                      catch (const RpcError& e)
                      {
                          a["error"] = e.message;
                      }
                      a["ok"] = found != nullptr;
                      attempts.push_back(a);
                      if (found)
                      {
                          route_used = route;
                          break;
                      }
                  }
                  if (!found) throw RpcError(NotFound, "asset not loadable by any route: " + path, json{{"attempts", attempts}});
                  return json{{"object", Handles::instance().object_json(found)}, {"route", route_used}, {"attempts", attempts}};
              });

        r.add({"datatable_rows", "Read rows from a live (patched, in-memory) UDataTable as JSON.",
               obj({{"ref", ref("DataTable object ref or path")}, {"rows", arr(str("row name"), "Only these rows")}, {"glob", str("Glob on row names")}, {"limit", integer("Max rows", 200)},
                    {"depth", integer("Struct recursion depth", 3)}, {"timeout_ms", timeout()}},
                   {"ref"}),
               true},
              [](const json& p) {
                  auto* obj = Handles::instance().resolve(need<std::string>(p, "ref"));
                  if (!obj->IsA<UDataTable>()) throw RpcError(InvalidParams, "ref is not a DataTable: " + Handles::class_name_of(obj));
                  auto* table = static_cast<UDataTable*>(obj);
                  UScriptStruct* row_struct = table->GetRowStruct();
                  std::vector<std::string> only = opt<std::vector<std::string>>(p, "rows", {});
                  std::string glob = opt<std::string>(p, "glob", "");
                  int limit = opt<int>(p, "limit", 200);
                  MarshalOptions o;
                  o.depth = opt<int>(p, "depth", 3);
                  json rows = json::object();
                  int total = 0;
                  for (auto& elem : table->GetRowMap())
                  {
                      ++total;
                      auto name = to_utf8(elem.Key.ToString());
                      if (!only.empty() && std::find(only.begin(), only.end(), name) == only.end()) continue;
                      if (!glob.empty() && !glob_match(glob, name)) continue;
                      if (static_cast<int>(rows.size()) >= limit) continue;
                      rows[name] = row_struct ? dump_struct(row_struct, elem.Value, o, o.depth) : json(nullptr);
                  }
                  return json{{"table", Handles::instance().object_json(table)}, {"row_struct", row_struct ? Handles::path_of(row_struct) : ""}, {"total_rows", total}, {"rows", rows}};
              });
    }
} // namespace ue5mcp::bridge
