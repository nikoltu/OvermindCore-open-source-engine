#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc2::casc {
class Storage;
}

namespace sc2::asset {

class AssetManager {
public:
    explicit AssetManager(const casc::Storage& store) : store_(store) {}

    struct PrewarmResult {
        size_t loaded = 0;
        size_t missing = 0;
        size_t bytes = 0;
    };

    PrewarmResult prewarm(const std::vector<std::string>& names);

    const std::vector<uint8_t>& get(const std::string& name);

    bool is_cached(const std::string& name) const {
        return cache_.find(name) != cache_.end();
    }
    size_t cached_count() const { return cache_.size(); }
    size_t cached_bytes() const { return bytes_; }
    void clear() {
        cache_.clear();
        bytes_ = 0;
    }

private:
    const casc::Storage& store_;
    std::unordered_map<std::string, std::vector<uint8_t>> cache_;
    size_t bytes_ = 0;
};

}
