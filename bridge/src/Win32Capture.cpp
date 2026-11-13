#include "Win32Capture.hpp"

#include <windows.h>
#include <wincodec.h>

#include <map>
#include <thread>
#include <vector>

using ue5mcp::RpcError;
using json = nlohmann::json;

namespace ue5mcp::bridge::win32
{
    namespace
    {
        struct EnumState
        {
            DWORD pid;
            HWND best{nullptr};
            long best_area{0};
        };

        BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp)
        {
            auto* st = reinterpret_cast<EnumState*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != st->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
            wchar_t title[256]{};
            GetWindowTextW(hwnd, title, 255);
            if (wcsstr(title, L"UE4SS")) return TRUE; // the UE4SS debugging GUI
            RECT rc{};
            GetClientRect(hwnd, &rc);
            long area = (rc.right - rc.left) * (rc.bottom - rc.top);
            if (area > st->best_area)
            {
                st->best_area = area;
                st->best = hwnd;
            }
            return TRUE;
        }

        std::wstring widen(const std::string& s)
        {
            if (s.empty()) return {};
            int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }

        WORD vk_for(const std::string& name)
        {
            static const std::map<std::string, WORD> named = {
                    {"space", VK_SPACE}, {"enter", VK_RETURN}, {"return", VK_RETURN}, {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE}, {"tab", VK_TAB},
                    {"backspace", VK_BACK}, {"delete", VK_DELETE}, {"insert", VK_INSERT}, {"home", VK_HOME}, {"end", VK_END}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT},
                    {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
                    {"leftshift", VK_LSHIFT}, {"shift", VK_LSHIFT}, {"rightshift", VK_RSHIFT}, {"leftctrl", VK_LCONTROL}, {"ctrl", VK_LCONTROL}, {"control", VK_LCONTROL},
                    {"rightctrl", VK_RCONTROL}, {"leftalt", VK_LMENU}, {"alt", VK_LMENU}, {"rightalt", VK_RMENU}, {"capslock", VK_CAPITAL},
                    {"tilde", VK_OEM_3}, {"grave", VK_OEM_3}, {"minus", VK_OEM_MINUS}, {"equals", VK_OEM_PLUS}, {"comma", VK_OEM_COMMA}, {"period", VK_OEM_PERIOD},
                    {"slash", VK_OEM_2}, {"backslash", VK_OEM_5}, {"semicolon", VK_OEM_1}, {"quote", VK_OEM_7}, {"leftbracket", VK_OEM_4}, {"rightbracket", VK_OEM_6},
                    {"numpad0", VK_NUMPAD0}, {"numpad1", VK_NUMPAD1}, {"numpad2", VK_NUMPAD2}, {"numpad3", VK_NUMPAD3}, {"numpad4", VK_NUMPAD4},
                    {"numpad5", VK_NUMPAD5}, {"numpad6", VK_NUMPAD6}, {"numpad7", VK_NUMPAD7}, {"numpad8", VK_NUMPAD8}, {"numpad9", VK_NUMPAD9}};
            std::string k;
            for (char c : name) k.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
            if (k.size() == 1)
            {
                char c = static_cast<char>(toupper(static_cast<unsigned char>(k[0])));
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<WORD>(c);
            }
            if (k.size() >= 2 && k[0] == 'f')
            {
                int n = atoi(k.c_str() + 1);
                if (n >= 1 && n <= 24) return static_cast<WORD>(VK_F1 + n - 1);
            }
            auto it = named.find(k);
            return it == named.end() ? 0 : it->second;
        }

        void send_key(WORD vk, bool down)
        {
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = vk;
            in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
            in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
            switch (vk)
            {
            case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
            case VK_INSERT: case VK_DELETE: case VK_RCONTROL: case VK_RMENU:
                in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                break;
            default:
                break;
            }
            SendInput(1, &in, sizeof in);
        }
    } // namespace

    void* game_window()
    {
        EnumState st{GetCurrentProcessId()};
        EnumWindows(&enum_proc, reinterpret_cast<LPARAM>(&st));
        return st.best;
    }

    bool focus_game_window()
    {
        HWND hwnd = static_cast<HWND>(game_window());
        if (!hwnd) return false;
        if (GetForegroundWindow() == hwnd) return true;
        // Attach to the foreground thread so SetForegroundWindow is allowed, then give the window a moment.
        DWORD fg_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        DWORD our_thread = GetCurrentThreadId();
        AttachThreadInput(our_thread, fg_thread, TRUE);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        AttachThreadInput(our_thread, fg_thread, FALSE);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        return GetForegroundWindow() == hwnd;
    }

    bool key_action(const std::string& key, const std::string& action, int hold_ms)
    {
        WORD vk = vk_for(key);
        if (!vk) return false;
        if (action == "down") send_key(vk, true);
        else if (action == "up") send_key(vk, false);
        else
        {
            send_key(vk, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms > 0 ? hold_ms : 60));
            send_key(vk, false);
        }
        return true;
    }

    bool mouse_action(const std::string& button, const std::string& action, std::optional<int> x, std::optional<int> y, int dx, int dy, int hold_ms)
    {
        HWND hwnd = static_cast<HWND>(game_window());
        if (x && y && hwnd)
        {
            POINT pt{*x, *y};
            ClientToScreen(hwnd, &pt);
            SetCursorPos(pt.x, pt.y);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        DWORD down = MOUSEEVENTF_LEFTDOWN, up = MOUSEEVENTF_LEFTUP;
        if (button == "right") { down = MOUSEEVENTF_RIGHTDOWN; up = MOUSEEVENTF_RIGHTUP; }
        else if (button == "middle") { down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; }
        auto send = [](DWORD flags, LONG mx = 0, LONG my = 0) {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = flags;
            in.mi.dx = mx;
            in.mi.dy = my;
            SendInput(1, &in, sizeof in);
        };
        if (action == "move" || dx || dy) send(MOUSEEVENTF_MOVE, dx, dy);
        if (action == "down") send(down);
        else if (action == "up") send(up);
        else if (action == "click")
        {
            send(down);
            std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms > 0 ? hold_ms : 60));
            send(up);
        }
        return true;
    }

    bool type_text(const std::string& utf8)
    {
        std::wstring w = widen(utf8);
        std::vector<INPUT> inputs;
        for (wchar_t c : w)
        {
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wScan = c;
            in.ki.dwFlags = KEYEVENTF_UNICODE;
            inputs.push_back(in);
            in.ki.dwFlags |= KEYEVENTF_KEYUP;
            inputs.push_back(in);
        }
        if (!inputs.empty()) SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
        return true;
    }

    json capture_window_png(void* hwnd_v, const std::filesystem::path& out)
    {
        HWND hwnd = static_cast<HWND>(hwnd_v);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) throw RpcError(ue5mcp::Unavailable, "game window has no client area (minimised?)");

        HDC wnd_dc = GetDC(hwnd);
        HDC mem_dc = CreateCompatibleDC(wnd_dc);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(wnd_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem_dc, bmp);
        // PW_RENDERFULLCONTENT (2) captures DirectX-presented content on Win8.1+; fall back to BitBlt.
        BOOL ok = PrintWindow(hwnd, mem_dc, PW_CLIENTONLY | 0x2);
        if (!ok) BitBlt(mem_dc, 0, 0, w, h, wnd_dc, 0, 0, SRCCOPY);
        GdiFlush();

        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool uninit = SUCCEEDED(hr);
        IWICImagingFactory* factory = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IWICStream* stream = nullptr;
        auto fail = [&](const char* what) {
            if (frame) frame->Release();
            if (encoder) encoder->Release();
            if (stream) stream->Release();
            if (factory) factory->Release();
            SelectObject(mem_dc, old);
            DeleteObject(bmp);
            DeleteDC(mem_dc);
            ReleaseDC(hwnd, wnd_dc);
            if (uninit) CoUninitialize();
            throw RpcError(ue5mcp::InternalError, std::string("PNG encode failed at ") + what);
        };
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) fail("factory");
        if (FAILED(factory->CreateStream(&stream))) fail("stream");
        if (FAILED(stream->InitializeFromFilename(out.c_str(), GENERIC_WRITE))) fail("open file");
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) fail("encoder");
        if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) fail("encoder init");
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) fail("frame");
        if (FAILED(frame->Initialize(nullptr))) fail("frame init");
        if (FAILED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h)))) fail("size");
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&fmt))) fail("format");
        // Force alpha opaque: PrintWindow leaves it undefined.
        auto* px = static_cast<std::uint8_t*>(bits);
        for (int i = 0; i < w * h; ++i) px[i * 4 + 3] = 255;
        if (FAILED(frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(w * 4), static_cast<UINT>(w * h * 4), px))) fail("write");
        if (FAILED(frame->Commit())) fail("frame commit");
        if (FAILED(encoder->Commit())) fail("encoder commit");

        frame->Release();
        encoder->Release();
        stream->Release();
        factory->Release();
        SelectObject(mem_dc, old);
        DeleteObject(bmp);
        DeleteDC(mem_dc);
        ReleaseDC(hwnd, wnd_dc);
        if (uninit) CoUninitialize();

        std::string path_utf8;
        {
            auto ws = out.wstring();
            int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
            path_utf8.resize(static_cast<size_t>(n));
            WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), path_utf8.data(), n, nullptr, nullptr);
        }
        return json{{"path", path_utf8}, {"width", w}, {"height", h}, {"method", ok ? "PrintWindow" : "BitBlt"}};
    }
} // namespace ue5mcp::bridge::win32
