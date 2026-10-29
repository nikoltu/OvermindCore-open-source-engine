#include "omc/data/strings.hpp"

namespace omc::data {

static bool starts_with(const std::string& s, const char* prefix) {
    std::size_t i = 0;
    for (; prefix[i]; ++i)
        if (i >= s.size() || s[i] != prefix[i]) return false;
    return true;
}

void StringTable::load_text(const std::string& text) {
    std::size_t i = 0, n = text.size();
    if (n >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        i = 3;

    while (i < n) {
        std::size_t nl = text.find('\n', i);
        std::size_t end = (nl == std::string::npos) ? n : nl;
        std::size_t line_end = end;
        if (line_end > i && text[line_end - 1] == '\r') --line_end;

        std::size_t p = i;
        while (p < line_end && (text[p] == ' ' || text[p] == '\t')) ++p;
        bool blank = (p == line_end);
        bool comment = (p + 1 < line_end && text[p] == '/' && text[p + 1] == '/');

        if (!blank && !comment) {
            std::size_t eq = text.find('=', p);
            if (eq != std::string::npos && eq < line_end) {
                std::string key = text.substr(p, eq - p);
                std::string val = text.substr(eq + 1, line_end - (eq + 1));
                map_[key] = val;
            }
        }

        if (nl == std::string::npos) break;
        i = nl + 1;
    }
}

void StringTable::load(const std::vector<uint8_t>& bytes) {
    load_text(std::string(bytes.begin(), bytes.end()));
}

static std::string strip_ref_prefix(const std::string& ref, int* depth) {
    if (starts_with(ref, "@Text/")) {
        *depth = 1;
        return ref.substr(6);
    }
    if (ref.empty() || ref[0] != '@') {
        *depth = 0;
        return ref;
    }
    std::size_t n = 0;
    while (n < ref.size() && n < 3 && ref[n] == '@') ++n;
    *depth = int(n);
    return ref.substr(n);
}

std::string StringTable::resolve(const std::string& ref) const {
    int depth = 0;
    const std::string key = strip_ref_prefix(ref, &depth);
    auto it = map_.find(key);
    return (it != map_.end()) ? it->second : ref;
}

std::string StringTable::resolve(const std::string& ref, const std::string& race,
                                 const std::string& skin) const {
    int depth = 0;
    const std::string key = strip_ref_prefix(ref, &depth);
    if (depth >= 3 && !skin.empty()) {
        auto it = map_.find(key + "_" + skin);
        if (it != map_.end()) return it->second;
    }
    if (depth >= 2 && !race.empty()) {
        auto it = map_.find(key + "_" + race);
        if (it != map_.end()) return it->second;
    }
    auto it = map_.find(key);
    return (it != map_.end()) ? it->second : ref;
}

const std::string* StringTable::find(const std::string& key) const {
    auto it = map_.find(key);
    return (it != map_.end()) ? &it->second : nullptr;
}

}
