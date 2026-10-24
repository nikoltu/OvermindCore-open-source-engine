#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
    bool print = true;
    std::string trace_path;
    std::unordered_map<std::string, std::size_t> col;
    std::vector<std::string> cols;
    std::vector<double> cur;
    std::string cur_mark;
    std::vector<std::vector<double>> rows;
    std::vector<std::string> row_marks;
    std::chrono::steady_clock::time_point last_tick{};
    bool tick_seen = false;
    std::size_t column(const char* name) {
        auto it = col.find(name);
        if (it != col.end()) return it->second;
        const std::size_t c = cols.size();
        col.emplace(name, c);
        cols.emplace_back(name);
        return c;
    }
    bool rewrite_old = false;
    FILE* body = nullptr;
    std::size_t written = 0;
    void append_rows() {
        if (trace_path.empty() || rows.empty()) return;
        if (!body) body = std::fopen((trace_path + ".rows").c_str(), written ? "ab" : "wb");
        if (!body) return;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            std::fprintf(body, "%zu,%.4f,%s", written + r + 1, rows[r].empty() ? 0.0 : rows[r][0], row_marks[r].c_str());
            for (std::size_t c = 1; c < rows[r].size(); ++c) std::fprintf(body, ",%.4f", rows[r][c]);
            std::fputc('\n', body);
        }
        std::fflush(body);
        written += rows.size();
        rows.clear();
        row_marks.clear();
    }
    void write_trace() {
        if (rewrite_old) { write_trace_old(); return; }
        if (trace_path.empty()) return;
        append_rows();
        if (body) { std::fclose(body); body = nullptr; }
        if (!written) return;
        FILE* f = std::fopen(trace_path.c_str(), "wb");
        if (!f) return;
        std::fprintf(f, "frame,wall_ms,mark");
        for (const std::string& c : cols) std::fprintf(f, ",%s", c.c_str());
        std::fputc('\n', f);
        if (FILE* in = std::fopen((trace_path + ".rows").c_str(), "rb")) {
            char buf[1 << 16];
            for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, in)) > 0;) std::fwrite(buf, 1, n, f);
            std::fclose(in);
        }
        std::fclose(f);
    }
    void write_trace_old() const {
        if (trace_path.empty() || rows.empty()) return;
        FILE* f = std::fopen(trace_path.c_str(), "wb");
        if (!f) return;
        std::fprintf(f, "frame,wall_ms");
        for (const std::string& c : cols) std::fprintf(f, ",%s", c.c_str());
        std::fprintf(f, ",mark\n");
        for (std::size_t r = 0; r < rows.size(); ++r) {
            std::fprintf(f, "%zu,%.4f", r + 1, rows[r].empty() ? 0.0 : rows[r][0]);
            for (std::size_t c = 0; c < cols.size(); ++c)
                std::fprintf(f, ",%.4f", c + 1 < rows[r].size() ? rows[r][c + 1] : 0.0);
            std::fprintf(f, ",%s\n", row_marks[r].c_str());
        }
        std::fclose(f);
    }
    FrameProfState() = default;
    FrameProfState(const FrameProfState&) = default;
    FrameProfState& operator=(const FrameProfState&) = default;
    ~FrameProfState() {
        write_trace();
        if (!rewrite_old && !trace_path.empty() && written) std::remove((trace_path + ".rows").c_str());
    }
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
        if (const char* t = std::getenv("SC2_PERF_TRACE"); t && t[0]) {
            st.print = st.on;
            st.on = true;
            st.trace_path = t;
            st.rewrite_old = std::getenv("SC2_PERF_TRACE_REWRITE") != nullptr;
        }
        return st;
    }();
    return s;
}

inline std::function<void(const char*)>& mem_mark_fn() {
    static std::function<void(const char*)> f;
    return f;
}
inline void mem_mark(const char* stage) {
    if (mem_mark_fn()) mem_mark_fn()(stage);
}
inline bool& mem_lap_marks() {
    static bool on = false;
    return on;
}

inline void frame_prof_mark(const std::string& m) {
    FrameProfState& s = frame_prof();
    if (!s.on || s.trace_path.empty()) return;
    if (!s.cur_mark.empty()) s.cur_mark += ' ';
    for (char ch : m) s.cur_mark += (ch == ',' || ch == '\n' || ch == '\r') ? ';' : ch;
}

inline void frame_prof_flush() { frame_prof().write_trace(); }

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
    if (!s.trace_path.empty()) {
        const std::size_t c = s.column((std::string("n:") + name).c_str());
        if (s.cur.size() <= c) s.cur.resize(c + 1, 0.0);
        s.cur[c] += v;
    }
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
    if (!s.trace_path.empty()) {
        const std::size_t c = s.column(name);
        if (s.cur.size() <= c) s.cur.resize(c + 1, 0.0);
        s.cur[c] += ms;
    }
}

inline void frame_prof_tick() {
    FrameProfState& s = frame_prof();
    if (!s.on) return;
    ++s.frame;
    if (!s.trace_path.empty()) {
        const auto now = std::chrono::steady_clock::now();
        std::vector<double> row(s.cols.size() + 1, 0.0);
        row[0] = s.tick_seen ? std::chrono::duration<double, std::milli>(now - s.last_tick).count() : 0.0;
        for (std::size_t c = 0; c < s.cur.size() && c < s.cols.size(); ++c) row[c + 1] = s.cur[c];
        s.rows.push_back(std::move(row));
        s.row_marks.push_back(s.cur_mark);
        s.cur.assign(s.cols.size(), 0.0);
        s.cur_mark.clear();
        s.last_tick = now;
        s.tick_seen = true;
        if (s.rewrite_old) {
            if (s.rows.size() % 600 == 0) s.write_trace_old();
        } else if (s.rows.size() >= 60) {
            s.append_rows();
        }
    }
    if (!s.print || s.frame % s.every) return;
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

class FrameProfLap {
public:
    FrameProfLap() : on_(frame_prof().on), t_(on_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    void lap(const char* name) {
        if (mem_lap_marks()) mem_mark(name);
        if (!on_) return;
        const auto now = std::chrono::steady_clock::now();
        frame_prof_add(name, std::chrono::duration<double, std::milli>(now - t_).count());
        t_ = now;
    }

private:
    bool on_;
    std::chrono::steady_clock::time_point t_;
};

}

#define SC2_PROF_CAT2(a, b) a##b
#define SC2_PROF_CAT(a, b) SC2_PROF_CAT2(a, b)
#define SC2_PROF_SCOPE(name) \
    ::sc2::render::FrameProfScope SC2_PROF_CAT(sc2_prof_, __LINE__)(name)
