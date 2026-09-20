#include "sc2/render/anim.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace sc2::render {

namespace {

std::vector<std::string> tokenize(const std::string& name) {
    std::vector<std::string> out;
    std::istringstream ss(name);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}
bool has_all(const std::vector<std::string>& hay,
             const std::vector<std::string>& need) {
    for (const std::string& n : need) {
        bool found = false;
        for (const std::string& h : hay)
            if (h == n) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

const formats::M3Track* find_track(const formats::Model& m, uint32_t seq,
                                   uint32_t anim_id) {
    if (seq >= m.stg.size()) return nullptr;
    for (uint32_t stc_i : m.stg[seq].stc_indices) {
        if (stc_i >= m.stc.size()) continue;
        const formats::M3STC& c = m.stc[stc_i];
        for (size_t k = 0; k < c.anim_ids.size() && k < c.anim_refs.size(); ++k) {
            if (c.anim_ids[k] != anim_id) continue;
            uint32_t p = c.anim_refs[k];
            uint32_t slot = p >> 16, elem = p & 0xFFFF;
            if (slot < 13 && elem < c.sd[slot].size()) return &c.sd[slot][elem];
        }
    }
    return nullptr;
}

bool sample(const formats::M3Track& tr, float t, uint16_t interp, int comps,
            float* out) {
    size_t n = tr.keys.size();
    if (n == 0 || tr.values.size() < n * comps) return false;
    if (t <= tr.keys.front() || n == 1) {
        for (int c = 0; c < comps; ++c) out[c] = tr.values[c];
        return true;
    }
    if (t >= tr.keys.back()) {
        for (int c = 0; c < comps; ++c) out[c] = tr.values[(n - 1) * comps + c];
        return true;
    }
    size_t j = 0;
    while (j + 1 < n && tr.keys[j + 1] <= t) ++j;
    if (interp == 0 || j + 1 >= n) {
        for (int c = 0; c < comps; ++c) out[c] = tr.values[j * comps + c];
        return true;
    }
    float k0 = float(tr.keys[j]), k1 = float(tr.keys[j + 1]);
    float f = k1 > k0 ? (t - k0) / (k1 - k0) : 0.0f;
    const float* a = &tr.values[j * comps];
    const float* b = &tr.values[(j + 1) * comps];
    if (comps == 4 && tr.slot == 3) {
        float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        float s = dot < 0 ? -1.0f : 1.0f;
        float len = 0;
        for (int c = 0; c < 4; ++c) { out[c] = a[c] + f * (s * b[c] - a[c]); len += out[c] * out[c]; }
        len = std::sqrt(len);
        if (len > 1e-8f) for (int c = 0; c < 4; ++c) out[c] /= len;
        return true;
    }
    for (int c = 0; c < comps; ++c) out[c] = a[c] + f * (b[c] - a[c]);
    return true;
}

void field(const formats::Model& m, uint32_t seq, const formats::M3AnimRef& a,
           float t, int comps, float* out) {
    for (int c = 0; c < comps; ++c) out[c] = a.init[c];
    if (a.anim_flags == 0) return;
    const formats::M3Track* tr = find_track(m, seq, a.anim_id);
    if (tr) sample(*tr, t, a.interp, comps, out);
}

Mat4 bind_matrix(const formats::Model& m, size_t b) {
    Mat4 r;
    if ((b + 1) * 16 <= m.inverse_bind.size())
        for (int i = 0; i < 16; ++i) r.m[i] = m.inverse_bind[b * 16 + i];
    else
        r = Mat4::identity();
    return r;
}

}

std::vector<Mat4> pose_world(const formats::Model& m, uint32_t seq, float time_ms) {
    size_t n = m.bones.size();
    std::vector<Mat4> world(n);
    if (n == 0) return world;

    float t = time_ms;
    if (seq < m.sequences.size()) t += float(m.sequences[seq].start_ms);

    std::vector<Mat4> local(n);
    for (size_t b = 0; b < n; ++b) {
        const formats::M3Bone& bone = m.bones[b];
        float T[3], R[4] = {0, 0, 0, 1}, S[3] = {1, 1, 1};
        field(m, seq, bone.location, t, 3, T);
        field(m, seq, bone.rotation, t, 4, R);
        field(m, seq, bone.scale, t, 3, S);
        local[b] = translation(T[0], T[1], T[2]) * from_quat(R[0], R[1], R[2], R[3]) *
                   scaling(S[0], S[1], S[2]);
    }
    std::vector<char> done(n, 0);
    for (bool progress = true; progress;) {
        progress = false;
        for (size_t b = 0; b < n; ++b) {
            if (done[b]) continue;
            const int par = m.bones[b].parent;
            if (par < 0 || size_t(par) >= n) {
                world[b] = local[b];
                done[b] = 1;
                progress = true;
            } else if (done[size_t(par)]) {
                world[b] = world[size_t(par)] * local[b];
                done[b] = 1;
                progress = true;
            }
        }
    }
    for (size_t b = 0; b < n; ++b)
        if (!done[b]) world[b] = local[b];
    return world;
}

bool posed_bounds(const formats::Model& m, int seq, float time_ms, float mn[3], float mx[3]) {
    if (m.vertex_count == 0 || m.positions.size() < std::size_t(m.vertex_count) * 3) return false;
    const std::vector<Mat4> pm = seq >= 0 ? pose(m, std::uint32_t(seq), time_ms) : std::vector<Mat4>{};
    const bool skin = !pm.empty() && m.vertex_bone_weights.size() >= std::size_t(m.vertex_count) * 4 &&
                      m.vertex_bone_indices.size() >= std::size_t(m.vertex_count) * 4;
    std::vector<std::uint32_t> vregion(m.vertex_count, 0);
    for (std::size_t ri = 0; ri < m.regions.size(); ++ri)
        for (std::uint32_t x = m.regions[ri].first_vertex;
             x < m.regions[ri].first_vertex + m.regions[ri].num_vertices && x < m.vertex_count; ++x)
            vregion[x] = std::uint32_t(ri);
    for (int c = 0; c < 3; ++c) { mn[c] = 1e30f; mx[c] = -1e30f; }
    for (std::uint32_t v = 0; v < m.vertex_count; ++v) {
        const float p[3] = {m.positions[v * 3], m.positions[v * 3 + 1], m.positions[v * 3 + 2]};
        float o[3] = {p[0], p[1], p[2]};
        if (skin) {
            o[0] = o[1] = o[2] = 0.0f;
            float wsum = 0.0f;
            const std::uint32_t base = m.regions.empty() ? 0 : m.regions[vregion[v]].first_bone_lookup;
            for (int k = 0; k < 4; ++k) {
                const float w = m.vertex_bone_weights[v * 4 + k] / 255.0f;
                if (w <= 0.0f) continue;
                const std::uint32_t look = base + m.vertex_bone_indices[v * 4 + k];
                std::uint32_t bone = look < m.bone_lookup.size() ? m.bone_lookup[look] : 0;
                if (bone >= pm.size()) bone = 0;
                const float* a = pm[bone].m.data();
                for (int c = 0; c < 3; ++c)
                    o[c] += w * (a[c] * p[0] + a[4 + c] * p[1] + a[8 + c] * p[2] + a[12 + c]);
                wsum += w;
            }
            if (wsum > 0.0f)
                for (int c = 0; c < 3; ++c) o[c] /= wsum;
            else
                for (int c = 0; c < 3; ++c) o[c] = p[c];
        }
        for (int c = 0; c < 3; ++c) {
            mn[c] = std::min(mn[c], o[c]);
            mx[c] = std::max(mx[c], o[c]);
        }
    }
    return true;
}

std::vector<Mat4> pose(const formats::Model& m, uint32_t seq, float time_ms) {
    std::vector<Mat4> world = pose_world(m, seq, time_ms);
    size_t n = world.size();
    std::vector<Mat4> skin(n, Mat4::identity());
    for (size_t b = 0; b < n; ++b) skin[b] = world[b] * bind_matrix(m, b);
    return skin;
}

int select_sequence(const formats::Model& m, const std::vector<std::string>& tokens,
                    uint64_t seed) {
    int best_extra = -1;
    std::vector<int> cands;
    for (size_t i = 0; i < m.sequences.size(); ++i) {
        std::vector<std::string> t = tokenize(m.sequences[i].name);
        if (!has_all(t, tokens)) continue;
        int extra = int(t.size()) - int(tokens.size());
        if (best_extra < 0 || extra < best_extra) { best_extra = extra; cands.clear(); }
        if (extra == best_extra) cands.push_back(int(i));
    }
    if (cands.empty()) return -1;
    if (cands.size() == 1) return cands[0];
    uint64_t total = 0;
    for (int i : cands) total += (m.sequences[i].frequency ? m.sequences[i].frequency : 1);
    seed ^= seed >> 33; seed *= 0xff51afd7ed558ccdULL; seed ^= seed >> 33;
    uint64_t pick = total ? seed % total : 0;
    for (int i : cands) {
        uint64_t w = m.sequences[i].frequency ? m.sequences[i].frequency : 1;
        if (pick < w) return i;
        pick -= w;
    }
    return cands[0];
}

void collect_events(const formats::Model& m, uint32_t seq, float t0, float t1,
                    std::vector<formats::M3Event>& out) {
    if (seq >= m.sequences.size()) return;
    const float start = float(m.sequences[seq].start_ms);
    for (const formats::M3Event& e : m.events_for_sequence(seq)) {
        float local = float(e.time_ms);
        if (local >= start) local -= start;
        if (local > t0 && local <= t1) out.push_back(e);
    }
}

int attachment_index(const formats::Model& m, const std::string& name) {
    auto lower = [](std::string s) {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = char(c + 32);
        return s;
    };
    const std::string q = lower(name);
    for (size_t i = 0; i < m.attachments.size(); ++i)
        if (lower(m.attachments[i].name) == q) return int(i);
    for (size_t i = 0; i < m.attachments.size(); ++i)
        if (lower(m.attachments[i].name).find(q) != std::string::npos) return int(i);
    return -1;
}

bool attachment_transform(const formats::Model& m, const std::string& name, uint32_t seq,
                          float time_ms, Mat4& out) {
    int ai = attachment_index(m, name);
    if (ai < 0) return false;
    uint32_t bone = m.attachments[size_t(ai)].bone;
    std::vector<Mat4> world = pose_world(m, seq, time_ms);
    if (bone >= world.size()) return false;
    out = world[bone];
    return true;
}

void animated_layer_field(const formats::Model& model, uint32_t seq, uint16_t interp, uint16_t flags, uint32_t anim_id,
                          float time_ms, int comps, const float* fallback, float* out) {
    for (int c = 0; c < comps; ++c) out[c] = fallback[c];
    if (flags == 0) return;
    const formats::M3Track* tr = find_track(model, seq, anim_id);
    if (!tr) return;
    float v[4] = {0, 0, 0, 0};
    if (!sample(*tr, time_ms, interp, comps, v)) return;
    for (int c = 0; c < comps; ++c) out[c] = v[c];
}

float animated_scalar(const formats::Model& model, uint32_t seq, const formats::M3AnimRef& ref,
                      float time_ms, float fallback) {
    if (ref.anim_flags == 0) return fallback;
    const formats::M3Track* tr = find_track(model, seq, ref.anim_id);
    if (!tr) return fallback;
    float v = 0.0f;
    if (!sample(*tr, time_ms, ref.interp, 1, &v)) return fallback;
    return v;
}

}
