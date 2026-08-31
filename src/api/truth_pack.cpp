#include "sc2/api/truth_pack.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace sc2::api {

namespace {

bool find_string_value(const std::string& t, std::size_t& i, std::string& out) {
    while (i < t.size() && t[i] != '"') {
        if (t[i] == ',' || t[i] == '}') return false;
        ++i;
    }
    if (i >= t.size()) return false;
    ++i;
    out.clear();
    while (i < t.size() && t[i] != '"') {
        if (t[i] == '\\' && i + 1 < t.size()) ++i;
        out.push_back(t[i]);
        ++i;
    }
    if (i < t.size()) ++i;
    return true;
}

bool find_number_value(const std::string& t, std::size_t& i, long& out) {
    while (i < t.size() && (t[i] == ' ' || t[i] == ':')) ++i;
    const std::size_t start = i;
    while (i < t.size() && (t[i] == '-' || (t[i] >= '0' && t[i] <= '9'))) ++i;
    if (i == start) return false;
    out = std::strtol(t.substr(start, i - start).c_str(), nullptr, 10);
    return true;
}

}

UnitTypePack load_unit_type_pack(const std::string& root, std::uint32_t data_build) {
    UnitTypePack pack;
    pack.data_build = data_build;
    if (root.empty()) {
        pack.error =
            "no data truth pack root is configured. Set TRUTH_DATA_DIR (or pass --truth-data) to "
            "the directory verification/data_truth_pack.py writes.";
        return pack;
    }
    pack.path = root + "/" + std::to_string(data_build) + "/unit_types.json";
    std::ifstream in(pack.path, std::ios::binary);
    if (!in) {
        pack.error = "no data truth pack for build " + std::to_string(data_build) + " at " +
                     pack.path +
                     ". The id space is joined by NAME through that pack, so without it this "
                     "client cannot name a unit the way the game does. Re-capture it with: "
                     "py verification/data_truth_pack.py";
        return pack;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    const std::string kIdKey = "\"unit_id\"";
    const std::string kNameKey = "\"name\"";
    std::size_t at = 0;
    while (true) {
        const std::size_t id_at = text.find(kIdKey, at);
        if (id_at == std::string::npos) break;
        std::size_t i = id_at + kIdKey.size();
        while (i < text.size() && (text[i] == ' ' || text[i] == ':')) ++i;
        long id = -1;
        if (!find_number_value(text, i, id)) {
            at = id_at + kIdKey.size();
            continue;
        }
        const std::size_t name_at = text.find(kNameKey, i);
        if (name_at == std::string::npos) break;
        std::size_t j = name_at + kNameKey.size();
        while (j < text.size() && (text[j] == ' ' || text[j] == ':')) ++j;
        std::string name;
        if (find_string_value(text, j, name) && !name.empty() && id >= 0)
            pack.id_by_name.emplace(name, std::uint32_t(id));
        at = j;
    }
    if (pack.id_by_name.empty()) {
        pack.error = "the data truth pack at " + pack.path +
                     " carried no unit_id/name rows. It is present but unusable; re-capture it "
                     "with: py verification/data_truth_pack.py";
        return pack;
    }
    pack.loaded = true;
    return pack;
}

AbilityPack load_ability_pack(const std::string& root, std::uint32_t data_build) {
    AbilityPack pack;
    pack.data_build = data_build;
    if (root.empty()) {
        pack.error =
            "no data truth pack root is configured. Set TRUTH_DATA_DIR (or pass --truth-data) to "
            "the directory verification/data_truth_pack.py writes.";
        return pack;
    }
    pack.path = root + "/" + std::to_string(data_build) + "/abilities.json";
    std::ifstream in(pack.path, std::ios::binary);
    if (!in) {
        pack.error = "no ability truth pack for build " + std::to_string(data_build) + " at " +
                     pack.path +
                     ". Order ability ids are joined by NAME through that pack, so without it this "
                     "client publishes no ability id at all rather than an invented one. "
                     "Re-capture it with: py verification/data_truth_pack.py";
        return pack;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    const std::string kIdKey = "\"ability_id\"";
    const std::string kRemapKey = "\"remaps_to_ability_id\"";
    static const char* const kNameKeys[] = {"\"link_name\"", "\"button_name\"",
                                            "\"friendly_name\""};
    std::size_t at = 0;
    while (true) {
        const std::size_t id_at = text.find(kIdKey, at);
        if (id_at == std::string::npos) break;
        std::size_t i = id_at + kIdKey.size();
        while (i < text.size() && (text[i] == ' ' || text[i] == ':')) ++i;
        long id = -1;
        if (!find_number_value(text, i, id)) {
            at = id_at + kIdKey.size();
            continue;
        }
        const std::size_t next = text.find(kIdKey, i);
        const std::size_t end = next == std::string::npos ? text.size() : next;
        at = end;
        if (id < 0) continue;
        const std::size_t remap_at = text.find(kRemapKey, i);
        if (remap_at != std::string::npos && remap_at < end) {
            std::size_t r = remap_at + kRemapKey.size();
            while (r < text.size() && (text[r] == ' ' || text[r] == ':')) ++r;
            long remap = -1;
            if (find_number_value(text, r, remap) && remap > 0)
                pack.remaps_to.emplace(std::uint32_t(id), std::uint32_t(remap));
        }
        for (const char* key : kNameKeys) {
            const std::size_t name_at = text.find(key, i);
            if (name_at == std::string::npos || name_at >= end) continue;
            std::size_t j = name_at + std::strlen(key);
            while (j < text.size() && (text[j] == ' ' || text[j] == ':')) ++j;
            std::string name;
            if (!find_string_value(text, j, name) || name.empty()) continue;
            pack.id_by_name.emplace(name, std::uint32_t(id));
        }
    }
    if (pack.id_by_name.empty()) {
        pack.error = "the ability truth pack at " + pack.path +
                     " carried no ability_id/name rows. It is present but unusable; re-capture it "
                     "with: py verification/data_truth_pack.py";
        return pack;
    }
    pack.loaded = true;
    return pack;
}

}
