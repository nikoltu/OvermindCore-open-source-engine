#include "omc/app/platform.hpp"
#include "omc/core/client_identity.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace omc::app::platform {

namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

fs::path config_dir() {
    const char* appdata = std::getenv("APPDATA");
    fs::path base = appdata && *appdata ? fs::path(appdata) : fs::temp_directory_path();
    const fs::path dir = base / identity::kConfigFolder;
    static bool migrated = false;
    if (!migrated) {
        migrated = true;
        std::error_code ec;
        const fs::path legacy = base / identity::kLegacyConfigFolder;
        if (!fs::exists(dir, ec) && fs::is_directory(legacy, ec)) fs::copy(legacy, dir, fs::copy_options::recursive, ec);
    }
    return dir;
}

std::map<std::string, std::string> read_config() {
    std::map<std::string, std::string> kv;
    std::ifstream in(config_path());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        kv[k] = v;
    }
    return kv;
}

}

std::string config_path() {
    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    return (config_dir() / "config.txt").string();
}

std::string get_config(const std::string& key) {
    auto kv = read_config();
    auto it = kv.find(key);
    return it == kv.end() ? std::string() : it->second;
}

bool set_config(const std::string& key, const std::string& value) {
    auto kv = read_config();
    kv[key] = value;
    std::ofstream out(config_path(), std::ios::trunc);
    if (!out) return false;
    out << "# " << identity::kName << " config\n";
    for (const auto& [k, v] : kv) out << k << "=" << v << "\n";
    return bool(out);
}

std::string pick_folder(const std::string& title) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::wstring wtitle = widen(title);
    wchar_t display[MAX_PATH] = {0};
    BROWSEINFOW bi = {};
    bi.hwndOwner = nullptr;
    bi.pszDisplayName = display;
    bi.lpszTitle = wtitle.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    std::string result;
    if (LPITEMIDLIST pidl = ::SHBrowseForFolderW(&bi)) {
        wchar_t path[MAX_PATH] = {0};
        if (::SHGetPathFromIDListW(pidl, path)) result = narrow(path);
        ::CoTaskMemFree(pidl);
    }
    ::CoUninitialize();
    return result;
}

void alert(const std::string& title, const std::string& message) {
    ::MessageBoxW(nullptr, widen(message).c_str(), widen(title).c_str(), MB_OK | MB_ICONWARNING);
}

void process_memory_mb(double& working_set, double& private_bytes) {
    working_set = private_bytes = 0.0;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (::K32GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        working_set = double(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        private_bytes = double(pmc.PrivateUsage) / (1024.0 * 1024.0);
    }
}

std::vector<MapEntry> list_maps(const std::string& root, int max_depth) {
    std::vector<MapEntry> out;
    std::error_code ec;
    if (root.empty() || !fs::exists(root, ec)) return out;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it.depth() > max_depth) { it.disable_recursion_pending(); continue; }
        const fs::path& p = it->path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".sc2map") {
            out.push_back({p.string(), p.stem().string()});
            it.disable_recursion_pending();
        }
    }
    std::sort(out.begin(), out.end(),
              [](const MapEntry& a, const MapEntry& b) { return a.name < b.name; });
    return out;
}

struct LoadingScreen::Impl {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring status;
    float frac = 0.0f;

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_CREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
            ::SetWindowLongPtr(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        auto* self = reinterpret_cast<Impl*>(::GetWindowLongPtr(h, GWLP_USERDATA));
        if (msg == WM_PAINT && self) {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(h, &ps);
            self->paint(dc);
            ::EndPaint(h, &ps);
            return 0;
        }
        if (msg == WM_ERASEBKGND) return 1;
        return ::DefWindowProcW(h, msg, wp, lp);
    }

    void paint(HDC dc) {
        RECT rc;
        ::GetClientRect(hwnd, &rc);
        int w = rc.right, hgt = rc.bottom;
        HDC mem = ::CreateCompatibleDC(dc);
        HBITMAP bmp = ::CreateCompatibleBitmap(dc, w, hgt);
        HGDIOBJ old = ::SelectObject(mem, bmp);

        HBRUSH bg = ::CreateSolidBrush(RGB(10, 14, 22));
        ::FillRect(mem, &rc, bg);
        ::DeleteObject(bg);

        ::SetBkMode(mem, TRANSPARENT);
        HFONT ftitle = ::CreateFontW(34, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ of = ::SelectObject(mem, ftitle);
        ::SetTextColor(mem, RGB(120, 200, 255));
        RECT tr = {0, hgt / 2 - 90, w, hgt / 2 - 50};
        ::DrawTextW(mem, title.c_str(), -1, &tr, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        ::SelectObject(mem, of);
        ::DeleteObject(ftitle);

        int bx0 = w / 2 - 220, bx1 = w / 2 + 220, by0 = hgt / 2, by1 = hgt / 2 + 24;
        HBRUSH frame = ::CreateSolidBrush(RGB(40, 56, 84));
        RECT fr = {bx0 - 2, by0 - 2, bx1 + 2, by1 + 2};
        ::FillRect(mem, &fr, frame);
        ::DeleteObject(frame);
        HBRUSH track = ::CreateSolidBrush(RGB(18, 24, 36));
        RECT trk = {bx0, by0, bx1, by1};
        ::FillRect(mem, &trk, track);
        ::DeleteObject(track);
        int fillw = int((bx1 - bx0) * std::max(0.0f, std::min(1.0f, frac)));
        HBRUSH fillb = ::CreateSolidBrush(RGB(80, 150, 220));
        RECT fl = {bx0, by0, bx0 + fillw, by1};
        ::FillRect(mem, &fl, fillb);
        ::DeleteObject(fillb);

        HFONT fstat = ::CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        of = ::SelectObject(mem, fstat);
        ::SetTextColor(mem, RGB(150, 165, 185));
        RECT sr = {0, by1 + 14, w, by1 + 44};
        ::DrawTextW(mem, status.c_str(), -1, &sr, DT_CENTER | DT_SINGLELINE);
        ::SelectObject(mem, of);
        ::DeleteObject(fstat);

        ::BitBlt(dc, 0, 0, w, hgt, mem, 0, 0, SRCCOPY);
        ::SelectObject(mem, old);
        ::DeleteObject(bmp);
        ::DeleteDC(mem);
    }
};

LoadingScreen::LoadingScreen(const std::string& title) : impl_(std::make_unique<Impl>()) {
    impl_->title = widen(title);
    impl_->status = L"Starting...";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = &Impl::proc;
    wc.hInstance = ::GetModuleHandle(nullptr);
    wc.lpszClassName = identity::kLoadingWindowClass;
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    ::RegisterClassW(&wc);

    int sw = ::GetSystemMetrics(SM_CXSCREEN), sh = ::GetSystemMetrics(SM_CYSCREEN);
    const int w = 640, h = 360;
    impl_->hwnd = ::CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, impl_->title.c_str(),
                                    WS_POPUP | WS_BORDER, (sw - w) / 2, (sh - h) / 2, w, h, nullptr,
                                    nullptr, wc.hInstance, impl_.get());
    if (impl_->hwnd) {
        ::ShowWindow(impl_->hwnd, SW_SHOW);
        ::UpdateWindow(impl_->hwnd);
    }
}

LoadingScreen::~LoadingScreen() { close(); }

void LoadingScreen::progress(float frac, const std::string& label) {
    if (!impl_ || !impl_->hwnd) return;
    impl_->frac = frac;
    impl_->status = widen(label);
    ::InvalidateRect(impl_->hwnd, nullptr, FALSE);
    MSG msg;
    while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
    }
    ::UpdateWindow(impl_->hwnd);
}

void LoadingScreen::close() {
    if (impl_ && impl_->hwnd) {
        ::DestroyWindow(impl_->hwnd);
        impl_->hwnd = nullptr;
    }
}

double double_click_ms() {
    const UINT ms = ::GetDoubleClickTime();
    return ms > 0 ? double(ms) : 500.0;
}

std::string clipboard_text() {
    std::string out;
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT) || !::OpenClipboard(nullptr)) return out;
    if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = static_cast<const wchar_t*>(::GlobalLock(h))) {
            out = narrow(std::wstring(w));
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    return out;
}

bool open_folder(const std::string& path) {
    std::error_code ec;
    if (path.empty() || !fs::is_directory(path, ec)) return false;
    const HINSTANCE r = ::ShellExecuteW(nullptr, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

bool attach_parent_console() {
    const HANDLE existing = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (existing && existing != INVALID_HANDLE_VALUE) return true;
    if (!::AttachConsole(ATTACH_PARENT_PROCESS)) return false;
    const bool out = std::freopen("CONOUT$", "w", stdout) != nullptr;
    std::freopen("CONOUT$", "w", stderr);
    return out;
}

namespace {

bool winsock_ready() {
    static const bool ok = [] {
        WSADATA wsa{};
        return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ok;
}

class WinConnection final : public Connection {
public:
    explicit WinConnection(SOCKET s) : s_(s) {}
    ~WinConnection() override { close(); }

    int recv(void* buf, int len) override {
        if (s_ == INVALID_SOCKET) return -1;
        return ::recv(s_, static_cast<char*>(buf), len, 0);
    }
    bool send_all(const void* buf, int len) override {
        if (s_ == INVALID_SOCKET) return false;
        const char* p = static_cast<const char*>(buf);
        while (len > 0) {
            const int n = ::send(s_, p, len, 0);
            if (n <= 0) return false;
            p += n;
            len -= n;
        }
        return true;
    }
    void close() override {
        if (s_ != INVALID_SOCKET) {
            ::shutdown(s_, SD_BOTH);
            ::closesocket(s_);
            s_ = INVALID_SOCKET;
        }
    }

private:
    SOCKET s_ = INVALID_SOCKET;
};

class WinListener final : public Listener {
public:
    explicit WinListener(SOCKET s) : s_(s) {}
    ~WinListener() override { close(); }

    std::unique_ptr<Connection> accept() override {
        if (s_ == INVALID_SOCKET) return nullptr;
        const SOCKET c = ::accept(s_, nullptr, nullptr);
        if (c == INVALID_SOCKET) return nullptr;
        BOOL one = TRUE;
        ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        return std::make_unique<WinConnection>(c);
    }
    void close() override {
        if (s_ != INVALID_SOCKET) {
            ::closesocket(s_);
            s_ = INVALID_SOCKET;
        }
    }

private:
    SOCKET s_ = INVALID_SOCKET;
};

}

std::unique_ptr<Listener> listen_tcp(int port, std::string& error) {
    if (!winsock_ready()) {
        error = "WSAStartup failed: no Winsock 2.2 on this system";
        return nullptr;
    }
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error = "socket() failed with WSA error " + std::to_string(::WSAGetLastError());
        return nullptr;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR) {
        const int e = ::WSAGetLastError();
        ::closesocket(s);
        error = "bind to 127.0.0.1:" + std::to_string(port) + " failed with WSA error " +
                std::to_string(e) +
                (e == WSAEADDRINUSE ? " (that port is already in use - another client is listening"
                                      " on it; pass --api-port N)"
                                    : "");
        return nullptr;
    }
    if (::listen(s, 1) == SOCKET_ERROR) {
        const int e = ::WSAGetLastError();
        ::closesocket(s);
        error = "listen() failed with WSA error " + std::to_string(e);
        return nullptr;
    }
    return std::make_unique<WinListener>(s);
}

}
