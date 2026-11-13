// FProperty <-> JSON marshalling, struct/class description, and the UFunction call builder.
#pragma once

#include "BridgeCommon.hpp"

#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UScriptStruct.hpp>
#include <Unreal/FProperty.hpp>

namespace ue5mcp::bridge
{
    namespace UE = RC::Unreal;

    struct MarshalOptions
    {
        int depth{2};            // struct/array recursion depth
        int max_elements{64};    // array/map/set elements per container
        bool enum_names{true};   // enums as names
        bool object_refs{true};  // objects as {path,handle,class}; false = path string only
    };

    // Read one property value out of a container (object or struct memory).
    json property_to_json(UE::FProperty* prop, void* container, const MarshalOptions& opt, int depth_left);
    // Read a raw value pointer (already offset) of the given property type.
    json value_to_json(UE::FProperty* prop, void* value_ptr, const MarshalOptions& opt, int depth_left);

    // Write JSON into a property value pointer. Throws RpcError(UnsupportedType / InvalidParams).
    void json_to_value(UE::FProperty* prop, void* value_ptr, const json& value, UE::UObject* owner_for_import);

    // Human-readable type name: "Int", "Struct<Vector>", "Array<Object<Actor>>", "Enum<E_Gait>" ...
    std::string type_name(UE::FProperty* prop);
    json property_flags(UE::FProperty* prop);
    json function_flags(UE::UFunction* fn);

    // All properties of an object: iterates the class chain (most derived first unless `parents_first`).
    json dump_properties(UE::UObject* object, const MarshalOptions& opt, const std::string& name_glob);
    json dump_struct(UE::UScriptStruct* type, void* data, const MarshalOptions& opt, int depth_left);

    // Class description for class_describe.
    json describe_struct_properties(UE::UStruct* type, const std::string& name_glob);
    json describe_functions(UE::UClass* type, const std::string& name_glob);
    json describe_function(UE::UFunction* fn);
    std::vector<UE::UStruct*> super_chain(UE::UStruct* type); // type first, then supers

    // Call a UFunction with JSON args. Returns {"return": ..., "out": {...}}.
    json call_function(UE::UObject* context, UE::UFunction* fn, const json& args, const MarshalOptions& opt);

    // Enum helpers
    std::string enum_name_for(UE::UEnum* e, std::int64_t value);
    std::optional<std::int64_t> enum_value_for(UE::UEnum* e, const std::string& name);

    // JSON diff for snapshots: list of {path, before, after}
    json json_diff(const json& a, const json& b, const std::string& path = "");
} // namespace ue5mcp::bridge
