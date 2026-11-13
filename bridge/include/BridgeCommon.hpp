// Shared includes and small helpers for the bridge. Everything here is UE4SS/Unreal-facing.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ue5mcp/protocol.hpp>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <String/StringType.hpp>

namespace ue5mcp::bridge
{
    using json = nlohmann::json;
    using RC::StringType;
    using RC::StringViewType;

    // UE4SS StringType is UTF-16 on Windows; the wire is UTF-8.
    std::string to_utf8(StringViewType s);
    std::string to_utf8(const wchar_t* s);
    StringType to_wide(std::string_view s);

    inline std::int64_t now_ms()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // Bridge-side logging: goes to the UE4SS console/log and to our own ring + file.
    void log(const std::string& line);
    void logf(const char* fmt, ...);
    std::vector<std::string> log_ring(std::size_t max_lines);

    // Param helpers that throw InvalidParams with the field name.
    template <typename T>
    T need(const json& p, const char* key)
    {
        if (!p.contains(key)) throw RpcError(InvalidParams, std::string("missing required param '") + key + "'");
        try
        {
            return p[key].get<T>();
        }
        catch (const std::exception&)
        {
            throw RpcError(InvalidParams, std::string("param '") + key + "' has the wrong type");
        }
    }
    template <typename T>
    T opt(const json& p, const char* key, T def)
    {
        if (!p.contains(key) || p[key].is_null()) return def;
        try
        {
            return p[key].get<T>();
        }
        catch (const std::exception&)
        {
            throw RpcError(InvalidParams, std::string("param '") + key + "' has the wrong type");
        }
    }
} // namespace ue5mcp::bridge
