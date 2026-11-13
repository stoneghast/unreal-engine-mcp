// UE4SS C++ mod entry points.
#include "Bridge.hpp"

#define UE5MCP_API __declspec(dllexport)
extern "C"
{
    UE5MCP_API RC::CppUserModBase* start_mod()
    {
        return new ue5mcp::bridge::BridgeMod();
    }

    UE5MCP_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
