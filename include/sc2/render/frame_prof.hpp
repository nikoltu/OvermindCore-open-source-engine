#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc2::render {

struct FrameProfState {
    bool on = false;
    int every = 60;
    int frame = 0;
    static constexpr std::size_t kMaxSamples = 4096;
    std::unordered_map<std::string, std::vector<double>> ms;
    std::vector<std::string> order;
    std::unordered_map<std::string, std::vector<double>> counts;
    std::vector<std::string> count_order;
};

inline FrameProfState& frame_prof() {
    static FrameProfState s = [] {
        FrameProfState st;
        const char* e = std::getenv("SC2_FRAME_PROF");
        st.on = e && e[0];
        if (const char* n = std::getenv("SC2_FRAME_PROF_EVERY")) {
            const int v = std::atoi(n);
            if (v > 0) st.every = v;
        }
        return st;
    }();
    return s;
}

inline void frame_prof_count(const char* name, double v) {
    FrameProfState& s = frame_prof();
    if (!s.on) return;
    auto it = s.counts.find(name);
    if (it == s.counts.end()) {
        it = s.counts.emplace(name, std::vector<double>{}).first;
        s.count_order.emplace_back(name);
    }
    if (it->second.size() < FrameProfState::kMaxSamples) it->second.push_back(v);
    else it->second[std::size_t(s.frame) % FrameProfState::kMaxSamples] = v;
}

inline void frame_prof_add(const char* name, double ms) {
    FrameProfState& s = frame_prof();
    auto it = s.ms.find(name);
    if (it == s.ms.end()) {
        it = s.ms.emplace(name, std::vector<double>{}).first;
        s.order.emplace_back(name);
    }
    if (it->second.size() < FrameProfState::kMaxSamples) it->second.push_back(ms);
    else it->second[std::size_t(s.frame) % FrameProfState::kMaxSamples] = ms;
}

inline void frame_prof_tick() {
    FrameProfState& s = frame_prof();
    if (!s.on) return;
    ++s.frame;
    if (s.frame % s.every) return;
    auto med = [](std::vector<double> v) {
        if (v.empty()) return 0.0;
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    double total = 0.0;
    if (auto it = s.ms.find("loop_iter"); it != s.ms.end()) total = med(it->second);
    std::printf("-- frame_prof @%d (median of up to %zu samples) --\n", s.frame,
                FrameProfState::kMaxSamples);
    for (const std::string& k : s.order) {
        const std::vector<double>& v = s.ms[k];
        const double m = med(v);
        const double mx = v.empty() ? 0.0 : *std::max_element(v.begin(), v.end());
        if (total > 0.0)
            std::printf("   %-26s median %7.3f ms   max %8.3f ms   %5.1f%% of frame\n", k.c_str(), m,
                        mx, 100.0 * m / total);
        else
            std::printf("   %-26s median %7.3f ms   max %8.3f ms\n", k.c_str(), m, mx);
    }
    for (const std::string& k : s.count_order) {
        const std::vector<double>& v = s.counts[k];
        std::printf("   %-26s median %10.1f       (count)\n", k.c_str(), med(v));
    }
    std::fflush(stdout);
}

class FrameProfScope {
public:
    explicit FrameProfScope(const char* name)
        : name_(name), on_(frame_prof().on),
          t0_(on_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~FrameProfScope() {
        if (!on_) return;
        const auto dt = std::chrono::steady_clock::now() - t0_;
        frame_prof_add(name_,
                       std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(dt)
                           .count());
    }
    FrameProfScope(const FrameProfScope&) = delete;
    FrameProfScope& operator=(const FrameProfScope&) = delete;

private:
    const char* name_;
    bool on_;
    std::chrono::steady_clock::time_point t0_;
};

}

#define SC2_PROF_CAT2(a, b) a##b
#define SC2_PROF_CAT(a, b) SC2_PROF_CAT2(a, b)
#define SC2_PROF_SCOPE(name) \
    ::sc2::render::FrameProfScope SC2_PROF_CAT(sc2_prof_, __LINE__)(name)
