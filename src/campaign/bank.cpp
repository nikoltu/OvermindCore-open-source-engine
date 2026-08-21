#include "sc2/campaign/bank.hpp"

#include <cstdio>
#include <sstream>

#include "sc2/core/xml.hpp"

namespace sc2::campaign {

void Bank::set_int(const std::string& section, const std::string& key,
                   int64_t v) {
    sections_[section][key] = BankValue::make_int(v);
}
void Bank::set_flag(const std::string& section, const std::string& key, bool v) {
    sections_[section][key] = BankValue::make_flag(v);
}
void Bank::set_string(const std::string& section, const std::string& key,
                      const std::string& v) {
    sections_[section][key] = BankValue::make_string(v);
}

const BankValue* Bank::get(const std::string& section,
                           const std::string& key) const {
    auto si = sections_.find(section);
    if (si == sections_.end()) return nullptr;
    auto ki = si->second.find(key);
    if (ki == si->second.end()) return nullptr;
    return &ki->second;
}
bool Bank::has(const std::string& section, const std::string& key) const {
    return get(section, key) != nullptr;
}
int64_t Bank::get_int(const std::string& section, const std::string& key,
                      int64_t def) const {
    const BankValue* v = get(section, key);
    return v ? v->i : def;
}
bool Bank::get_flag(const std::string& section, const std::string& key,
                    bool def) const {
    const BankValue* v = get(section, key);
    return v ? (v->i != 0) : def;
}
std::string Bank::get_string(const std::string& section, const std::string& key,
                             const std::string& def) const {
    const BankValue* v = get(section, key);
    return v ? v->s : def;
}

static void xml_escape(const std::string& in, std::string& out) {
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
}

std::string Bank::to_xml(bool with_signature) const {
    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    out += "<Bank version=\"1\">\n";
    for (const auto& [sname, keys] : sections_) {
        out += "    <Section name=\"";
        xml_escape(sname, out);
        out += "\">\n";
        for (const auto& [kname, val] : keys) {
            out += "        <Key name=\"";
            xml_escape(kname, out);
            out += "\">";
            switch (val.type) {
                case BankValue::Type::Int:
                    out += "<Value int=\"" + std::to_string(val.i) + "\"/>";
                    break;
                case BankValue::Type::Flag:
                    out += "<Value flag=\"";
                    out += (val.i != 0) ? "1" : "0";
                    out += "\"/>";
                    break;
                case BankValue::Type::String:
                    out += "<Value string=\"";
                    xml_escape(val.s, out);
                    out += "\"/>";
                    break;
            }
            out += "</Key>\n";
        }
        out += "    </Section>\n";
    }
    if (with_signature) {
        out += "    <Signature value=\"";
        xml_escape(placeholder_signature(), out);
        out += "\"/>\n";
    }
    out += "</Bank>\n";
    return out;
}

bool Bank::from_xml(const std::string& xml, Bank& out) {
    out.sections_.clear();
    core::XmlNode root;
    try {
        root = core::xml_parse(xml);
    } catch (...) {
        return false;
    }
    if (root.name != "Bank") return false;

    for (const core::XmlNode* section : root.elements("Section")) {
        const std::string* sname = section->attr("name");
        if (!sname) return false;
        for (const core::XmlNode* key : section->elements("Key")) {
            const std::string* kname = key->attr("name");
            if (!kname) return false;
            const auto values = key->elements("Value");
            if (values.empty()) return false;
            const core::XmlNode* value = values.front();
            if (const std::string* iv = value->attr("int")) {
                out.set_int(*sname, *kname,
                            static_cast<int64_t>(std::stoll(*iv)));
            } else if (const std::string* fv = value->attr("flag")) {
                out.set_flag(*sname, *kname, *fv != "0");
            } else if (const std::string* sv = value->attr("string")) {
                out.set_string(*sname, *kname, *sv);
            } else {
                return false;
            }
        }
    }
    return true;
}

std::string Bank::placeholder_signature() const {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](const std::string& s) {
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ULL;
        }
    };
    for (const auto& [sname, keys] : sections_) {
        mix(sname);
        for (const auto& [kname, val] : keys) {
            mix(kname);
            switch (val.type) {
                case BankValue::Type::Int: mix("i" + std::to_string(val.i)); break;
                case BankValue::Type::Flag: mix(val.i ? "f1" : "f0"); break;
                case BankValue::Type::String: mix("s" + val.s); break;
            }
        }
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llX",
                  static_cast<unsigned long long>(h));
    return std::string("PLACEHOLDER-") + buf;
}

}
