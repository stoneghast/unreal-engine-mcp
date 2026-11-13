#include "Session.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace ue5mcp::host
{
    std::string timestamp_now(bool for_filename)
    {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto t = system_clock::to_time_t(now);
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
        std::tm tm{};
        localtime_s(&tm, &t);
        char buf[64];
        if (for_filename)
            std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d-%03lld", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(ms));
        else
            std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%03lld", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(ms));
        return buf;
    }

    Session::Session(fs::path session_root, std::size_t page_bytes) : m_page_bytes(page_bytes)
    {
        m_dir = session_root / timestamp_now(true);
        std::error_code ec;
        fs::create_directories(m_dir, ec);
    }

    void Session::record_command(const std::string& tool, const json& args)
    {
        std::lock_guard lock(m_mutex);
        m_commands.push_back(json{{"t", timestamp_now(false)}, {"tool", tool}, {"args", args}});
        while (m_commands.size() > 50) m_commands.pop_front();
    }

    json Session::last_commands() const
    {
        std::lock_guard lock(m_mutex);
        return json(std::vector<json>(m_commands.begin(), m_commands.end()));
    }

    json Session::store_result(const std::string& tool, const json& result, bool is_error)
    {
        std::lock_guard lock(m_mutex);
        auto id = std::to_string(++m_seq);
        auto file = m_dir / (timestamp_now(true) + "-" + id + "-" + tool + (is_error ? ".error.json" : ".json"));
        std::string text = result.dump(1);
        {
            std::ofstream out(file, std::ios::binary);
            out << text;
        }
        if (text.size() <= m_page_bytes) return result;

        // Paginate: the page content is the raw JSON text, sliced on bytes. The agent sees it as text.
        return json{{"truncated", true},
                    {"result_id", id},
                    {"total_bytes", text.size()},
                    {"page_bytes", m_page_bytes},
                    {"file", file.string()},
                    {"hint", "Call session_page with result_id and offset to read the rest, or open the file."},
                    {"page", text.substr(0, m_page_bytes)}};
    }

    json Session::page(const std::string& result_id, std::size_t offset, std::size_t limit) const
    {
        std::lock_guard lock(m_mutex);
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(m_dir, ec))
        {
            auto name = entry.path().filename().string();
            // <ts>-<id>-<tool>.json  -> second dash-separated field is the id
            auto first = name.find('-', 16); // skip yyyymmdd-hhmmss-mmm
            if (first == std::string::npos) continue;
            auto second = name.find('-', first + 1);
            if (second == std::string::npos) continue;
            if (name.substr(first + 1, second - first - 1) != result_id) continue;
            std::ifstream in(entry.path(), std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)), {});
            if (offset >= text.size()) return json{{"page", ""}, {"offset", offset}, {"total_bytes", text.size()}, {"eof", true}};
            auto n = std::min(limit == 0 ? m_page_bytes : limit, text.size() - offset);
            return json{{"page", text.substr(offset, n)}, {"offset", offset}, {"next", offset + n}, {"total_bytes", text.size()}, {"eof", offset + n >= text.size()}};
        }
        throw RpcError(NotFound, "no stored result with id " + result_id);
    }

    json Session::list_results(std::size_t limit) const
    {
        std::lock_guard lock(m_mutex);
        std::vector<fs::directory_entry> entries;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(m_dir, ec)) entries.push_back(e);
        std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.path().filename() > b.path().filename(); });
        json out = json::array();
        for (auto& e : entries)
        {
            if (out.size() >= limit) break;
            out.push_back(json{{"file", e.path().string()}, {"bytes", e.file_size(ec)}});
        }
        return out;
    }

    fs::path Session::write_crash_report(const json& report)
    {
        auto file = m_dir / ("crash-" + timestamp_now(true) + ".json");
        std::ofstream out(file, std::ios::binary);
        out << report.dump(1);
        return file;
    }
} // namespace ue5mcp::host
