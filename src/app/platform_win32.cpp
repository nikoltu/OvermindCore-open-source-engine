#include "sc2/app/platform.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace sc2::app::platform {

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
    return base / "CrafterSC";
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
    out << "# CrafterSC config\n";
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
        return ::DefWindowProc(h, msg, wp, lp);
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
    wc.lpszClassName = L"CrafterSCLoading";
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

}
