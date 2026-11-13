#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <ue5mcp/protocol.hpp>

namespace ue5mcp::host
{
    struct Config
    {
        std::filesystem::path config_file;      // where it was loaded from (may be empty)
        std::filesystem::path game_exe;         // executable to launch (launcher or shipping exe)
        std::vector<std::wstring> game_args;    // extra command line arguments
        std::filesystem::path game_root;        // game install root (holds <Project>/Content/Paks, <Project>/Binaries/Win64/ue4ss)
        std::filesystem::path ue4ss_dir;        // derived if empty: first */Binaries/Win64/ue4ss under game_root
        std::filesystem::path paks_dir;         // derived if empty: first */Content/Paks under game_root
        std::filesystem::path session_dir{"sessions"};
        std::filesystem::path profile_dir;      // profiles/<game>, holds recipes.json
        std::uint16_t bridge_port{kDefaultPort};
        int connect_timeout_ms{1500};
        std::size_t page_bytes{60000};          // results larger than this are paginated
        int default_timeout_ms{kDefaultTimeoutMs};

        static Config load(const std::filesystem::path& file);
        static Config defaults();
        void derive_paths();
        json to_json() const;
    };
} // namespace ue5mcp::host
