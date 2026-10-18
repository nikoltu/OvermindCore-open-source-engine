#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc2::data {

class StringTable {
public:
    void load(const std::vector<uint8_t>& bytes);
    void load_text(const std::string& text);

    std::string resolve(const std::string& ref) const;

    std::string resolve(const std::string& ref, const std::string& race,
                        const std::string& skin = std::string()) const;

    const std::string* find(const std::string& key) const;

    std::size_t size() const { return map_.size(); }
    bool empty() const { return map_.empty(); }

private:
    std::unordered_map<std::string, std::string> map_;
};

}
