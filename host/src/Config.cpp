#include "Config.hpp"

#include <windows.h>

#include <fstream>

namespace fs = std::filesystem;

namespace ue5mcp::host
{
    static std::wstring widen(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    static std::string narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    Config Config::defaults()
    {
        Config c;
        c.session_dir = fs::current_path() / "sessions";
        return c;
    }

    Config Config::load(const fs::path& file)
    {
        Config c = defaults();
        if (file.empty() || !fs::exists(file)) return c;
        std::ifstream in(file);
        json j = json::parse(in, nullptr, true, true);
        c.config_file = fs::absolute(file);
        auto base = c.config_file.parent_path();
        auto path_of = [&](const char* key) -> fs::path {
            if (!j.contains(key) || j[key].is_null()) return {};
            fs::path p = widen(j[key].get<std::string>());
            return p.is_absolute() ? p : base / p;
        };
        c.game_exe = path_of("game_exe");
        c.game_root = path_of("game_root");
        c.ue4ss_dir = path_of("ue4ss_dir");
        c.paks_dir = path_of("paks_dir");
        if (j.contains("session_dir")) c.session_dir = path_of("session_dir");
        c.profile_dir = path_of("profile");
        if (j.contains("game_args"))
            for (auto& a : j["game_args"]) c.game_args.push_back(widen(a.get<std::string>()));
        c.bridge_port = static_cast<std::uint16_t>(j.value("bridge_port", static_cast<int>(kDefaultPort)));
        c.connect_timeout_ms = j.value("connect_timeout_ms", 1500);
        c.page_bytes = j.value("page_bytes", static_cast<std::size_t>(60000));
        c.default_timeout_ms = j.value("default_timeout_ms", kDefaultTimeoutMs);
        c.derive_paths();
        return c;
    }

    void Config::derive_paths()
    {
        if (game_root.empty() && !game_exe.empty())
        {
            // <root>/<Project>/Binaries/Win64/<exe>  or <root>/<launcher exe>
            auto p = game_exe.parent_path();
            if (p.filename() == L"Win64" && p.parent_path().filename() == L"Binaries")
                game_root = p.parent_path().parent_path().parent_path();
            else
                game_root = p;
        }
        if (game_root.empty() || !fs::exists(game_root)) return;
        std::error_code ec;
        for (auto& project : fs::directory_iterator(game_root, ec))
        {
            if (!project.is_directory()) continue;
            if (ue4ss_dir.empty())
            {
                auto u = project.path() / "Binaries" / "Win64" / "ue4ss";
                if (fs::exists(u / "UE4SS.dll", ec)) ue4ss_dir = u;
            }
            if (paks_dir.empty())
            {
                auto pk = project.path() / "Content" / "Paks";
                if (fs::is_directory(pk, ec)) paks_dir = pk;
            }
        }
    }

    json Config::to_json() const
    {
        json args = json::array();
        for (auto& a : game_args) args.push_back(narrow(a));
        return json{{"config_file", narrow(config_file.wstring())},
                    {"game_exe", narrow(game_exe.wstring())},
                    {"game_args", args},
                    {"game_root", narrow(game_root.wstring())},
                    {"ue4ss_dir", narrow(ue4ss_dir.wstring())},
                    {"paks_dir", narrow(paks_dir.wstring())},
                    {"session_dir", narrow(session_dir.wstring())},
                    {"profile", narrow(profile_dir.wstring())},
                    {"bridge_port", bridge_port},
                    {"connect_timeout_ms", connect_timeout_ms},
                    {"page_bytes", page_bytes},
                    {"default_timeout_ms", default_timeout_ms}};
    }
} // namespace ue5mcp::host
