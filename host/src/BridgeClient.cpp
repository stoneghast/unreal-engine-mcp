#include "BridgeClient.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdio>

namespace ue5mcp::host
{
    namespace
    {
        struct WsaInit
        {
            WsaInit()
            {
                WSADATA d;
                WSAStartup(MAKEWORD(2, 2), &d);
            }
            ~WsaInit() { WSACleanup(); }
        };
        WsaInit& wsa()
        {
            static WsaInit w;
            return w;
        }
    } // namespace

    BridgeClient::BridgeClient(std::uint16_t port, int connect_timeout_ms) : m_port(port), m_connect_timeout_ms(connect_timeout_ms)
    {
        wsa();
    }

    BridgeClient::~BridgeClient()
    {
        close();
    }

    void BridgeClient::close()
    {
        if (m_socket != invalid_socket)
        {
            closesocket(static_cast<SOCKET>(m_socket));
            m_socket = invalid_socket;
        }
        m_rx.clear();
    }

    bool BridgeClient::ensure_connected()
    {
        if (connected()) return true;
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;

        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_port);
        inet_pton(AF_INET, kLoopback, &addr.sin_addr);
        int rc = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
        {
            closesocket(s);
            return false;
        }
        fd_set wset, eset;
        FD_ZERO(&wset);
        FD_ZERO(&eset);
        FD_SET(s, &wset);
        FD_SET(s, &eset);
        timeval tv{m_connect_timeout_ms / 1000, (m_connect_timeout_ms % 1000) * 1000};
        rc = select(0, nullptr, &wset, &eset, &tv);
        if (rc <= 0 || FD_ISSET(s, &eset))
        {
            closesocket(s);
            return false;
        }
        nonblocking = 0;
        ioctlsocket(s, FIONBIO, &nonblocking);
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
        m_socket = static_cast<std::uintptr_t>(s);
        m_rx.clear();
        refresh_describe();
        return true;
    }

    bool BridgeClient::send_line(const std::string& line)
    {
        std::string data = line + "\n";
        const char* p = data.data();
        int left = static_cast<int>(data.size());
        while (left > 0)
        {
            int n = send(static_cast<SOCKET>(m_socket), p, left, 0);
            if (n <= 0) return false;
            p += n;
            left -= n;
        }
        return true;
    }

    bool BridgeClient::read_line(std::string& line, int timeout_ms)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;)
        {
            if (take_line(m_rx, line)) return true;
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            fd_set rset;
            FD_ZERO(&rset);
            FD_SET(static_cast<SOCKET>(m_socket), &rset);
            timeval tv{static_cast<long>(remaining / 1000), static_cast<long>((remaining % 1000) * 1000)};
            int rc = select(0, &rset, nullptr, nullptr, &tv);
            if (rc <= 0) return false;
            char buf[65536];
            int n = recv(static_cast<SOCKET>(m_socket), buf, sizeof buf, 0);
            if (n <= 0)
            {
                close();
                return false;
            }
            m_rx.append(buf, static_cast<size_t>(n));
        }
    }

    json BridgeClient::call(const std::string& method, const json& params, int timeout_ms)
    {
        std::lock_guard lock(m_mutex);
        if (!ensure_connected()) throw RpcError(BridgeDisconnected, "bridge not connected on 127.0.0.1:" + std::to_string(m_port) + " (game not running, or the UE5MCP mod is not loaded)");
        auto id = m_next_id++;
        json p = params.is_object() ? params : json::object();
        if (!p.contains("timeout_ms")) p["timeout_ms"] = timeout_ms;
        if (!send_line(make_request(id, method, p).dump()))
        {
            close();
            throw RpcError(BridgeDisconnected, "send failed; bridge connection lost");
        }
        // Allow the bridge its own timeout plus slack for transport.
        int wait_ms = timeout_ms + 3000;
        std::string line;
        for (;;)
        {
            if (!read_line(line, wait_ms))
            {
                if (!connected()) throw RpcError(BridgeDisconnected, "connection closed while waiting for " + method + " (did the game crash?)");
                close();
                throw RpcError(Timeout, "no response from bridge within " + std::to_string(wait_ms) + " ms for " + method);
            }
            json msg;
            try
            {
                msg = json::parse(line);
            }
            catch (...)
            {
                continue; // skip garbage lines
            }
            if (!msg.contains("id") || msg["id"] != id) continue; // not ours (notification or stale)
            if (msg.contains("error"))
            {
                auto& e = msg["error"];
                throw RpcError(e.value("code", static_cast<int>(InternalError)), e.value("message", std::string("bridge error")), e.contains("data") ? e["data"] : json(nullptr));
            }
            return msg.contains("result") ? msg["result"] : json(nullptr);
        }
    }

    bool BridgeClient::refresh_describe()
    {
        try
        {
            // Direct send without the public lock (called from ensure_connected which runs under call's lock or standalone).
            auto id = m_next_id++;
            if (!send_line(make_request(id, "bridge_describe", json::object()).dump())) return false;
            std::string line;
            while (read_line(line, 5000))
            {
                json msg = json::parse(line, nullptr, false);
                if (msg.is_discarded() || !msg.contains("id") || msg["id"] != id) continue;
                if (!msg.contains("result")) return false;
                m_describe = msg["result"];
                m_tools.clear();
                for (auto& t : m_describe.value("tools", json::array())) m_tools.push_back(ToolSpec::from_json(t));
                return true;
            }
        }
        catch (...)
        {
        }
        return false;
    }
} // namespace ue5mcp::host
