#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

#include "sc2/app/ui_canvas.hpp"

namespace sc2::app {

inline constexpr char kUiScaleKey[] = "display.ui_scale";
inline constexpr float kUiScaleMin = 0.75f;
inline constexpr float kUiScaleMax = 1.5f;

inline float parse_ui_scale(const std::string& raw) {
    if (raw.empty()) return kUiScaleDefault;
    char* end = nullptr;
    const float v = std::strtof(raw.c_str(), &end);
    if (end == raw.c_str() || !(v == v)) return kUiScaleDefault;
    if (v < kUiScaleMin) return kUiScaleMin;
    if (v > kUiScaleMax) return kUiScaleMax;
    return v;
}

inline std::string format_ui_scale(float ui_scale) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(ui_scale));
    return std::string(buf);
}

inline Canvas canvas_from_settings(std::uint32_t phys_w, std::uint32_t phys_h, float base_ref_height,
                                   const std::string& ui_scale_raw) {
    return make_canvas_scaled(phys_w, phys_h, base_ref_height, parse_ui_scale(ui_scale_raw));
}

struct DisplayMode {
    enum class Kind { Windowed, BorderlessFullscreen, ExclusiveFullscreen };
    Kind kind = Kind::BorderlessFullscreen;
    std::uint32_t width = 0, height = 0;
    std::uint32_t refresh_mHz = 0;
    int monitor = 0;
    bool vsync = true;
};

inline constexpr char kDisplayModeKey[] = "display.mode";
inline constexpr char kDisplayWidthKey[] = "display.width";
inline constexpr char kDisplayHeightKey[] = "display.height";
inline constexpr char kDisplayRefreshKey[] = "display.refresh_mHz";
inline constexpr char kDisplayMonitorKey[] = "display.monitor";
inline constexpr char kDisplayVsyncKey[] = "display.vsync";

namespace detail {
inline std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}

inline DisplayMode::Kind parse_display_kind(const std::string& raw) {
    const std::string s = detail::lower_ascii(raw);
    if (s == "windowed") return DisplayMode::Kind::Windowed;
    if (s == "exclusive" || s == "exclusivefullscreen") return DisplayMode::Kind::ExclusiveFullscreen;
    return DisplayMode::Kind::BorderlessFullscreen;
}

inline bool parse_bool(const std::string& raw, bool dflt) {
    const std::string s = detail::lower_ascii(raw);
    if (s == "1" || s == "true" || s == "on" || s == "yes") return true;
    if (s == "0" || s == "false" || s == "off" || s == "no") return false;
    return dflt;
}

inline std::uint32_t parse_uint(const std::string& raw, std::uint32_t dflt) {
    std::size_t i = 0;
    while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
    if (i >= raw.size() || raw[i] == '-') return dflt;
    char* end = nullptr;
    const unsigned long v = std::strtoul(raw.c_str(), &end, 10);
    if (end == raw.c_str()) return dflt;
    return v > 0xFFFFFFFFul ? 0xFFFFFFFFu : static_cast<std::uint32_t>(v);
}

inline int parse_int(const std::string& raw, int dflt) {
    if (raw.empty()) return dflt;
    char* end = nullptr;
    const long v = std::strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str()) return dflt;
    return static_cast<int>(v);
}

inline DisplayMode display_mode_from_config(const std::function<std::string(const std::string&)>& get) {
    DisplayMode m;
    m.kind = parse_display_kind(get(kDisplayModeKey));
    m.width = parse_uint(get(kDisplayWidthKey), 0);
    m.height = parse_uint(get(kDisplayHeightKey), 0);
    m.refresh_mHz = parse_uint(get(kDisplayRefreshKey), 0);
    m.monitor = parse_int(get(kDisplayMonitorKey), 0);
    m.vsync = parse_bool(get(kDisplayVsyncKey), true);
    return m;
}

}
