// Stable object handles and ref resolution. All functions must be called on the game thread.
#pragma once

#include <unordered_map>

#include "BridgeCommon.hpp"

#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/FWeakObjectPtr.hpp>

namespace ue5mcp::bridge
{
    namespace UE = RC::Unreal;

    class Handles
    {
      public:
        static Handles& instance();

        // Returns "h:N" for a live object; the same object always gets the same handle.
        std::string handle_for(UE::UObject* object);

        // Resolve any ref form. Throws RpcError(NotFound) when it does not resolve to a live object.
        UE::UObject* resolve(const std::string& ref);
        UE::UObject* try_resolve(const std::string& ref);

        // {"path","handle","class"} or null.
        json object_json(UE::UObject* object);

        // Full object path without the "Class " prefix that GetFullName() adds.
        static std::string path_of(UE::UObject* object);
        static std::string class_name_of(UE::UObject* object);

        bool is_live_pointer(void* ptr);
        void prune(); // drop dead handles

      private:
        std::mutex m_mutex;
        std::unordered_map<std::uint64_t, UE::FWeakObjectPtr> m_by_id;
        std::unordered_map<UE::UObject*, std::uint64_t> m_by_ptr;
        std::uint64_t m_next{1};
    };

    // Find a UClass by short name ("Actor"), by path ("/Script/Engine.Actor"), or by any ref whose class is used.
    UE::UClass* find_class(const std::string& name_or_path);
    // Find a UFunction: "Class:Func" path, "/Script/Pkg.Class:Func", or by name on an object's class chain.
    UE::UFunction* find_function(const std::string& spec, UE::UObject* context);
} // namespace ue5mcp::bridge
