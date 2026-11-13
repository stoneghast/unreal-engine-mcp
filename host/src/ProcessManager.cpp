#include "ProcessManager.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <fstream>

namespace fs = std::filesystem;

namespace ue5mcp::host
{
    ProcessManager::~ProcessManager()
    {
        close_handle();
    }

    void ProcessManager::close_handle()
    {
        if (m_process)
        {
            CloseHandle(static_cast<HANDLE>(m_process));
            m_process = nullptr;
        }
    }

    std::uint32_t ProcessManager::launch(const fs::path& exe, const std::vector<std::wstring>& args)
    {
        if (exe.empty() || !fs::exists(exe)) throw RpcError(InvalidParams, "game executable not found: " + exe.string());
        if (running()) throw RpcError(InvalidParams, "game already running (pid " + std::to_string(m_pid) + ")");
        close_handle();

        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        for (auto& a : args)
        {
            cmd += L" ";
            bool quote = a.find(L' ') != std::wstring::npos && a.front() != L'"';
            cmd += quote ? L"\"" + a + L"\"" : a;
        }
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        std::wstring cwd = exe.parent_path().wstring();
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr, cwd.c_str(), &si, &pi))
        {
            throw RpcError(InternalError, "CreateProcess failed with error " + std::to_string(GetLastError()));
        }
        CloseHandle(pi.hThread);
        m_process = pi.hProcess;
        m_pid = pi.dwProcessId;
        m_kill_requested = false;
        m_exit_reported = false;
        m_started = std::chrono::steady_clock::now();
        return m_pid;
    }

    bool ProcessManager::attach_by_exe_name(const std::wstring& exe_name)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof pe;
        bool found = false;
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                if (_wcsicmp(pe.szExeFile, exe_name.c_str()) == 0)
                {
                    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (h)
                    {
                        close_handle();
                        m_process = h;
                        m_pid = pe.th32ProcessID;
                        m_kill_requested = false;
                        m_exit_reported = false;
                        m_started = std::chrono::steady_clock::now();
                        found = true;
                        break;
                    }
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return found;
    }

    bool ProcessManager::running()
    {
        if (!m_process) return false;
        return WaitForSingleObject(static_cast<HANDLE>(m_process), 0) == WAIT_TIMEOUT;
    }

    std::optional<std::uint32_t> ProcessManager::exit_code()
    {
        if (!m_process || running()) return std::nullopt;
        DWORD code = 0;
        if (!GetExitCodeProcess(static_cast<HANDLE>(m_process), &code)) return std::nullopt;
        return static_cast<std::uint32_t>(code);
    }

    bool ProcessManager::wait_exit(int timeout_ms)
    {
        if (!m_process) return true;
        return WaitForSingleObject(static_cast<HANDLE>(m_process), static_cast<DWORD>(timeout_ms)) == WAIT_OBJECT_0;
    }

    void ProcessManager::kill()
    {
        if (!m_process) return;
        m_kill_requested = true;
        TerminateProcess(static_cast<HANDLE>(m_process), 1);
        WaitForSingleObject(static_cast<HANDLE>(m_process), 5000);
    }

    double ProcessManager::uptime_seconds() const
    {
        if (!m_process) return 0;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count();
    }

    bool ProcessManager::take_unexpected_exit()
    {
        if (!m_process || running() || m_kill_requested || m_exit_reported) return false;
        m_exit_reported = true;
        return true;
    }

    fs::path newest_file(const fs::path& dir, const std::wstring& prefix, const std::wstring& ext)
    {
        fs::path best;
        fs::file_time_type best_time{};
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) return best;
        for (auto& e : fs::directory_iterator(dir, ec))
        {
            auto name = e.path().filename().wstring();
            if (name.rfind(prefix, 0) != 0) continue;
            if (!ext.empty() && e.path().extension().wstring() != ext) continue;
            auto t = e.last_write_time(ec);
            if (best.empty() || t > best_time)
            {
                best = e.path();
                best_time = t;
            }
        }
        return best;
    }

    std::vector<std::string> tail_file(const fs::path& file, std::size_t lines, std::uint64_t* cursor_in_out)
    {
        std::vector<std::string> out;
        std::ifstream in(file, std::ios::binary);
        if (!in) return out;
        in.seekg(0, std::ios::end);
        auto size = static_cast<std::uint64_t>(in.tellg());
        std::uint64_t start = 0;
        if (cursor_in_out && *cursor_in_out > 0 && *cursor_in_out <= size)
            start = *cursor_in_out;
        else if (!cursor_in_out || *cursor_in_out == 0)
        {
            // read at most 2 MB from the end when no cursor
            start = size > (2u << 20) ? size - (2u << 20) : 0;
        }
        in.seekg(static_cast<std::streamoff>(start));
        std::string data((std::istreambuf_iterator<char>(in)), {});
        if (cursor_in_out) *cursor_in_out = size;

        // UE4SS.log is UTF-8 in current builds; handle a UTF-16LE BOM defensively.
        if (start == 0 && data.size() >= 2 && static_cast<unsigned char>(data[0]) == 0xFF && static_cast<unsigned char>(data[1]) == 0xFE)
        {
            std::wstring w(reinterpret_cast<const wchar_t*>(data.data() + 2), (data.size() - 2) / 2);
            int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
            data = std::move(s);
        }
        std::size_t pos = 0;
        std::vector<std::string> all;
        while (pos < data.size())
        {
            auto nl = data.find('\n', pos);
            if (nl == std::string::npos) nl = data.size();
            std::string line = data.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!(start > 0 && all.empty() && pos == 0 && cursor_in_out == nullptr)) all.push_back(std::move(line));
            pos = nl + 1;
        }
        if (all.size() > lines) all.erase(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(all.size() - lines));
        return all;
    }
} // namespace ue5mcp::host
