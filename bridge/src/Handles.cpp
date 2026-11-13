#include "Handles.hpp"

#include <charconv>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UStruct.hpp>
#include <Unreal/NameTypes.hpp>
#pragma warning(push)
#pragma warning(disable : 4005)
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#pragma warning(pop)

namespace ue5mcp::bridge
{
    using namespace RC::Unreal;

    Handles& Handles::instance()
    {
        static Handles h;
        return h;
    }

    std::string Handles::path_of(UObject* object)
    {
        if (!object) return {};
        auto full = to_utf8(object->GetFullName()); // "Class /Path/To.Object:Sub"
        auto sp = full.find(' ');
        return sp == std::string::npos ? full : full.substr(sp + 1);
    }

    std::string Handles::class_name_of(UObject* object)
    {
        if (!object || !object->GetClassPrivate()) return {};
        return to_utf8(object->GetClassPrivate()->GetName());
    }

    std::string Handles::handle_for(UObject* object)
    {
        if (!object) return {};
        std::lock_guard lock(m_mutex);
        auto it = m_by_ptr.find(object);
        if (it != m_by_ptr.end())
        {
            auto& weak = m_by_id[it->second];
            if (weak.Get() == object) return "h:" + std::to_string(it->second);
            // address reused by a different object: drop the stale mapping
            m_by_id.erase(it->second);
            m_by_ptr.erase(it);
        }
        auto id = m_next++;
        m_by_id.emplace(id, FWeakObjectPtr{object});
        m_by_ptr.emplace(object, id);
        return "h:" + std::to_string(id);
    }

    json Handles::object_json(UObject* object)
    {
        if (!object) return nullptr;
        return json{{"path", path_of(object)}, {"handle", handle_for(object)}, {"class", class_name_of(object)}};
    }

    bool Handles::is_live_pointer(void* ptr)
    {
        bool found = false;
        UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
            if (object == ptr)
            {
                found = true;
                return RC::LoopAction::Break;
            }
            return RC::LoopAction::Continue;
        });
        return found;
    }

    void Handles::prune()
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_by_id.begin(); it != m_by_id.end();)
        {
            if (!it->second.Get())
            {
                for (auto p = m_by_ptr.begin(); p != m_by_ptr.end(); ++p)
                    if (p->second == it->first)
                    {
                        m_by_ptr.erase(p);
                        break;
                    }
                it = m_by_id.erase(it);
            }
            else ++it;
        }
    }

    UObject* Handles::try_resolve(const std::string& ref)
    {
        if (ref.empty()) return nullptr;
        if (ref.rfind("h:", 0) == 0)
        {
            std::uint64_t id = 0;
            auto [p, ec] = std::from_chars(ref.data() + 2, ref.data() + ref.size(), id);
            if (ec != std::errc{}) return nullptr;
            std::lock_guard lock(m_mutex);
            auto it = m_by_id.find(id);
            return it == m_by_id.end() ? nullptr : it->second.Get();
        }
        if (ref.rfind("@0x", 0) == 0 || ref.rfind("@0X", 0) == 0)
        {
            std::uint64_t addr = 0;
            auto [p, ec] = std::from_chars(ref.data() + 3, ref.data() + ref.size(), addr, 16);
            if (ec != std::errc{}) return nullptr;
            auto* ptr = reinterpret_cast<void*>(addr);
            return is_live_pointer(ptr) ? static_cast<UObject*>(ptr) : nullptr;
        }
        std::string path = ref;
        // Accept GetFullName() output ("Class /Path") by dropping the class prefix.
        if (auto sp = path.find(' '); sp != std::string::npos && path.find('/', sp) == sp + 1) path = path.substr(sp + 1);
        return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, to_wide(path));
    }

    UObject* Handles::resolve(const std::string& ref)
    {
        auto* obj = try_resolve(ref);
        if (!obj) throw RpcError(NotFound, "object not found or no longer alive: " + ref, json{{"ref", ref}});
        return obj;
    }

    // ------------------------------------------------------------------ class / function lookup

    UClass* find_class(const std::string& name_or_path)
    {
        if (name_or_path.empty()) return nullptr;
        if (name_or_path[0] == '/')
        {
            auto* obj = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, to_wide(name_or_path));
            if (obj && obj->IsA<UClass>()) return static_cast<UClass*>(obj);
            return nullptr;
        }
        // A ref (handle/pointer) resolves to its class, or to itself when it is a class.
        if (auto* obj = Handles::instance().try_resolve(name_or_path))
            return obj->IsA<UClass>() ? static_cast<UClass*>(obj) : obj->GetClassPrivate();

        // Short name: common native packages first, then a full scan (covers Blueprint classes like BP_X_C).
        static const char* packages[] = {"/Script/Engine.", "/Script/CoreUObject.", "/Script/UMG.", "/Script/GameplayAbilities.", "/Script/AIModule."};
        for (auto pkg : packages)
            if (auto* c = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, to_wide(std::string(pkg) + name_or_path))) return c;

        auto wanted = to_wide(name_or_path);
        UClass* found = nullptr;
        UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
            if (object && object->IsA<UClass>() && object->GetName() == wanted)
            {
                found = static_cast<UClass*>(object);
                return RC::LoopAction::Break;
            }
            return RC::LoopAction::Continue;
        });
        return found;
    }

    static UFunction* function_in_chain(UStruct* type, const StringType& name)
    {
        // Native classes in this engine build do not expose their UFunctions through the Children list, so a path lookup
        // (class path + ":" + name) is tried first for every struct in the chain; it resolves through the object hash.
        for (UStruct* s = type; s; s = s->GetSuperStruct())
        {
            auto path = to_wide(Handles::path_of(s) + ":") + name;
            if (auto* obj = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path); obj && obj->IsA<UFunction>()) return static_cast<UFunction*>(obj);
        }
        for (UStruct* s = type; s; s = s->GetSuperStruct())
        {
            for (UFunction* fn : TFieldRange<UFunction>(s, EFieldIterationFlags::None))
                if (fn && fn->GetName() == name) return fn;
            // Native interface functions (e.g. AssetRegistry on AssetRegistryImpl) live on the UInterface class, not the implementor.
            if (s->IsA<UClass>())
                for (auto& iface : static_cast<UClass*>(s)->GetInterfaces())
                    if (UClass* ic = iface.Class)
                        for (UFunction* fn : TFieldRange<UFunction>(ic, EFieldIterationFlags::None))
                            if (fn && fn->GetName() == name) return fn;
        }
        return nullptr;
    }

    UFunction* find_function(const std::string& spec, UObject* context)
    {
        if (spec.empty()) return nullptr;
        if (spec[0] == '/')
        {
            auto* obj = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, to_wide(spec));
            return obj && obj->IsA<UFunction>() ? static_cast<UFunction*>(obj) : nullptr;
        }
        auto colon = spec.find(':');
        if (colon != std::string::npos)
        {
            auto cls = find_class(spec.substr(0, colon));
            if (!cls) return nullptr;
            return function_in_chain(cls, to_wide(spec.substr(colon + 1)));
        }
        if (!context) return nullptr;
        UStruct* type = context->IsA<UClass>() ? static_cast<UStruct*>(context) : static_cast<UStruct*>(context->GetClassPrivate());
        return function_in_chain(type, to_wide(spec));
    }
} // namespace ue5mcp::bridge
