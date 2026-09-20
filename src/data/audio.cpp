#include "sc2/data/audio.hpp"

#include <algorithm>
#include <cstdlib>

#include "sc2/core/xml.hpp"
#include "sc2/data/catalog.hpp"

namespace sc2::data {

using core::XmlNode;

namespace {

std::string expand_id(std::string v, const std::string& id) {
    for (size_t p; (p = v.find("##id##")) != std::string::npos;)
        v.replace(p, 6, id);
    return v;
}

std::string asset_file(const XmlNode& e) {
    if (const std::string* f = e.attr("File")) return *f;
    for (const XmlNode& c : e.children)
        if (c.name == "File") {
            if (const std::string* v = c.attr("value")) return *v;
            return {};
        }
    return {};
}

std::vector<SoundAsset> resolve_assets(const std::vector<XmlNode>& fields,
                                       const std::string& id) {
    std::vector<std::string> order;
    std::unordered_map<std::string, SoundAsset> byidx;
    long auto_idx = 0;

    for (const XmlNode& c : fields) {
        if (c.name != "AssetArray") continue;

        std::string key;
        if (const std::string* ix = c.attr("index")) {
            key = *ix;
            char* end = nullptr;
            long n = std::strtol(ix->c_str(), &end, 10);
            if (end && *end == '\0' && n >= auto_idx) auto_idx = n + 1;
        } else {
            key = std::to_string(auto_idx++);
        }

        if (const std::string* rm = c.attr("removed"); rm && *rm == "1") {
            byidx.erase(key);
            order.erase(std::remove(order.begin(), order.end(), key), order.end());
            continue;
        }

        std::string file = asset_file(c);
        if (file.empty()) continue;

        SoundAsset a;
        a.file = expand_id(std::move(file), id);
        if (const std::string* v = c.attr("Volume")) a.volume = *v;
        if (const std::string* w = c.attr("Weight")) a.weight = *w;

        if (byidx.find(key) == byidx.end()) order.push_back(key);
        byidx[key] = std::move(a);
    }

    std::vector<SoundAsset> out;
    out.reserve(order.size());
    for (const std::string& k : order) out.push_back(byidx[k]);
    return out;
}

}

Soundtrack resolve_soundtrack(const Catalog& cat, const std::string& id) {
    Soundtrack st;
    st.id = id;
    if (!cat.find("CSoundtrack", id)) return st;
    st.found = true;
    for (const XmlNode& c : cat.resolved("CSoundtrack", id)) {
        if (c.name == "CueArray") {
            for (const XmlNode& k : c.children)
                if (k.name == "MasterLayer")
                    if (const std::string* snd = k.attr("Sound")) st.master_sound = expand_id(*snd, id);
        } else if (c.name == "Delay") {
            const std::string v = c.attr_or("value", "");
            const std::size_t comma = v.find(',');
            st.delay_min = float(std::atof(v.c_str()));
            st.delay_max = comma == std::string::npos ? st.delay_min : float(std::atof(v.c_str() + comma + 1));
        } else if (c.name == "Select") {
            st.select = c.attr_or("value", "");
        } else if (c.name == "Next") {
            st.next = c.attr_or("value", "");
        } else if (c.name == "Flags") {
            if (c.attr_or("index", "") == "Continuous") st.continuous = c.attr_or("value", "1") != "0";
        }
    }
    return st;
}

void AudioCatalog::build(const Catalog& cat) {
    sounds_.clear();

    for (const std::string& id : cat.ids("CSound")) {
        Sound s;
        s.id = id;
        if (const Catalog::Entry* e = cat.find("CSound", id)) {
            s.parent = e->parent;
            s.is_default = e->is_default;
        }

        const std::vector<XmlNode>& fields = cat.resolved("CSound", id);
        s.assets = resolve_assets(fields, id);

        s.volume = cat.scalar("CSound", id, "Volume");
        s.category = cat.scalar("CSound", id, "Category");
        s.select = cat.scalar("CSound", id, "Select");
        s.mode = cat.scalar("CSound", id, "Mode");
        s.chance = cat.scalar("CSound", id, "Chance");
        s.pitch = cat.scalar("CSound", id, "Pitch");

        for (const XmlNode& c : fields) {
            if (c.name == "VolumeRolloffFadeBlend") {
                s.rolloff_blend = c.attr_or("value", "");
                continue;
            }
            if (c.name != "VolumeRolloffPoints") continue;
            const std::string* dist = c.attr("Distance");
            if (!dist || dist->empty()) {
                s.rolloff.clear();
                continue;
            }
            Sound::RolloffPoint pt;
            pt.distance = float(std::atof(dist->c_str()));
            if (const std::string* vol = c.attr("Volume")) pt.volume_db = float(std::atof(vol->c_str()));
            s.rolloff.push_back(pt);
        }
        for (const XmlNode& c : fields) {
            if (c.name != "Flags") continue;
            std::string idx = c.attr_or("index", "");
            std::string val = c.attr_or("value", "");
            bool found = false;
            for (auto& kv : s.flags)
                if (kv.first == idx) { kv.second = val; found = true; break; }
            if (!found) s.flags.emplace_back(idx, val);
        }

        sounds_[id] = std::move(s);
    }
}

const Sound* AudioCatalog::sound(const std::string& id) const {
    auto it = sounds_.find(id);
    return it == sounds_.end() ? nullptr : &it->second;
}

std::vector<std::string> AudioCatalog::ids() const {
    std::vector<std::string> out;
    out.reserve(sounds_.size());
    for (const auto& [id, s] : sounds_) {
        (void)s;
        out.push_back(id);
    }
    return out;
}

std::vector<std::string> AudioCatalog::resolve(const std::string& id) const {
    std::vector<std::string> out;
    const Sound* s = sound(id);
    if (!s) return out;
    out.reserve(s->assets.size());
    for (const SoundAsset& a : s->assets) out.push_back(a.file);
    return out;
}

}
