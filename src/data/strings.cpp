#include "sc2/data/strings.hpp"

namespace sc2::data {

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

std::string StringTable::resolve(const std::string& ref) const {
    std::string key;
    if (starts_with(ref, "@Text/"))
        key = ref.substr(6);
    else if (starts_with(ref, "@@"))
        key = ref.substr(2);
    else if (!ref.empty() && ref[0] == '@')
        key = ref.substr(1);
    else
        key = ref;

    auto it = map_.find(key);
    return (it != map_.end()) ? it->second : ref;
}

const std::string* StringTable::find(const std::string& key) const {
    auto it = map_.find(key);
    return (it != map_.end()) ? &it->second : nullptr;
}

}
