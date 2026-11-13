// Shared wire-protocol definitions for the ue5mcp host and bridge.
// Transport: newline-delimited JSON-RPC 2.0 over TCP on 127.0.0.1.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ue5mcp
{
    using json = nlohmann::json;

    constexpr std::uint16_t kDefaultPort = 19777;
    constexpr const char* kLoopback = "127.0.0.1";
    constexpr int kDefaultTimeoutMs = 5000;

    // JSON-RPC and bridge error codes (see docs/PROTOCOL.md)
    enum ErrorCode : int
    {
        ParseError = -32700,
        InvalidRequest = -32600,
        MethodNotFound = -32601,
        InvalidParams = -32602,
        InternalError = -32603,

        Timeout = 1001,
        GameThreadUnavailable = 1002,
        NotFound = 1003,
        ExecutionException = 1004,
        UnsupportedType = 1005,
        LuaError = 1006,
        Unavailable = 1007,
        BridgeDisconnected = 1008, // host-only: no bridge connection
    };

    // Thrown by tool implementations; converted into a JSON-RPC error object.
    struct RpcError
    {
        int code{InternalError};
        std::string message;
        json data{nullptr};

        RpcError(int c, std::string m, json d = nullptr) : code(c), message(std::move(m)), data(std::move(d)) {}
    };

    inline json make_error(const json& id, int code, const std::string& message, const json& data = nullptr)
    {
        json err{{"code", code}, {"message", message}};
        if (!data.is_null()) err["data"] = data;
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", err}};
    }

    inline json make_result(const json& id, json result)
    {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
    }

    inline json make_request(std::int64_t id, std::string_view method, json params)
    {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", std::string(method)}, {"params", std::move(params)}};
    }

    // A tool as advertised by bridge_describe and by the host over MCP.
    struct ToolSpec
    {
        std::string name;
        std::string description;
        json input_schema; // JSON Schema object
        bool game_thread{false};

        json to_json() const
        {
            return json{{"name", name}, {"description", description}, {"inputSchema", input_schema}, {"gameThread", game_thread}};
        }
        static ToolSpec from_json(const json& j)
        {
            ToolSpec t;
            t.name = j.value("name", "");
            t.description = j.value("description", "");
            t.input_schema = j.contains("inputSchema") ? j["inputSchema"] : json{{"type", "object"}};
            // MCP clients require `properties` to be an object; an empty brace-init list serialises as null.
            if (t.input_schema.is_object() && t.input_schema.contains("properties") && t.input_schema["properties"].is_null())
                t.input_schema["properties"] = json::object();
            t.game_thread = j.value("gameThread", false);
            return t;
        }
    };

    // Small helpers for building JSON Schema inline.
    namespace schema
    {
        inline json obj(json properties, std::vector<std::string> required = {})
        {
            if (properties.is_null()) properties = json::object(); // obj({}) must mean "no parameters", not null
            json s{{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", true}};
            if (!required.empty()) s["required"] = required;
            return s;
        }
        inline json str(const char* desc, std::optional<std::string> def = {})
        {
            json s{{"type", "string"}, {"description", desc}};
            if (def) s["default"] = *def;
            return s;
        }
        inline json num(const char* desc, std::optional<double> def = {})
        {
            json s{{"type", "number"}, {"description", desc}};
            if (def) s["default"] = *def;
            return s;
        }
        inline json integer(const char* desc, std::optional<std::int64_t> def = {})
        {
            json s{{"type", "integer"}, {"description", desc}};
            if (def) s["default"] = *def;
            return s;
        }
        inline json boolean(const char* desc, std::optional<bool> def = {})
        {
            json s{{"type", "boolean"}, {"description", desc}};
            if (def) s["default"] = *def;
            return s;
        }
        inline json arr(json items, const char* desc)
        {
            return json{{"type", "array"}, {"items", std::move(items)}, {"description", desc}};
        }
        inline json any(const char* desc)
        {
            return json{{"description", desc}};
        }
        inline json ref(const char* desc = "Object reference: handle 'h:N', full object path, or '@0x...' pointer")
        {
            return str(desc);
        }
        inline json timeout()
        {
            return integer("Milliseconds to wait for the game thread (default 5000)", kDefaultTimeoutMs);
        }
        inline json vec3(const char* desc)
        {
            return json{{"type", "object"}, {"description", desc},
                        {"properties", {{"x", {{"type", "number"}}}, {"y", {{"type", "number"}}}, {"z", {{"type", "number"}}}}}};
        }
        inline json rot(const char* desc)
        {
            return json{{"type", "object"}, {"description", desc},
                        {"properties", {{"pitch", {{"type", "number"}}}, {"yaw", {{"type", "number"}}}, {"roll", {{"type", "number"}}}}}};
        }
    } // namespace schema

    // Line framing helpers: find complete lines in a receive buffer.
    inline bool take_line(std::string& buffer, std::string& line)
    {
        auto pos = buffer.find('\n');
        if (pos == std::string::npos) return false;
        line.assign(buffer, 0, pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        buffer.erase(0, pos + 1);
        return true;
    }

    // Glob match: * ? and case-insensitive ASCII. Used for name filters everywhere.
    inline bool glob_match(std::string_view pattern, std::string_view text)
    {
        size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
        auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; };
        while (t < text.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' || lower(pattern[p]) == lower(text[t])))
            {
                ++p; ++t;
            }
            else if (p < pattern.size() && pattern[p] == '*')
            {
                star = p++; mark = t;
            }
            else if (star != std::string_view::npos)
            {
                p = star + 1; t = ++mark;
            }
            else return false;
        }
        while (p < pattern.size() && pattern[p] == '*') ++p;
        return p == pattern.size();
    }
} // namespace ue5mcp
