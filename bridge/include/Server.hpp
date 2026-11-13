// Loopback-only TCP listener. One reader thread per connection; requests are dispatched through BridgeMod.
#pragma once

#include <atomic>
#include <thread>

#include "BridgeCommon.hpp"

namespace ue5mcp::bridge
{
    class Server
    {
      public:
        Server(std::uint16_t port, int max_connections);
        ~Server();

        bool start(); // binds 127.0.0.1:port; returns false on failure (logged)
        void stop();
        std::uint16_t port() const { return m_port; }
        int connections() const { return m_connections.load(); }

      private:
        std::uint16_t m_port;
        int m_max_connections;
        std::uintptr_t m_listener{~static_cast<std::uintptr_t>(0)};
        std::atomic_bool m_running{false};
        std::atomic_int m_connections{0};
        std::thread m_accept_thread;
        std::mutex m_threads_mutex;
        std::vector<std::thread> m_client_threads;

        void accept_loop();
        void client_loop(std::uintptr_t socket);
    };
} // namespace ue5mcp::bridge
