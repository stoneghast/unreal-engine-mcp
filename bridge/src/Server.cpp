#include "Server.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "Bridge.hpp"

namespace ue5mcp::bridge
{
    Server::Server(std::uint16_t port, int max_connections) : m_port(port), m_max_connections(max_connections)
    {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }

    Server::~Server()
    {
        stop();
        WSACleanup();
    }

    bool Server::start()
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
        {
            logf("socket() failed: %d", WSAGetLastError());
            return false;
        }
        // Exclusive use: no other process may bind the same port, and we never bind anything but loopback.
        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_port);
        inet_pton(AF_INET, kLoopback, &addr.sin_addr);
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR)
        {
            logf("bind(127.0.0.1:%u) failed: %d (another game instance with the bridge running?)", m_port, WSAGetLastError());
            closesocket(s);
            return false;
        }
        if (listen(s, 4) == SOCKET_ERROR)
        {
            logf("listen failed: %d", WSAGetLastError());
            closesocket(s);
            return false;
        }
        m_listener = static_cast<std::uintptr_t>(s);
        m_running = true;
        m_accept_thread = std::thread([this] { accept_loop(); });
        logf("listening on 127.0.0.1:%u", m_port);
        return true;
    }

    void Server::stop()
    {
        if (!m_running.exchange(false)) return;
        if (m_listener != ~static_cast<std::uintptr_t>(0))
        {
            closesocket(static_cast<SOCKET>(m_listener));
            m_listener = ~static_cast<std::uintptr_t>(0);
        }
        if (m_accept_thread.joinable()) m_accept_thread.join();
        std::lock_guard lock(m_threads_mutex);
        for (auto& t : m_client_threads)
            if (t.joinable()) t.detach(); // readers exit when their socket errors; do not block unload on them
        m_client_threads.clear();
    }

    void Server::accept_loop()
    {
        while (m_running)
        {
            sockaddr_in peer{};
            int len = sizeof peer;
            SOCKET c = accept(static_cast<SOCKET>(m_listener), reinterpret_cast<sockaddr*>(&peer), &len);
            if (c == INVALID_SOCKET) break;
            if (peer.sin_addr.S_un.S_addr != htonl(INADDR_LOOPBACK) || m_connections >= m_max_connections)
            {
                closesocket(c);
                continue;
            }
            BOOL nodelay = TRUE;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
            ++m_connections;
            std::lock_guard lock(m_threads_mutex);
            m_client_threads.emplace_back([this, c] { client_loop(static_cast<std::uintptr_t>(c)); });
        }
    }

    void Server::client_loop(std::uintptr_t socket)
    {
        SOCKET s = static_cast<SOCKET>(socket);
        std::string rx;
        std::string line;
        char buf[65536];
        log("client connected");
        while (m_running)
        {
            int n = recv(s, buf, sizeof buf, 0);
            if (n <= 0) break;
            rx.append(buf, static_cast<size_t>(n));
            while (take_line(rx, line))
            {
                if (line.empty()) continue;
                json request = json::parse(line, nullptr, false);
                json response = request.is_discarded() ? make_error(nullptr, ParseError, "parse error") : BridgeMod::instance()->dispatch(request);
                std::string out = response.dump();
                out.push_back('\n');
                const char* p = out.data();
                int left = static_cast<int>(out.size());
                while (left > 0)
                {
                    int sent = send(s, p, left, 0);
                    if (sent <= 0)
                    {
                        left = -1;
                        break;
                    }
                    p += sent;
                    left -= sent;
                }
                if (left < 0) break;
            }
        }
        closesocket(s);
        --m_connections;
        log("client disconnected");
    }
} // namespace ue5mcp::bridge
