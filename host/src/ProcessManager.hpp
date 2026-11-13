#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <ue5mcp/protocol.hpp>

namespace ue5mcp::host
{
    // Owns (or attaches to) the game process.
    class ProcessManager
    {
      public:
        ~ProcessManager();

        // Launch the executable; returns pid. Throws RpcError on failure.
        std::uint32_t launch(const std::filesystem::path& exe, const std::vector<std::wstring>& args);

        // Attach to an already-running process by executable file name (e.g. from a previous host session).
        bool attach_by_exe_name(const std::wstring& exe_name);

        bool running();
        std::uint32_t pid() const { return m_pid; }
        std::optional<std::uint32_t> exit_code();
        bool wait_exit(int timeout_ms);
        void kill();
        bool kill_requested() const { return m_kill_requested; }
        double uptime_seconds() const;

        // True once after the process has gone away without kill() having been called.
        bool take_unexpected_exit();

      private:
        void* m_process{nullptr};
        std::uint32_t m_pid{0};
        bool m_kill_requested{false};
        bool m_exit_reported{false};
        std::chrono::steady_clock::time_point m_started{};
        void close_handle();
    };

    // Find the newest file matching a prefix in a directory, or empty.
    std::filesystem::path newest_file(const std::filesystem::path& dir, const std::wstring& prefix, const std::wstring& ext);
    // Last N lines of a text file (UTF-8 or UTF-16LE with BOM, as UE4SS writes).
    std::vector<std::string> tail_file(const std::filesystem::path& file, std::size_t lines, std::uint64_t* cursor_in_out = nullptr);
} // namespace ue5mcp::host
