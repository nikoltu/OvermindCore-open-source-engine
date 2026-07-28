#include "sc2/asset/manager.hpp"

#include "sc2/casc/blte.hpp"
#include "sc2/casc/storage.hpp"

namespace sc2::asset {

AssetManager::PrewarmResult AssetManager::prewarm(
    const std::vector<std::string>& names) {
    PrewarmResult r;
    for (const std::string& name : names) {
        if (is_cached(name)) continue;
        try {
            auto data = store_.read_by_name(name);
            bytes_ += data.size();
            cache_.emplace(name, std::move(data));
            ++r.loaded;
        } catch (const casc::BlteEncrypted&) {
            ++r.missing;
        } catch (const std::exception&) {
            ++r.missing;
        }
    }
    r.bytes = bytes_;
    return r;
}

const std::vector<uint8_t>& AssetManager::get(const std::string& name) {
    auto it = cache_.find(name);
    if (it != cache_.end()) return it->second;
    auto data = store_.read_by_name(name);
    bytes_ += data.size();
    return cache_.emplace(name, std::move(data)).first->second;
}

}
