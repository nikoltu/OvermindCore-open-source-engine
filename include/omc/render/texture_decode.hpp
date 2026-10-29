#pragma once
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "omc/formats/m3.hpp"

namespace omc::casc {
class Storage;
}

namespace omc::render {

struct DecodedTexture {
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
    bool ok = false;
    bool sphere = false;
    std::shared_ptr<formats::M3> m3;
    std::shared_ptr<formats::Model> model;
    int pick = -1;
};

std::vector<std::string> texture_roots_2d(const std::string& texture_root);
std::vector<std::string> texture_roots_env(const std::string& texture_root);

DecodedTexture decode_texture_2d(const casc::Storage& store, const std::string& texture_root, const std::string& path, bool normal,
                                 bool triplanar, std::uint32_t max_dim = 0);
DecodedTexture decode_texture_env(const casc::Storage& store, const std::string& texture_root, const std::string& path);

std::string texture_key_2d(const std::string& path, bool normal, bool triplanar, std::uint32_t max_dim);
std::string texture_key_env(const std::string& path);

inline bool& world_async_loads() {
    static bool on = false;
    return on;
}

inline bool& texture_take_once() {
    static bool on = true;
    return on;
}
inline std::size_t& upload_batch_budget_mb() {
    static std::size_t mb = 16;
    return mb;
}
inline std::uint32_t& portrait_texture_cap() {
    static std::uint32_t v = [] {
        const char* e = std::getenv("OMC_PORTRAIT_TEX_MAX");
        const int n = e ? std::atoi(e) : 1024;
        return std::uint32_t(n > 0 ? n : 0);
    }();
    return v;
}
inline bool& console_texture_half() {
    static bool on = false;
    return on;
}
void halve_decoded(DecodedTexture& t);

class TextureDecodePool {
public:
    static TextureDecodePool& instance();
    static TextureDecodePool* existing();
    ~TextureDecodePool();
    void request(const std::string& key, std::function<DecodedTexture()> job);
    bool has(const std::string& key) const;
    bool ready(const std::string& key) const;
    std::optional<DecodedTexture> take(const std::string& key);
    void trim(std::size_t keep);
    void drain();
    void erase(const std::string& key);
    std::size_t size() const;
    std::size_t bytes() const;

private:
    TextureDecodePool();
    struct Entry {
        bool done = false;
        std::uint64_t seq = 0;
        DecodedTexture tex;
        std::function<DecodedTexture()> job;
    };
    void worker();
    mutable std::mutex mu_;
    std::condition_variable cv_job_, cv_done_;
    std::unordered_map<std::string, std::shared_ptr<Entry>> entries_;
    std::deque<std::string> queue_;
    std::vector<std::thread> threads_;
    std::uint64_t seq_ = 0;
    int running_ = 0;
    bool stop_ = false;
};

}
