#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace sc2::campaign {

struct BankValue {
    enum class Type { Int, Flag, String };
    Type type = Type::Int;
    int64_t i = 0;
    std::string s;

    static BankValue make_int(int64_t v) { return BankValue{Type::Int, v, {}}; }
    static BankValue make_flag(bool v) { return BankValue{Type::Flag, v ? 1 : 0, {}}; }
    static BankValue make_string(std::string v) {
        return BankValue{Type::String, 0, std::move(v)};
    }
    bool operator==(const BankValue& o) const {
        return type == o.type && i == o.i && s == o.s;
    }
    bool operator!=(const BankValue& o) const { return !(*this == o); }
};

class Bank {
public:
    using Section = std::map<std::string, BankValue>;

    void set_int(const std::string& section, const std::string& key, int64_t v);
    void set_flag(const std::string& section, const std::string& key, bool v);
    void set_string(const std::string& section, const std::string& key,
                    const std::string& v);

    bool has(const std::string& section, const std::string& key) const;
    const BankValue* get(const std::string& section, const std::string& key) const;
    int64_t get_int(const std::string& section, const std::string& key,
                    int64_t def = 0) const;
    bool get_flag(const std::string& section, const std::string& key,
                  bool def = false) const;
    std::string get_string(const std::string& section, const std::string& key,
                           const std::string& def = std::string()) const;

    bool empty() const { return sections_.empty(); }
    std::size_t section_count() const { return sections_.size(); }
    const std::map<std::string, Section>& sections() const { return sections_; }

    std::string to_xml(bool with_signature = false) const;

    static bool from_xml(const std::string& xml, Bank& out);

    std::string placeholder_signature() const;

private:
    std::map<std::string, Section> sections_;
};

}
