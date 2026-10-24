#include "sc2/render/anim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

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
           float t, int comps, float* out, const ClipLayers* global = nullptr) {
    for (int c = 0; c < comps; ++c) out[c] = a.init[c];
    if (a.anim_flags == 0) return;
    const formats::M3Track* tr = find_track(m, seq, a.anim_id);
    if (!tr && global)
        for (const ClipLayer& L : *global)
            if ((tr = find_track(m, L.seq, a.anim_id)) != nullptr) {
                t = L.time_ms + (L.seq < m.sequences.size() ? float(m.sequences[L.seq].start_ms) : 0.0f);
                break;
            }
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

bool region_visible(const formats::Model& m, int seq, float time_ms, std::size_t region, const ClipLayers* global) {
    static const bool kOff = std::getenv("SC2_NO_BATCH_BONE_VISIBILITY") != nullptr || std::getenv("SC2_NO_BONE_VISIBILITY") != nullptr;
    if (kOff || seq < 0 || region >= m.region_bone.size()) return true;
    const int b = m.region_bone[region];
    if (b < 0 || std::size_t(b) >= m.bones.size()) return true;
    const formats::M3Bone& bone = m.bones[std::size_t(b)];
    if (bone.visibility.anim_flags == 0) return true;
    float V[1] = {bone.visibility.init[0]};
    field(m, std::uint32_t(seq), bone.visibility, time_ms, 1, V, global);
    return V[0] >= 0.5f;
}

std::vector<Mat4> pose_world(const formats::Model& m, uint32_t seq, float time_ms) {
    return pose_world(m, seq, time_ms, -1, 0.0f);
}

std::vector<Mat4> pose_world(const formats::Model& m, uint32_t seq, float time_ms, int yaw_bone, float yaw_rad,
                             const ClipLayers* global) {
    size_t n = m.bones.size();
    std::vector<Mat4> world(n);
    if (n == 0) return world;

    float t = time_ms;
    if (seq < m.sequences.size()) t += float(m.sequences[seq].start_ms);

    static const bool kUiKeep = std::getenv("SC2_BONE_VIS_UI_KEEP") != nullptr;
    const bool ui_model = kUiKeep && m.name.find("\\UI\\") != std::string::npos;
    static const std::string kBoneVisTraceValue = std::getenv("SC2_BONE_VIS_TRACE") ? std::getenv("SC2_BONE_VIS_TRACE") : "";
    static const char* const kBoneVisTrace = std::getenv("SC2_BONE_VIS_TRACE") ? kBoneVisTraceValue.c_str() : nullptr;
    std::vector<Mat4> local(n);
    for (size_t b = 0; b < n; ++b) {
        const formats::M3Bone& bone = m.bones[b];
        float T[3], R[4] = {0, 0, 0, 1}, S[3] = {1, 1, 1};
        field(m, seq, bone.location, t, 3, T, global);
        field(m, seq, bone.rotation, t, 4, R, global);
        field(m, seq, bone.scale, t, 3, S, global);
        static const bool kBoneVis = std::getenv("SC2_NO_BONE_VISIBILITY") == nullptr;
        if (kBoneVisTrace && bone.visibility.anim_flags == 0) {
            static int told_static = 0;
            const char* want = std::getenv("SC2_BONE_VIS_MODEL");
            if (want && m.name.find(want) != std::string::npos && told_static++ < 600)
                std::fprintf(stderr, "[bonevis-static] %s parent %d model %s init %.0f\n", bone.name.c_str(), int(bone.parent), m.name.c_str(), double(bone.visibility.init[0]));
        }
        if (kBoneVis && !ui_model && bone.visibility.anim_flags != 0) {
            float V[1] = {bone.visibility.init[0]};
            field(m, seq, bone.visibility, t, 1, V, global);
            if (kBoneVisTrace) {
                static int told = 0;
                if (std::string(kBoneVisTrace) == "1" ? told++ < 400 : bone.name.find(kBoneVisTrace) != std::string::npos && told++ < 6) std::fprintf(stderr, "[bonevis] %s flags %x model %s seq %u t %.0f init %.0f now %.3f\n", bone.name.c_str(), unsigned(bone.flags), m.name.c_str(), unsigned(seq), double(t), double(bone.visibility.init[0]), double(V[0]));
            }
            if (V[0] < 0.5f) S[0] = S[1] = S[2] = 0.0f;
        }
        local[b] = translation(T[0], T[1], T[2]) * from_quat(R[0], R[1], R[2], R[3]) *
                   scaling(S[0], S[1], S[2]);
        if (int(b) == yaw_bone && yaw_rad != 0.0f) local[b] = local[b] * rotation_z(yaw_rad);
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
    return pose(m, seq, time_ms, -1, 0.0f);
}

std::vector<Mat4> pose(const formats::Model& m, uint32_t seq, float time_ms, int yaw_bone, float yaw_rad,
                       const ClipLayers* global) {
    std::vector<Mat4> world = pose_world(m, seq, time_ms, yaw_bone, yaw_rad, global);
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

int select_variation(const formats::Model& m, const std::string& token, uint64_t seed, double clock_ms, float* time_ms) {
    std::vector<int> cands;
    uint64_t total = 0;
    for (size_t i = 0; i < m.sequences.size(); ++i) {
        std::vector<std::string> t = tokenize(m.sequences[i].name);
        bool named = false, other = false;
        for (const std::string& s : t) {
            if (s == token) named = true;
            else if (s.find_first_not_of("0123456789") != std::string::npos) other = true;
        }
        if (!named || other) continue;
        cands.push_back(int(i));
        total += m.sequences[i].frequency ? m.sequences[i].frequency : 1;
    }
    if (time_ms) *time_ms = 0.0f;
    if (cands.empty()) return -1;
    double t = clock_ms > 0.0 ? clock_ms : 0.0;
    int seq = cands[0];
    for (uint64_t loop = 0; loop < 100000; ++loop) {
        uint64_t h = seed + loop * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33;
        uint64_t pick = h % total;
        seq = cands[0];
        for (int i : cands) {
            const uint64_t w = m.sequences[i].frequency ? m.sequences[i].frequency : 1;
            if (pick < w) { seq = i; break; }
            pick -= w;
        }
        const double len = double(m.sequences[size_t(seq)].length_ms());
        if (len <= 0.0 || t < len) break;
        t -= len;
    }
    if (time_ms) *time_ms = float(t);
    return seq;
}

int select_take(const formats::Model& m, int seq, uint64_t seed, uint32_t loop) {
    if (seq < 0 || size_t(seq) >= m.sequences.size()) return seq;
    auto words = [](const std::string& name) {
        std::vector<std::string> out;
        for (const std::string& s : tokenize(name))
            if (s.find_first_not_of("0123456789") != std::string::npos) out.push_back(s);
        std::sort(out.begin(), out.end());
        return out;
    };
    const std::vector<std::string> own = words(m.sequences[size_t(seq)].name);
    std::vector<int> cands;
    uint64_t total = 0;
    for (size_t i = 0; i < m.sequences.size(); ++i) {
        if (words(m.sequences[i].name) != own) continue;
        cands.push_back(int(i));
        total += m.sequences[i].frequency ? m.sequences[i].frequency : 1;
    }
    if (cands.size() < 2 || total == 0) return seq;
    uint64_t h = seed + uint64_t(loop) * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33;
    uint64_t pick = h % total;
    for (int i : cands) {
        const uint64_t w = m.sequences[i].frequency ? m.sequences[i].frequency : 1;
        if (pick < w) return i;
        pick -= w;
    }
    return seq;
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

static const formats::M3Track* find_layered(const formats::Model& m, uint32_t seq, uint32_t anim_id, float& t,
                                            const ClipLayers* global) {
    if (const formats::M3Track* tr = find_track(m, seq, anim_id)) return tr;
    if (global)
        for (const ClipLayer& L : *global)
            if (const formats::M3Track* tr = find_track(m, L.seq, anim_id)) {
                t = L.time_ms;
                return tr;
            }
    return nullptr;
}

void animated_layer_field(const formats::Model& model, uint32_t seq, uint16_t interp, uint16_t flags, uint32_t anim_id,
                          float time_ms, int comps, const float* fallback, float* out, const ClipLayers* global) {
    for (int c = 0; c < comps; ++c) out[c] = fallback[c];
    if (flags == 0) return;
    const formats::M3Track* tr = find_layered(model, seq, anim_id, time_ms, global);
    if (!tr) return;
    float v[4] = {0, 0, 0, 0};
    if (!sample(*tr, time_ms, interp, comps, v)) return;
    for (int c = 0; c < comps; ++c) out[c] = v[c];
}

float animated_scalar(const formats::Model& model, uint32_t seq, const formats::M3AnimRef& ref,
                      float time_ms, float fallback, const ClipLayers* global) {
    if (ref.anim_flags == 0) return fallback;
    const formats::M3Track* tr = find_layered(model, seq, ref.anim_id, time_ms, global);
    if (!tr) return fallback;
    float v = 0.0f;
    if (!sample(*tr, time_ms, ref.interp, 1, &v)) return fallback;
    return v;
}

}
