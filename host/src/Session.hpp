#pragma once

#include <deque>
#include <filesystem>
#include <mutex>
#include <string>

#include <ue5mcp/protocol.hpp>

namespace ue5mcp::host
{
    // Persists every tool result under <session_dir>/<session-id>/, keeps the last commands for crash
    // reports, and paginates results that are too large for one MCP response.
    class Session
    {
      public:
        Session(std::filesystem::path session_root, std::size_t page_bytes);

        const std::filesystem::path& dir() const { return m_dir; }

        // Record a command before it is sent (so a crash mid-call still shows it).
        void record_command(const std::string& tool, const json& args);

        // Store a result; returns the JSON that should go back to the client (paginated if needed).
        json store_result(const std::string& tool, const json& result, bool is_error);

        json page(const std::string& result_id, std::size_t offset, std::size_t limit) const;
        json list_results(std::size_t limit) const;

        json last_commands() const;

        // Write a crash report with whatever context is available.
        std::filesystem::path write_crash_report(const json& report);

      private:
        std::filesystem::path m_dir;
        std::size_t m_page_bytes;
        mutable std::mutex m_mutex;
        std::deque<json> m_commands;
        std::uint64_t m_seq{0};
    };

    std::string timestamp_now(bool for_filename);
} // namespace ue5mcp::host
