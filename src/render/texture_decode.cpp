#include "omc/render/texture_decode.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "omc/casc/storage.hpp"
#include "omc/formats/dds.hpp"
#include "omc/render/terrain_tex.hpp"

namespace omc::render {

std::vector<std::string> texture_roots_2d(const std::string& texture_root) {
    std::vector<std::string> roots = {texture_root};
    for (const char* r : {"mods/liberty.sc2mod/base.sc2assets/", "mods/core.sc2mod/base.sc2assets/",
                          "mods/void.sc2mod/base.sc2assets/", "mods/swarm.sc2mod/base.sc2assets/", "",
                          "mods/liberty.sc2mod/enus.sc2assets/", "mods/swarm.sc2mod/enus.sc2assets/",
                          "mods/void.sc2mod/enus.sc2assets/", "mods/core.sc2mod/enus.sc2assets/"})
        roots.push_back(r);
    return roots;
}

std::vector<std::string> texture_roots_env(const std::string& texture_root) {
    std::vector<std::string> roots = {texture_root};
    for (const char* r : {"mods/liberty.sc2mod/base.sc2assets/", "mods/core.sc2mod/base.sc2assets/",
                          "mods/void.sc2mod/base.sc2assets/", "mods/swarm.sc2mod/base.sc2assets/", ""})
        roots.push_back(r);
    return roots;
}

std::string texture_key_2d(const std::string& path, bool normal, bool triplanar, std::uint32_t max_dim) {
    return path + (normal ? "#n" : "") + (triplanar ? "#tri" : "") + (max_dim ? "#max" + std::to_string(max_dim) : std::string());
}

std::string texture_key_env(const std::string& path) { return path + "#envll"; }

static void fit_max_dim(DecodedTexture& t, std::uint32_t max_dim) {
    if (!max_dim) return;
    while ((t.w > max_dim || t.h > max_dim) && t.w > 1 && t.h > 1) {
        const std::uint32_t nw = std::max(1u, t.w / 2), nh = std::max(1u, t.h / 2);
        std::vector<std::uint8_t> out(std::size_t(nw) * nh * 4);
        for (std::uint32_t y = 0; y < nh; ++y)
            for (std::uint32_t x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    unsigned s = 0;
                    for (std::uint32_t dy = 0; dy < 2; ++dy)
                        for (std::uint32_t dx = 0; dx < 2; ++dx) {
                            const std::uint32_t sx = std::min(t.w - 1, x * 2 + dx), sy = std::min(t.h - 1, y * 2 + dy);
                            s += t.rgba[(std::size_t(sy) * t.w + sx) * 4 + std::size_t(c)];
                        }
                    out[(std::size_t(y) * nw + x) * 4 + std::size_t(c)] = std::uint8_t((s + 2) / 4);
                }
        t.rgba = std::move(out);
        t.w = nw;
        t.h = nh;
    }
}

void halve_decoded(DecodedTexture& t) {
    if (t.ok && t.w > 1 && t.h > 1) fit_max_dim(t, std::max(t.w, t.h) / 2);
}

DecodedTexture decode_texture_2d(const casc::Storage& store, const std::string& texture_root, const std::string& path, bool normal,
                                 bool triplanar, std::uint32_t max_dim) {
    DecodedTexture out;
    for (const std::string& root : texture_roots_2d(texture_root)) {
        try {
            auto bytes = store.read_by_name(root + path);
            formats::DdsInfo info;
            auto rgba = formats::dds_decode_rgba(bytes, info);
            if (!rgba.empty() && info.width && info.height) {
                if (normal) dxt5nm_to_rgb(rgba);
                if (triplanar) {
                    std::string sib = path;
                    const std::size_t at = sib.rfind("_00.");
                    bool built = false;
                    if (at != std::string::npos) {
                        sib.replace(at, 3, "_01");
                        try {
                            formats::DdsInfo si;
                            auto top = formats::dds_decode_rgba(store.read_by_name(root + sib), si);
                            if (!top.empty() && si.width == info.width && si.height == info.height) {
                                if (normal) dxt5nm_to_rgb(top);
                                const std::size_t W = info.width, H = info.height;
                                std::vector<uint8_t> atlas(W * 2 * H * 4);
                                for (std::size_t y = 0; y < H; ++y) {
                                    std::memcpy(&atlas[y * W * 2 * 4], &top[y * W * 4], W * 4);
                                    std::memcpy(&atlas[(y * W * 2 + W) * 4], &rgba[y * W * 4], W * 4);
                                }
                                out.w = std::uint32_t(W * 2);
                                out.h = std::uint32_t(H);
                                out.rgba = std::move(atlas);
                                out.ok = true;
                                built = true;
                            }
                        } catch (const std::exception&) {}
                    }
                    if (!built) {
                        const std::size_t W = info.width, H = info.height;
                        std::vector<uint8_t> atlas(W * 2 * H * 4);
                        for (std::size_t y = 0; y < H; ++y) {
                            std::memcpy(&atlas[y * W * 2 * 4], &rgba[y * W * 4], W * 4);
                            std::memcpy(&atlas[(y * W * 2 + W) * 4], &rgba[y * W * 4], W * 4);
                        }
                        out.w = std::uint32_t(W * 2);
                        out.h = std::uint32_t(H);
                        out.rgba = std::move(atlas);
                        out.ok = true;
                    }
                    break;
                }
                out.w = info.width;
                out.h = info.height;
                out.rgba = std::move(rgba);
                out.ok = true;
                break;
            }
        } catch (const std::exception&) {   }
    }
    if (out.ok) fit_max_dim(out, max_dim);
    return out;
}

DecodedTexture decode_texture_env(const casc::Storage& store, const std::string& texture_root, const std::string& path) {
    DecodedTexture out;
    for (const std::string& root : texture_roots_env(texture_root)) {
        try {
            const std::vector<uint8_t> bytes = store.read_by_name(root + path);
            if (bytes.size() < 128 + 6) continue;
            const uint32_t caps2 = uint32_t(bytes[112]) | uint32_t(bytes[113]) << 8 | uint32_t(bytes[114]) << 16 | uint32_t(bytes[115]) << 24;
            const bool dx10 = bytes[84] == 'D' && bytes[85] == 'X' && bytes[86] == '1' && bytes[87] == '0';
            if (dx10) break;
            if (!(caps2 & 0x200u)) {
                if (std::getenv("OMC_NO_SPHERE_ENVIO")) break;
                formats::DdsInfo info;
                std::vector<uint8_t> rgba = formats::dds_decode_rgba(bytes, info);
                if (!rgba.empty() && info.width && info.height) {
                    out.w = info.width;
                    out.h = info.height;
                    out.rgba = std::move(rgba);
                    out.ok = true;
                    out.sphere = true;
                }
                break;
            }
            const size_t face = (bytes.size() - 128) / 6;
            std::vector<uint8_t> faces[6];
            uint32_t fw = 0, fh = 0;
            bool ok = true;
            for (int k = 0; k < 6 && ok; ++k) {
                std::vector<uint8_t> one(128 + face);
                std::memcpy(one.data(), bytes.data(), 128);
                std::memcpy(one.data() + 128, bytes.data() + 128 + size_t(k) * face, face);
                one[112] = one[113] = one[114] = one[115] = 0;
                formats::DdsInfo info;
                faces[k] = formats::dds_decode_rgba(one, info);
                if (faces[k].empty() || info.width == 0 || info.height == 0 || (k && (info.width != fw || info.height != fh))) ok = false;
                fw = info.width;
                fh = info.height;
            }
            if (!ok) break;
            const uint32_t W = 512, H = 256;
            std::vector<uint8_t> ll(size_t(W) * H * 4, 255);
            for (uint32_t j = 0; j < H; ++j)
                for (uint32_t i = 0; i < W; ++i) {
                    const float ph = ((float(i) + 0.5f) / float(W) - 0.5f) * 6.2831853f;
                    const float th = (float(j) + 0.5f) / float(H) * 3.14159265f;
                    const float x = std::sin(th) * std::cos(ph), y = std::sin(th) * std::sin(ph), z = std::cos(th);
                    const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
                    int f = 0;
                    float sc = 0, tc = 0, ma = 1;
                    if (ax >= ay && ax >= az) { f = x > 0 ? 0 : 1; sc = x > 0 ? -z : z; tc = -y; ma = ax; }
                    else if (ay >= az) { f = y > 0 ? 2 : 3; sc = x; tc = y > 0 ? z : -z; ma = ay; }
                    else { f = z > 0 ? 4 : 5; sc = z > 0 ? x : -x; tc = -y; ma = az; }
                    const uint32_t px = std::min(fw - 1, uint32_t(std::max(0.0f, (sc / ma * 0.5f + 0.5f) * float(fw))));
                    const uint32_t py = std::min(fh - 1, uint32_t(std::max(0.0f, (tc / ma * 0.5f + 0.5f) * float(fh))));
                    const uint8_t* s = &faces[f][(size_t(py) * fw + px) * 4];
                    uint8_t* d = &ll[(size_t(j) * W + i) * 4];
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                }
            out.w = W;
            out.h = H;
            out.rgba = std::move(ll);
            out.ok = true;
            break;
        } catch (const std::exception&) {   }
    }
    return out;
}

static std::atomic<TextureDecodePool*> g_pool{nullptr};

TextureDecodePool& TextureDecodePool::instance() {
    static TextureDecodePool pool;
    g_pool.store(&pool);
    return pool;
}

TextureDecodePool* TextureDecodePool::existing() { return g_pool.load(); }

TextureDecodePool::TextureDecodePool() {
    unsigned n = 2;
    if (const char* e = std::getenv("OMC_TEX_DECODE_THREADS")) n = unsigned(std::max(1, std::min(8, std::atoi(e))));
    for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this] { worker(); });
}

TextureDecodePool::~TextureDecodePool() {
    g_pool.store(nullptr);
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        queue_.clear();
    }
    cv_job_.notify_all();
    for (std::thread& t : threads_)
        if (t.joinable()) t.join();
}

void TextureDecodePool::worker() {
    for (;;) {
        std::shared_ptr<Entry> e;
        std::function<DecodedTexture()> job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_job_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            const std::string key = queue_.front();
            queue_.pop_front();
            auto it = entries_.find(key);
            if (it == entries_.end() || it->second->done || !it->second->job) continue;
            e = it->second;
            job = std::move(e->job);
            e->job = nullptr;
            ++running_;
        }
        DecodedTexture t;
        try { t = job(); } catch (...) { t = DecodedTexture{}; }
        {
            std::lock_guard<std::mutex> lk(mu_);
            e->tex = std::move(t);
            e->done = true;
            --running_;
        }
        cv_done_.notify_all();
    }
}

void TextureDecodePool::request(const std::string& key, std::function<DecodedTexture()> job) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_ || entries_.count(key)) return;
        auto e = std::make_shared<Entry>();
        e->seq = ++seq_;
        e->job = std::move(job);
        entries_.emplace(key, std::move(e));
        queue_.push_back(key);
    }
    cv_job_.notify_one();
}

bool TextureDecodePool::has(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.count(key) != 0;
}

bool TextureDecodePool::ready(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(key);
    return it != entries_.end() && it->second->done;
}

std::optional<DecodedTexture> TextureDecodePool::take(const std::string& key) {
    std::shared_ptr<Entry> e;
    std::function<DecodedTexture()> job;
    {
        std::unique_lock<std::mutex> lk(mu_);
        auto it = entries_.find(key);
        if (it == entries_.end()) return std::nullopt;
        e = it->second;
        if (!e->done && e->job) {
            job = std::move(e->job);
            e->job = nullptr;
        } else {
            cv_done_.wait(lk, [&] { return e->done; });
            DecodedTexture t = std::move(e->tex);
            entries_.erase(key);
            return t;
        }
    }
    DecodedTexture t;
    try { t = job(); } catch (...) { t = DecodedTexture{}; }
    {
        std::lock_guard<std::mutex> lk(mu_);
        e->done = true;
        auto it = entries_.find(key);
        if (it != entries_.end() && it->second == e) entries_.erase(it);
    }
    cv_done_.notify_all();
    return t;
}

void TextureDecodePool::drain() {
    std::unique_lock<std::mutex> lk(mu_);
    queue_.clear();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!it->second->done && it->second->job) it = entries_.erase(it);
        else ++it;
    }
    cv_done_.wait(lk, [this] { return running_ == 0; });
    entries_.clear();
}

void TextureDecodePool::erase(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.erase(key);
}

void TextureDecodePool::trim(std::size_t keep) {
    std::lock_guard<std::mutex> lk(mu_);
    if (entries_.size() <= keep) return;
    std::vector<std::pair<std::uint64_t, std::string>> done;
    for (const auto& kv : entries_)
        if (kv.second->done) done.emplace_back(kv.second->seq, kv.first);
    std::sort(done.begin(), done.end());
    for (std::size_t i = 0; i < done.size() && entries_.size() > keep; ++i) entries_.erase(done[i].second);
}

std::size_t TextureDecodePool::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
}

std::size_t TextureDecodePool::bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t b = 0;
    for (const auto& kv : entries_) b += kv.second->tex.rgba.size();
    return b;
}

}
