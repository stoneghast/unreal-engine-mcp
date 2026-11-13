#include "Marshal.hpp"

#include <cstring>
#include <unordered_map>

#include "Handles.hpp"

#pragma warning(push)
#pragma warning(disable : 4005)
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/FStrProperty.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UEnum.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#pragma warning(pop)

namespace ue5mcp::bridge
{
    using namespace RC::Unreal;

    // ------------------------------------------------------------------ helpers

    static std::string prop_class(FProperty* p)
    {
        return to_utf8(p->GetClass().GetFName().ToString());
    }

    static std::string fstring_utf8(const FString& s)
    {
        const TCHAR* chars = *s;
        return chars ? to_utf8(reinterpret_cast<const wchar_t*>(chars)) : std::string{};
    }

    static std::string export_text(FProperty* prop, void* value_ptr)
    {
        FString text{};
        prop->ExportTextItem(text, value_ptr, nullptr, nullptr, 0);
        return fstring_utf8(text);
    }

    static void import_text(FProperty* prop, void* value_ptr, const std::string& text, UObject* owner)
    {
        auto wide = to_wide(text);
        const TCHAR* result = prop->ImportText(reinterpret_cast<const TCHAR*>(wide.c_str()), value_ptr, 0, owner, nullptr);
        if (!result)
            throw RpcError(InvalidParams, "ImportText rejected value for " + to_utf8(prop->GetName()) + " (" + type_name(prop) + "): " + text,
                           json{{"property", to_utf8(prop->GetName())}, {"type", type_name(prop)}, {"text", text}});
    }

    static std::string quote(const std::string& s)
    {
        std::string out = "\"";
        for (char c : s)
        {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    }

    std::string enum_name_for(UEnum* e, std::int64_t value)
    {
        if (!e) return std::to_string(value);
        for (auto& [name, v] : e->ForEachName())
        {
            if (v == value)
            {
                auto s = to_utf8(name.ToString());
                if (auto p = s.rfind("::"); p != std::string::npos) s = s.substr(p + 2);
                return s;
            }
        }
        return std::to_string(value);
    }

    std::optional<std::int64_t> enum_value_for(UEnum* e, const std::string& wanted)
    {
        if (!e) return std::nullopt;
        for (auto& [name, v] : e->ForEachName())
        {
            auto s = to_utf8(name.ToString());
            if (s == wanted) return v;
            if (auto p = s.rfind("::"); p != std::string::npos && s.substr(p + 2) == wanted) return v;
        }
        return std::nullopt;
    }

    std::vector<UStruct*> super_chain(UStruct* type)
    {
        std::vector<UStruct*> out;
        for (UStruct* s = type; s; s = s->GetSuperStruct()) out.push_back(s);
        return out;
    }

    // ------------------------------------------------------------------ type names and flags

    std::string type_name(FProperty* prop)
    {
        std::string cls = prop_class(prop);
        auto strip = [](std::string s) {
            if (s.ends_with("Property")) s.resize(s.size() - 8);
            return s;
        };
        if (cls == "StructProperty") return "Struct<" + to_utf8(static_cast<FStructProperty*>(prop)->GetStruct()->GetName()) + ">";
        if (cls == "ObjectProperty" || cls == "ObjectPtrProperty" || cls == "WeakObjectProperty" || cls == "SoftObjectProperty" || cls == "LazyObjectProperty")
        {
            UClass* oc = static_cast<FObjectProperty*>(prop)->GetPropertyClass();
            return strip(cls) + "<" + (oc ? to_utf8(oc->GetName()) : "?") + ">";
        }
        if (cls == "ClassProperty" || cls == "ClassPtrProperty" || cls == "SoftClassProperty")
        {
            UClass* mc = static_cast<FClassProperty*>(prop)->GetMetaClass();
            return strip(cls) + "<" + (mc ? to_utf8(mc->GetName()) : "?") + ">";
        }
        if (cls == "EnumProperty")
        {
            UEnum* e = static_cast<FEnumProperty*>(prop)->GetEnum();
            return "Enum<" + (e ? to_utf8(e->GetName()) : "?") + ">";
        }
        if (cls == "ByteProperty")
        {
            UEnum* e = static_cast<FByteProperty*>(prop)->GetEnum();
            return e ? "Byte<" + to_utf8(e->GetName()) + ">" : "Byte";
        }
        if (cls == "ArrayProperty") return "Array<" + type_name(static_cast<FArrayProperty*>(prop)->GetInner()) + ">";
        if (cls == "SetProperty") return "Set<" + type_name(static_cast<FSetProperty*>(prop)->GetElementProp()) + ">";
        if (cls == "MapProperty")
        {
            auto* mp = static_cast<FMapProperty*>(prop);
            return "Map<" + type_name(mp->GetKeyProp()) + "," + type_name(mp->GetValueProp()) + ">";
        }
        if (cls == "InterfaceProperty") return "Interface";
        return strip(cls);
    }

    json property_flags(FProperty* prop)
    {
        static const std::pair<std::uint64_t, const char*> names[] = {
                {0x1, "Edit"}, {0x2, "ConstParm"}, {0x4, "BlueprintVisible"}, {0x10, "BlueprintReadOnly"}, {0x20, "Net"},
                {0x80, "Parm"}, {0x100, "OutParm"}, {0x400, "ReturnParm"}, {0x2000, "Transient"}, {0x4000, "Config"},
                {0x20000, "EditConst"}, {0x80000, "InstancedReference"}, {0x1000000, "SaveGame"}, {0x8000000, "ReferenceParm"},
                {0x10000000, "BlueprintAssignable"}, {0x20000000, "Deprecated"}, {0x100000000ull, "RepNotify"},
                {0x800000000ull, "EditorOnly"}, {0x100000000000ull, "BlueprintCallable"}, {0x1000000000000ull, "ExposeOnSpawn"},
                {0x10000000000000ull, "Public"}, {0x20000000000000ull, "Protected"}, {0x40000000000000ull, "Private"}};
        auto flags = static_cast<std::uint64_t>(prop->GetPropertyFlags());
        json out = json::array();
        for (auto& [bit, name] : names)
            if (flags & bit) out.push_back(name);
        return out;
    }

    json function_flags(UFunction* fn)
    {
        static const std::pair<std::uint32_t, const char*> names[] = {
                {0x1, "Final"}, {0x4, "BlueprintAuthorityOnly"}, {0x8, "BlueprintCosmetic"}, {0x40, "Net"}, {0x80, "NetReliable"},
                {0x200, "Exec"}, {0x400, "Native"}, {0x800, "Event"}, {0x2000, "Static"}, {0x4000, "NetMulticast"},
                {0x8000, "UbergraphFunction"}, {0x10000, "MulticastDelegate"}, {0x20000, "Public"}, {0x40000, "Private"},
                {0x80000, "Protected"}, {0x100000, "Delegate"}, {0x200000, "NetServer"}, {0x400000, "HasOutParms"},
                {0x800000, "HasDefaults"}, {0x1000000, "NetClient"}, {0x4000000, "BlueprintCallable"}, {0x8000000, "BlueprintEvent"},
                {0x10000000, "BlueprintPure"}, {0x40000000, "Const"}};
        auto flags = static_cast<std::uint32_t>(fn->GetFunctionFlags());
        json out = json::array();
        for (auto& [bit, name] : names)
            if (flags & bit) out.push_back(name);
        return out;
    }

    // ------------------------------------------------------------------ read: value -> JSON

    static json numeric_to_json(const std::string& cls, void* ptr)
    {
        if (cls == "Int8Property") return *static_cast<std::int8_t*>(ptr);
        if (cls == "Int16Property") return *static_cast<std::int16_t*>(ptr);
        if (cls == "IntProperty") return *static_cast<std::int32_t*>(ptr);
        if (cls == "Int64Property") return *static_cast<std::int64_t*>(ptr);
        if (cls == "ByteProperty") return *static_cast<std::uint8_t*>(ptr);
        if (cls == "UInt16Property") return *static_cast<std::uint16_t*>(ptr);
        if (cls == "UInt32Property") return *static_cast<std::uint32_t*>(ptr);
        if (cls == "UInt64Property") return *static_cast<std::uint64_t*>(ptr);
        if (cls == "FloatProperty") return *static_cast<float*>(ptr);
        if (cls == "DoubleProperty") return *static_cast<double*>(ptr);
        return nullptr;
    }

    static std::int64_t numeric_to_int(FProperty* prop, void* ptr)
    {
        auto v = numeric_to_json(prop_class(prop), ptr);
        return v.is_number() ? v.get<std::int64_t>() : 0;
    }

    static json object_ref_json(UObject* obj, const MarshalOptions& opt)
    {
        if (!obj) return nullptr;
        if (!opt.object_refs) return Handles::path_of(obj);
        return Handles::instance().object_json(obj);
    }

    json value_to_json(FProperty* prop, void* ptr, const MarshalOptions& opt, int depth_left)
    {
        const std::string cls = prop_class(prop);
        if (cls == "BoolProperty")
        {
            auto* bp = static_cast<FBoolProperty*>(prop);
            auto byte = *(static_cast<std::uint8_t*>(ptr) + bp->GetByteOffset());
            return (byte & bp->GetFieldMask()) != 0;
        }
        if (cls == "ByteProperty")
        {
            UEnum* e = static_cast<FByteProperty*>(prop)->GetEnum();
            auto v = *static_cast<std::uint8_t*>(ptr);
            return (e && opt.enum_names) ? json(enum_name_for(e, v)) : json(v);
        }
        if (auto n = numeric_to_json(cls, ptr); !n.is_null()) return n;
        if (cls == "EnumProperty")
        {
            auto* ep = static_cast<FEnumProperty*>(prop);
            auto v = numeric_to_int(ep->GetUnderlyingProperty(), ptr);
            return opt.enum_names ? json(enum_name_for(ep->GetEnum(), v)) : json(v);
        }
        if (cls == "StrProperty") return fstring_utf8(*static_cast<FString*>(ptr));
        if (cls == "NameProperty") return to_utf8(static_cast<FName*>(ptr)->ToString());
        if (cls == "TextProperty") return export_text(prop, ptr);
        if (cls == "ObjectProperty" || cls == "ObjectPtrProperty" || cls == "ClassProperty" || cls == "ClassPtrProperty" || cls == "InterfaceProperty") return object_ref_json(*static_cast<UObject**>(ptr), opt);
        if (cls == "WeakObjectProperty") return object_ref_json(static_cast<FWeakObjectPtr*>(ptr)->Get(), opt);
        if (cls == "SoftObjectProperty" || cls == "SoftClassProperty" || cls == "LazyObjectProperty") return export_text(prop, ptr);
        if (cls == "StructProperty")
        {
            auto* sp = static_cast<FStructProperty*>(prop);
            if (depth_left <= 0) return json{{"$struct", to_utf8(sp->GetStruct()->GetName())}, {"$text", export_text(prop, ptr)}};
            return dump_struct(sp->GetStruct(), ptr, opt, depth_left - 1);
        }
        if (cls == "ArrayProperty")
        {
            auto* ap = static_cast<FArrayProperty*>(prop);
            auto* inner = ap->GetInner();
            auto* arr = static_cast<FScriptArray*>(ptr);
            int n = arr->Num();
            json out = json::array();
            auto* data = static_cast<std::uint8_t*>(arr->GetData());
            int limit = std::min(n, opt.max_elements);
            for (int i = 0; i < limit; ++i) out.push_back(value_to_json(inner, data + static_cast<std::size_t>(i) * inner->GetElementSize(), opt, depth_left));
            if (n > limit) out.push_back(json{{"$truncated", n - limit}, {"$total", n}});
            return out;
        }
        if (cls == "SetProperty")
        {
            auto* sp = static_cast<FSetProperty*>(prop);
            auto* elem = sp->GetElementProp();
            auto* set = static_cast<FScriptSet*>(ptr);
            auto layout = FScriptSet::GetScriptLayout(elem->GetSize(), elem->GetMinAlignment());
            json out = json::array();
            int count = 0;
            for (int i = 0; i < set->GetMaxIndex() && count < opt.max_elements; ++i)
            {
                if (!set->IsValidIndex(i)) continue;
                out.push_back(value_to_json(elem, set->GetData(i, layout), opt, depth_left));
                ++count;
            }
            return out;
        }
        if (cls == "MapProperty")
        {
            auto* mp = static_cast<FMapProperty*>(prop);
            auto* kp = mp->GetKeyProp();
            auto* vp = mp->GetValueProp();
            auto* map = static_cast<FScriptMap*>(ptr);
            auto layout = FScriptMap::GetScriptLayout(kp->GetSize(), kp->GetMinAlignment(), vp->GetSize(), vp->GetMinAlignment());
            json out = json::array();
            int count = 0;
            for (int i = 0; i < map->GetMaxIndex() && count < opt.max_elements; ++i)
            {
                if (!map->IsValidIndex(i)) continue;
                auto* pair = static_cast<std::uint8_t*>(map->GetData(i, layout));
                out.push_back(json{{"key", value_to_json(kp, pair, opt, depth_left)}, {"value", value_to_json(vp, pair + layout.ValueOffset, opt, depth_left)}});
                ++count;
            }
            return out;
        }
        // Delegates, field paths, optionals and anything new: UE's own text form.
        return export_text(prop, ptr);
    }

    json property_to_json(FProperty* prop, void* container, const MarshalOptions& opt, int depth_left)
    {
        int dim = prop->GetArrayDim();
        if (dim <= 1) return value_to_json(prop, prop->ContainerPtrToValuePtr<void>(container), opt, depth_left);
        json out = json::array();
        auto* base = static_cast<std::uint8_t*>(prop->ContainerPtrToValuePtr<void>(container));
        for (int i = 0; i < dim; ++i) out.push_back(value_to_json(prop, base + static_cast<std::size_t>(i) * prop->GetElementSize(), opt, depth_left));
        return out;
    }

    json dump_struct(UScriptStruct* type, void* data, const MarshalOptions& opt, int depth_left)
    {
        json out = json::object();
        for (UStruct* s : super_chain(type))
        {
            for (FProperty* p : TFieldRange<FProperty>(s, EFieldIterationFlags::IncludeDeprecated))
            {
                out[to_utf8(p->GetName())] = property_to_json(p, data, opt, depth_left);
            }
        }
        return out;
    }

    json dump_properties(UObject* object, const MarshalOptions& opt, const std::string& name_glob)
    {
        json out = json::object();
        for (UStruct* s : super_chain(object->GetClassPrivate()))
        {
            for (FProperty* p : TFieldRange<FProperty>(s, EFieldIterationFlags::IncludeDeprecated))
            {
                auto name = to_utf8(p->GetName());
                if (!name_glob.empty() && !glob_match(name_glob, name)) continue;
                if (out.contains(name)) continue; // shadowed by a more derived class
                out[name] = property_to_json(p, object, opt, opt.depth);
            }
        }
        return out;
    }

    // ------------------------------------------------------------------ write: JSON -> value

    static std::string json_to_ue_text(FProperty* prop, const json& v);

    static std::string struct_to_ue_text(UScriptStruct* type, const json& v)
    {
        if (v.is_string()) return v.get<std::string>();
        if (!v.is_object()) throw RpcError(InvalidParams, "struct " + to_utf8(type->GetName()) + " needs an object or a UE text string like (X=1,Y=2)");
        std::string out = "(";
        bool first = true;
        for (UStruct* s : super_chain(type))
        {
            for (FProperty* p : TFieldRange<FProperty>(s, EFieldIterationFlags::IncludeDeprecated))
            {
                auto name = to_utf8(p->GetName());
                const json* field = nullptr;
                if (v.contains(name)) field = &v[name];
                else
                {
                    // case-insensitive match (x vs X)
                    for (auto& [k, val] : v.items())
                        if (_stricmp(k.c_str(), name.c_str()) == 0) field = &val;
                }
                if (!field) continue;
                if (!first) out += ",";
                first = false;
                out += name + "=" + json_to_ue_text(p, *field);
            }
        }
        return out + ")";
    }

    static std::string json_to_ue_text(FProperty* prop, const json& v)
    {
        const std::string cls = prop_class(prop);
        if (v.is_null()) return "None";
        if (cls == "BoolProperty") return v.is_boolean() ? (v.get<bool>() ? "True" : "False") : (v.get<double>() != 0 ? "True" : "False");
        if (cls == "StrProperty" || cls == "NameProperty") return v.is_string() ? quote(v.get<std::string>()) : quote(v.dump());
        if (cls == "TextProperty") return "INVTEXT(" + quote(v.is_string() ? v.get<std::string>() : v.dump()) + ")";
        if (cls == "EnumProperty" || cls == "ByteProperty")
        {
            if (v.is_string()) return v.get<std::string>();
            return std::to_string(v.get<std::int64_t>());
        }
        if (cls == "ObjectProperty" || cls == "ObjectPtrProperty" || cls == "ClassProperty" || cls == "ClassPtrProperty" || cls == "WeakObjectProperty" || cls == "SoftObjectProperty" || cls == "SoftClassProperty" ||
            cls == "InterfaceProperty" || cls == "LazyObjectProperty")
        {
            if (v.is_object() && v.contains("path")) return v["path"].get<std::string>();
            if (v.is_string())
            {
                auto s = v.get<std::string>();
                if (s.rfind("h:", 0) == 0 || s.rfind("@0x", 0) == 0) return Handles::path_of(Handles::instance().resolve(s));
                return s;
            }
            throw RpcError(InvalidParams, "object value for " + to_utf8(prop->GetName()) + " must be a ref string or null");
        }
        if (cls == "StructProperty") return struct_to_ue_text(static_cast<FStructProperty*>(prop)->GetStruct(), v);
        if (cls == "ArrayProperty" || cls == "SetProperty")
        {
            if (v.is_string()) return v.get<std::string>();
            if (!v.is_array()) throw RpcError(InvalidParams, to_utf8(prop->GetName()) + " needs an array");
            FProperty* inner = cls == "ArrayProperty" ? static_cast<FArrayProperty*>(prop)->GetInner() : static_cast<FSetProperty*>(prop)->GetElementProp();
            std::string out = "(";
            for (std::size_t i = 0; i < v.size(); ++i)
            {
                if (i) out += ",";
                out += json_to_ue_text(inner, v[i]);
            }
            return out + ")";
        }
        if (cls == "MapProperty")
        {
            if (v.is_string()) return v.get<std::string>();
            auto* mp = static_cast<FMapProperty*>(prop);
            std::string out = "(";
            bool first = true;
            auto add = [&](const json& k, const json& val) {
                if (!first) out += ",";
                first = false;
                out += "(" + json_to_ue_text(mp->GetKeyProp(), k) + ", " + json_to_ue_text(mp->GetValueProp(), val) + ")";
            };
            if (v.is_array())
                for (auto& e : v) add(e.value("key", json(nullptr)), e.value("value", json(nullptr)));
            else if (v.is_object())
                for (auto& [k, val] : v.items()) add(json(k), val);
            return out + ")";
        }
        // numerics and everything else
        if (v.is_string()) return v.get<std::string>();
        return v.dump();
    }

    void json_to_value(FProperty* prop, void* ptr, const json& value, UObject* owner)
    {
        const std::string cls = prop_class(prop);
        // Fast typed paths
        if (cls == "Int8Property") { *static_cast<std::int8_t*>(ptr) = static_cast<std::int8_t>(value.get<std::int64_t>()); return; }
        if (cls == "Int16Property") { *static_cast<std::int16_t*>(ptr) = static_cast<std::int16_t>(value.get<std::int64_t>()); return; }
        if (cls == "IntProperty") { *static_cast<std::int32_t*>(ptr) = static_cast<std::int32_t>(value.get<std::int64_t>()); return; }
        if (cls == "Int64Property") { *static_cast<std::int64_t*>(ptr) = value.get<std::int64_t>(); return; }
        if (cls == "UInt16Property") { *static_cast<std::uint16_t*>(ptr) = static_cast<std::uint16_t>(value.get<std::uint64_t>()); return; }
        if (cls == "UInt32Property") { *static_cast<std::uint32_t*>(ptr) = static_cast<std::uint32_t>(value.get<std::uint64_t>()); return; }
        if (cls == "UInt64Property") { *static_cast<std::uint64_t*>(ptr) = value.get<std::uint64_t>(); return; }
        if (cls == "FloatProperty") { *static_cast<float*>(ptr) = value.get<float>(); return; }
        if (cls == "DoubleProperty") { *static_cast<double*>(ptr) = value.get<double>(); return; }
        if (cls == "ByteProperty")
        {
            UEnum* e = static_cast<FByteProperty*>(prop)->GetEnum();
            if (value.is_string())
            {
                auto ev = enum_value_for(e, value.get<std::string>());
                if (!ev) throw RpcError(InvalidParams, "unknown enum entry " + value.get<std::string>() + " for " + to_utf8(prop->GetName()));
                *static_cast<std::uint8_t*>(ptr) = static_cast<std::uint8_t>(*ev);
            }
            else *static_cast<std::uint8_t*>(ptr) = static_cast<std::uint8_t>(value.get<std::int64_t>());
            return;
        }
        if (cls == "EnumProperty")
        {
            auto* ep = static_cast<FEnumProperty*>(prop);
            std::int64_t v;
            if (value.is_string())
            {
                auto ev = enum_value_for(ep->GetEnum(), value.get<std::string>());
                if (!ev) throw RpcError(InvalidParams, "unknown enum entry " + value.get<std::string>() + " for " + to_utf8(prop->GetName()));
                v = *ev;
            }
            else v = value.get<std::int64_t>();
            auto* under = ep->GetUnderlyingProperty();
            json_to_value(under, ptr, json(v), owner);
            return;
        }
        if (cls == "ObjectProperty" || cls == "ObjectPtrProperty" || cls == "ClassProperty" || cls == "ClassPtrProperty" || cls == "InterfaceProperty")
        {
            UObject* obj = nullptr;
            if (value.is_object() && value.contains("handle")) obj = Handles::instance().resolve(value["handle"].get<std::string>());
            else if (value.is_object() && value.contains("path")) obj = Handles::instance().resolve(value["path"].get<std::string>());
            else if (value.is_string()) obj = Handles::instance().resolve(value.get<std::string>());
            else if (!value.is_null()) throw RpcError(InvalidParams, "object value for " + to_utf8(prop->GetName()) + " must be a ref string or null");
            *static_cast<UObject**>(ptr) = obj;
            return;
        }
        if (cls == "NameProperty")
        {
            // Direct FName construction: avoids ImportText quoting rules for paths and names with special characters.
            std::string s = value.is_string() ? value.get<std::string>() : value.dump();
            *static_cast<FName*>(ptr) = s.empty() || s == "None" ? FName() : FName(to_wide(s), FNAME_Add);
            return;
        }
        if (cls == "StrProperty")
        {
            std::string s = value.is_string() ? value.get<std::string>() : value.dump();
            auto wide = to_wide(s);
            *static_cast<FString*>(ptr) = FString(reinterpret_cast<const TCHAR*>(wide.c_str()));
            return;
        }
        if (cls == "WeakObjectProperty")
        {
            UObject* obj = value.is_null() ? nullptr : Handles::instance().resolve(value.is_string() ? value.get<std::string>() : value.value("path", std::string{}));
            *static_cast<FWeakObjectPtr*>(ptr) = FWeakObjectPtr{obj};
            return;
        }
        // Everything else goes through UE's text importer, which constructs values correctly (strings, texts, structs, containers, soft paths).
        import_text(prop, ptr, json_to_ue_text(prop, value), owner);
    }

    // ------------------------------------------------------------------ describe

    json describe_struct_properties(UStruct* type, const std::string& name_glob)
    {
        json out = json::array();
        for (UStruct* s : super_chain(type))
        {
            auto owner = to_utf8(s->GetName());
            for (FProperty* p : TFieldRange<FProperty>(s, EFieldIterationFlags::IncludeDeprecated))
            {
                auto name = to_utf8(p->GetName());
                if (!name_glob.empty() && !glob_match(name_glob, name)) continue;
                json e{{"name", name}, {"type", type_name(p)}, {"offset", p->GetOffset_Internal()}, {"size", p->GetSize()}, {"owner", owner}, {"flags", property_flags(p)}};
                if (p->GetArrayDim() > 1) e["array_dim"] = p->GetArrayDim();
                out.push_back(std::move(e));
            }
        }
        return out;
    }

    json describe_function(UFunction* fn)
    {
        json params = json::array();
        json ret = nullptr;
        auto return_offset = fn->GetReturnValueOffset();
        for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
        {
            if (!p->HasAnyPropertyFlags(CPF_Parm)) continue;
            json e{{"name", to_utf8(p->GetName())}, {"type", type_name(p)}};
            json flags = json::array();
            if (p->HasAnyPropertyFlags(CPF_OutParm)) flags.push_back("out");
            if (p->HasAnyPropertyFlags(CPF_ReferenceParm)) flags.push_back("ref");
            if (p->HasAnyPropertyFlags(CPF_ConstParm)) flags.push_back("const");
            if (p->HasAnyPropertyFlags(CPF_ReturnParm) || (return_offset != 0xFFFF && p->GetOffset_Internal() == return_offset))
            {
                ret = e;
                continue;
            }
            e["flags"] = flags;
            params.push_back(std::move(e));
        }
        auto flags = static_cast<std::uint32_t>(fn->GetFunctionFlags());
        return json{{"name", to_utf8(fn->GetName())},
                    {"path", Handles::path_of(fn)},
                    {"native", (flags & 0x400) != 0},
                    {"static", (flags & 0x2000) != 0},
                    {"flags", function_flags(fn)},
                    {"params", params},
                    {"return", ret},
                    {"parms_size", fn->GetParmsSize()}};
    }

    json describe_functions(UClass* type, const std::string& name_glob)
    {
        json out = json::array();
        // Native classes here have empty Children lists; collect their UFunctions by outer with one pass over the object array.
        std::unordered_map<UStruct*, std::vector<UFunction*>> by_outer;
        for (UStruct* s : super_chain(type)) by_outer[s] = {};
        UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
            if (object && object->IsA<UFunction>())
                if (auto it = by_outer.find(static_cast<UStruct*>(object->GetOuterPrivate())); it != by_outer.end()) it->second.push_back(static_cast<UFunction*>(object));
            return RC::LoopAction::Continue;
        });
        for (UStruct* s : super_chain(type))
        {
            auto owner = to_utf8(s->GetName());
            for (UFunction* fn : by_outer[s])
            {
                if (!fn) continue;
                auto name = to_utf8(fn->GetName());
                if (name_glob.empty() || glob_match(name_glob, name))
                {
                    json d = describe_function(fn);
                    d["owner"] = owner;
                    out.push_back(std::move(d));
                }
            }
        }
        return out;
    }

    // ------------------------------------------------------------------ call

    json call_function(UObject* context, UFunction* fn, const json& args, const MarshalOptions& opt)
    {
        if (!fn) throw RpcError(NotFound, "function is null");
        auto flags = static_cast<std::uint32_t>(fn->GetFunctionFlags());
        if (!context)
        {
            if (!(flags & 0x2000)) throw RpcError(InvalidParams, "non-static function " + to_utf8(fn->GetName()) + " needs a context object (ref)");
            auto* outer = fn->GetOuterPrivate();
            if (!outer || !outer->IsA<UClass>()) throw RpcError(NotFound, "cannot find the owning class of " + to_utf8(fn->GetName()));
            context = static_cast<UClass*>(outer)->GetClassDefaultObject();
        }

        const auto parms_size = fn->GetParmsSize();
        std::vector<std::uint8_t> buffer(parms_size > 0 ? parms_size : 1, 0);
        void* parms = buffer.data();
        const auto return_offset = fn->GetReturnValueOffset();

        struct Param
        {
            FProperty* prop;
            bool is_return;
            bool is_out;
        };
        std::vector<Param> params;
        for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
        {
            if (!p->HasAnyPropertyFlags(CPF_Parm)) continue;
            bool is_return = p->HasAnyPropertyFlags(CPF_ReturnParm) || (return_offset != 0xFFFF && p->GetOffset_Internal() == return_offset);
            bool is_out = !is_return && p->HasAnyPropertyFlags(CPF_OutParm) && !p->HasAnyPropertyFlags(CPF_ConstParm);
            params.push_back({p, is_return, is_out});
        }

        // Construct every parameter so FString/FText/TArray/struct values are valid even when not supplied.
        for (auto& prm : params) prm.prop->InitializeValue(prm.prop->ContainerPtrToValuePtr<void>(parms));

        struct Cleanup
        {
            std::vector<Param>& params;
            void* parms;
            ~Cleanup()
            {
                for (auto& prm : params) prm.prop->DestroyValue(prm.prop->ContainerPtrToValuePtr<void>(parms));
            }
        } cleanup{params, parms};

        // Fill inputs by name (object) or position (array). Out-only params may also be seeded.
        std::size_t positional = 0;
        for (auto& prm : params)
        {
            if (prm.is_return) continue;
            auto name = to_utf8(prm.prop->GetName());
            const json* value = nullptr;
            if (args.is_object())
            {
                if (args.contains(name)) value = &args[name];
                else
                    for (auto& [k, v] : args.items())
                        if (_stricmp(k.c_str(), name.c_str()) == 0) value = &v;
            }
            else if (args.is_array())
            {
                if (positional < args.size()) value = &args[positional];
                ++positional;
            }
            if (!value) continue;
            json_to_value(prm.prop, prm.prop->ContainerPtrToValuePtr<void>(parms), *value, context);
        }
        if (args.is_object())
        {
            for (auto& [k, v] : args.items())
            {
                if (k == "timeout_ms" || k == "ref" || k == "function" || k == "args" || k == "depth") continue;
                bool known = false;
                for (auto& prm : params)
                    if (_stricmp(to_utf8(prm.prop->GetName()).c_str(), k.c_str()) == 0) known = true;
                if (!known) throw RpcError(InvalidParams, "unknown parameter '" + k + "' for " + to_utf8(fn->GetName()), json{{"function", describe_function(fn)}});
            }
        }

        context->ProcessEvent(fn, parms);

        json result{{"return", nullptr}, {"out", json::object()}};
        for (auto& prm : params)
        {
            if (prm.is_return) result["return"] = property_to_json(prm.prop, parms, opt, opt.depth);
            else if (prm.is_out) result["out"][to_utf8(prm.prop->GetName())] = property_to_json(prm.prop, parms, opt, opt.depth);
        }
        return result;
    }

    // ------------------------------------------------------------------ diff

    json json_diff(const json& a, const json& b, const std::string& path)
    {
        json out = json::array();
        if (a.is_object() && b.is_object())
        {
            for (auto& [k, va] : a.items())
            {
                auto sub = path.empty() ? k : path + "." + k;
                if (!b.contains(k)) out.push_back(json{{"path", sub}, {"before", va}, {"after", nullptr}, {"removed", true}});
                else
                {
                    auto d = json_diff(va, b[k], sub);
                    out.insert(out.end(), d.begin(), d.end());
                }
            }
            for (auto& [k, vb] : b.items())
                if (!a.contains(k)) out.push_back(json{{"path", path.empty() ? k : path + "." + k}, {"before", nullptr}, {"after", vb}, {"added", true}});
            return out;
        }
        if (a.is_array() && b.is_array() && a.size() == b.size())
        {
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                auto d = json_diff(a[i], b[i], path + "[" + std::to_string(i) + "]");
                out.insert(out.end(), d.begin(), d.end());
            }
            return out;
        }
        if (a != b) out.push_back(json{{"path", path}, {"before", a}, {"after", b}});
        return out;
    }
} // namespace ue5mcp::bridge
