#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <ue5mcp/protocol.hpp>

namespace ue5mcp::host
{
    // One TCP connection to the in-game bridge. Synchronous request/response; reconnects on demand.
    class BridgeClient
    {
      public:
        BridgeClient(std::uint16_t port, int connect_timeout_ms);
        ~BridgeClient();

        bool connected() const { return m_socket != invalid_socket; }
        bool ensure_connected(); // tries to connect if not connected; false if the bridge is not listening
        void close();

        // Throws RpcError on transport failure or when the bridge returns an error.
        json call(const std::string& method, const json& params, int timeout_ms);

        // Cached tool list from bridge_describe (refreshed on connect).
        const std::vector<ToolSpec>& tools() const { return m_tools; }
        const json& describe_info() const { return m_describe; }
        bool refresh_describe();

      private:
        static constexpr std::uintptr_t invalid_socket = ~static_cast<std::uintptr_t>(0);
        std::uint16_t m_port;
        int m_connect_timeout_ms;
        std::uintptr_t m_socket{invalid_socket};
        std::string m_rx;
        std::int64_t m_next_id{1};
        std::mutex m_mutex;
        std::vector<ToolSpec> m_tools;
        json m_describe;

        bool send_line(const std::string& line);
        bool read_line(std::string& line, int timeout_ms);
    };
} // namespace ue5mcp::host
