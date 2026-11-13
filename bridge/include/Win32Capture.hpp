// Win32 helpers that need no engine access: game window lookup, screenshot to PNG, input injection.
#pragma once

#include <filesystem>
#include <string>

#include <ue5mcp/protocol.hpp>

namespace ue5mcp::bridge::win32
{
    // Main top-level window of this process (largest visible one), or nullptr.
    void* game_window();

    // Capture the window's client area to a PNG. Returns {path,width,height}. Throws RpcError on failure.
    nlohmann::json capture_window_png(void* hwnd, const std::filesystem::path& out);

    // Bring the game window to the foreground (needed before SendInput).
    bool focus_game_window();

    // Key by name: "F", "Space", "LeftShift", "F5", "Escape", "Mouse" buttons handled separately.
    bool key_action(const std::string& key, const std::string& action, int hold_ms);
    bool mouse_action(const std::string& button, const std::string& action, std::optional<int> x, std::optional<int> y, int dx, int dy, int hold_ms);
    bool type_text(const std::string& utf8);
} // namespace ue5mcp::bridge::win32
