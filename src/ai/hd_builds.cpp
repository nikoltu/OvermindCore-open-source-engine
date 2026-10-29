#include "omc/ai/hd_builds.hpp"

#include <cctype>
#include <cstdlib>
#include <utility>

namespace omc::ai::hd {

namespace {

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string strip_comments(const std::string& t) {
    std::string out;
    out.reserve(t.size());
    for (std::size_t i = 0; i < t.size();) {
        if (t[i] == '"') {
            const std::size_t s = i++;
            while (i < t.size() && t[i] != '"' && t[i] != '\n') {
                if (t[i] == '\\' && i + 1 < t.size()) ++i;
                ++i;
            }
            if (i < t.size() && t[i] == '"') ++i;
            out.append(t, s, i - s);
        } else if (t.compare(i, 2, "//") == 0) {
            while (i < t.size() && t[i] != '\n') ++i;
        } else if (t.compare(i, 2, "/*") == 0) {
            const std::size_t e = t.find("*/", i + 2);
            i = e == std::string::npos ? t.size() : e + 2;
        } else {
            out += t[i++];
        }
    }
    return out;
}

std::vector<std::string> tokenize(const std::string& t) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < t.size();) {
        const char c = t[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (ident_char(c)) {
            const std::size_t s = i;
            while (i < t.size() && ident_char(t[i])) ++i;
            out.push_back(t.substr(s, i - s));
        } else if (c == '"') {
            const std::size_t s = i++;
            while (i < t.size() && t[i] != '"') {
                if (t[i] == '\\' && i + 1 < t.size()) ++i;
                ++i;
            }
            if (i < t.size()) ++i;
            out.push_back(t.substr(s, i - s));
        } else if (i + 1 < t.size() && (t.compare(i, 2, "==") == 0 || t.compare(i, 2, "!=") == 0 ||
                                        t.compare(i, 2, "<=") == 0 || t.compare(i, 2, ">=") == 0 ||
                                        t.compare(i, 2, "&&") == 0 || t.compare(i, 2, "||") == 0)) {
            out.push_back(t.substr(i, 2));
            i += 2;
        } else {
            out.push_back(std::string(1, c));
            ++i;
        }
    }
    return out;
}

bool is_number(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

std::size_t close_brace(const std::vector<std::string>& tk, std::size_t open) {
    int d = 0;
    for (std::size_t i = open; i < tk.size(); ++i) {
        if (tk[i] == "{") ++d;
        else if (tk[i] == "}" && --d == 0) return i;
    }
    return tk.size();
}

std::pair<std::size_t, std::size_t> function_body(const std::vector<std::string>& tk, const std::string& name) {
    for (std::size_t i = 1; i + 1 < tk.size(); ++i) {
        if (tk[i] != name || tk[i + 1] != "(") continue;
        const std::string& type = tk[i - 1];
        if (type != "bool" && type != "int" && type != "void") continue;
        std::size_t j = i + 1;
        int d = 0;
        for (; j < tk.size(); ++j) {
            if (tk[j] == "(") ++d;
            else if (tk[j] == ")" && --d == 0) break;
        }
        if (j + 1 < tk.size() && tk[j + 1] == "{") return {j + 1, close_brace(tk, j + 1)};
    }
    return {0, 0};
}

std::string unit_of(const std::string& tok) {
    std::string n = tok;
    const std::size_t us = n.find('_', 2);
    if (us != std::string::npos) n = n.substr(us + 1);
    const std::string alias = "_Alias";
    if (n.size() > alias.size() && n.compare(n.size() - alias.size(), alias.size(), alias) == 0)
        n.resize(n.size() - alias.size());
    return n;
}

void stock_rows(const std::vector<std::string>& tk, std::size_t b, std::size_t e, bool unit_next,
                std::vector<Row>& out) {
    for (std::size_t i = b; i + 6 < e; ++i) {
        if (tk[i] != "AISetStock" && !(unit_next && tk[i] == "AISetStockUnitNext")) continue;
        if (tk[i + 1] != "(" || tk[i + 2] != "player" || tk[i + 3] != "," || !is_number(tk[i + 4]) ||
            tk[i + 5] != "," || !starts_with(tk[i + 6], "c_"))
            continue;
        out.push_back({unit_of(tk[i + 6]), std::atoi(tk[i + 4].c_str())});
    }
}

int deadline_of(const std::vector<std::string>& tk, std::size_t b, std::size_t e) {
    for (std::size_t i = b; i + 4 < e; ++i)
        if (tk[i] == "AIGetTime" && tk[i + 1] == "(" && tk[i + 2] == ")" && tk[i + 3] == "<" && is_number(tk[i + 4]))
            return std::atoi(tk[i + 4].c_str());
    return 0;
}

bool phase_style(const std::string& id, std::string& phase, std::string& style) {
    for (const char* p : {"Open", "Mid", "Late"}) {
        if (!starts_with(id, p)) continue;
        std::size_t i = std::string(p).size();
        const std::size_t s = i;
        while (i < id.size() && std::isalpha(static_cast<unsigned char>(id[i]))) ++i;
        if (i == s || i >= id.size() || id[i] != '_') return false;
        phase = p;
        style = id.substr(s, i - s);
        return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> dispatch(const std::vector<std::string>& tk, const std::string& prefix,
                                                          bool late) {
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t i = 0; i + 8 < tk.size(); ++i) {
        if (tk[i] != "buildIndex" || tk[i + 1] != "==" || !starts_with(tk[i + 2], prefix) || tk[i + 3] != ")" ||
            tk[i + 4] != "{")
            continue;
        std::string fn;
        if (!late && tk[i + 5] == "done" && tk[i + 6] == "=" && tk[i + 8] == "(") fn = tk[i + 7];
        if (late && tk[i + 5] == "return" && tk[i + 7] == "(") fn = tk[i + 6];
        if (!fn.empty()) out.push_back({tk[i + 2].substr(prefix.size()), fn});
    }
    return out;
}

const std::string* lookup(const std::vector<std::pair<std::string, std::string>>& v, const std::string& k) {
    for (const auto& kv : v)
        if (kv.first == k) return &kv.second;
    return nullptr;
}

bool has_word(const std::string& s, const std::string& w) {
    for (std::size_t p = s.find(w); p != std::string::npos; p = s.find(w, p + 1)) {
        const bool l = p == 0 || !ident_char(s[p - 1]);
        const bool r = p + w.size() >= s.size() || !ident_char(s[p + w.size()]);
        if (l && r) return true;
    }
    return false;
}

std::string build_type_in(const std::string& s) {
    const std::string key = "e_buildType_";
    const std::size_t p = s.find(key);
    if (p == std::string::npos) return {};
    std::size_t e = p + key.size();
    while (e < s.size() && ident_char(s[e])) ++e;
    return s.substr(p + key.size(), e - p - key.size());
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string raw_function_body(const std::string& raw, const std::string& name) {
    for (std::size_t p = raw.find(name); p != std::string::npos; p = raw.find(name, p + 1)) {
        if (p > 0 && ident_char(raw[p - 1])) continue;
        const std::size_t ls = raw.rfind('\n', p);
        const std::string head = raw.substr(ls == std::string::npos ? 0 : ls + 1, p - (ls == std::string::npos ? 0 : ls + 1));
        if (head.find("//") != std::string::npos) continue;
        const std::string th = trim(head);
        if (th != "int" && th != "bool" && th != "void") continue;
        std::size_t i = p + name.size();
        while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        if (i >= raw.size() || raw[i] != '(') continue;
        const std::size_t cp = raw.find(')', i);
        if (cp == std::string::npos) continue;
        std::size_t j = cp + 1;
        while (j < raw.size() && std::isspace(static_cast<unsigned char>(raw[j]))) ++j;
        if (j >= raw.size() || raw[j] != '{') continue;
        int d = 0;
        std::size_t k = j;
        for (; k < raw.size(); ++k) {
            if (raw.compare(k, 2, "//") == 0) {
                while (k < raw.size() && raw[k] != '\n') ++k;
                continue;
            }
            if (raw[k] == '{') ++d;
            else if (raw[k] == '}' && --d == 0) break;
        }
        return raw.substr(j + 1, k - j - 1);
    }
    return {};
}

std::vector<Menu> menus_of(const std::string& raw, const std::string& fn, const std::string& prefix) {
    std::vector<Menu> out;
    const std::string body = raw_function_body(raw, fn);
    int d = 1;
    std::size_t ls = 0;
    while (ls <= body.size()) {
        std::size_t le = body.find('\n', ls);
        if (le == std::string::npos) le = body.size();
        const std::string line = body.substr(ls, le - ls);
        ls = le + 1;
        const std::size_t cpos = line.find("//");
        const std::string code = cpos == std::string::npos ? line : line.substr(0, cpos);
        const std::string tc = trim(code);
        const bool lead = !tc.empty() && tc[0] == '}';
        if (d - (lead ? 1 : 0) == 1) {
            std::string style;
            if (has_word(code, "if")) style = build_type_in(code);
            else if (has_word(code, "else")) style = build_type_in(line);
            if (!style.empty()) out.push_back({style, {}});
        }
        for (const char* call : {"AISetSubStateChanceRace", "AISetSubStateChance"}) {
            const std::string c = call;
            for (std::size_t p = code.find(c); p != std::string::npos; p = code.find(c, p + 1)) {
                const std::size_t after = p + c.size();
                if ((p > 0 && ident_char(code[p - 1])) || (after < code.size() && ident_char(code[after]))) continue;
                std::size_t op = after;
                while (op < code.size() && std::isspace(static_cast<unsigned char>(code[op]))) ++op;
                if (op >= code.size() || code[op] != '(') continue;
                const std::size_t cp = code.find(')', op);
                if (cp == std::string::npos) continue;
                std::vector<std::string> args;
                std::string a = code.substr(op + 1, cp - op - 1);
                for (std::size_t s = 0;;) {
                    const std::size_t comma = a.find(',', s);
                    args.push_back(trim(a.substr(s, comma == std::string::npos ? std::string::npos : comma - s)));
                    if (comma == std::string::npos) break;
                    s = comma + 1;
                }
                if (args.size() < 2 || out.empty()) continue;
                Pick pk;
                pk.build = starts_with(args[0], prefix) ? args[0].substr(prefix.size()) : args[0];
                std::vector<int> w;
                for (std::size_t k = 1; k < args.size(); ++k) w.push_back(std::atoi(args[k].c_str()));
                if (w.size() == 1) w.assign(4, w[0]);
                if (w.size() < 4) continue;
                pk.w_random = w[0];
                pk.w_protoss = w[1];
                pk.w_terran = w[2];
                pk.w_zerg = w[3];
                out.back().picks.push_back(pk);
            }
        }
        for (char ch : code) {
            if (ch == '{') ++d;
            else if (ch == '}') --d;
        }
        if (le == body.size()) break;
    }
    return out;
}

std::uint64_t mix(std::uint64_t h, const std::string& s) {
    for (unsigned char c : s) h = (h ^ c) * 0x100000001B3ull;
    return (h ^ 0xFF) * 0x100000001B3ull;
}

std::uint64_t mix(std::uint64_t h, int v) { return mix(h, std::to_string(v)); }

}

const char* race_name(int race_index) {
    switch (race_index) {
        case 0: return "Terran";
        case 1: return "Protoss";
        case 2: return "Zerg";
        default: return "";
    }
}

std::string shared_script_path(int race_index) {
    const std::string r = race_name(race_index);
    return "mods/void.sc2mod/base.sc2data/TriggerLibs/" + r + "/" + r + "High.galaxy";
}

std::string build_script_path(int race_index) {
    const std::string r = race_name(race_index);
    return "mods/void.sc2mod/base.sc2data/TriggerLibs/" + r + "/" + r + "VyHd.galaxy";
}

RaceTables parse_race(int race_index, const std::string& shared_script, const std::string& build_script) {
    RaceTables t;
    const std::string race = race_name(race_index);
    if (race.empty() || shared_script.empty() || build_script.empty()) return t;
    const std::string prefix = std::string("e_hdBuildIndex_") + race[0];
    const std::vector<std::string> high = tokenize(strip_comments(shared_script));
    const std::vector<std::string> diff = tokenize(strip_comments(build_script));
    const auto open_mid = dispatch(diff, prefix, false);
    const auto late = dispatch(high, prefix, true);
    static const char* const kTiers[] = {"Minimum", "Base", "Small", "Medium", "Large", "Full"};
    for (std::size_t i = 0; i + 4 < high.size(); ++i) {
        if (high[i] != "const" || high[i + 1] != "int" || !starts_with(high[i + 2], prefix) || high[i + 3] != "=") continue;
        const std::string id = high[i + 2].substr(prefix.size());
        std::string phase, style;
        if (!phase_style(id, phase, style)) continue;
        Build b;
        b.id = id;
        b.style = style;
        if (phase == "Late") {
            const std::string* fn = lookup(late, id);
            if (!fn) continue;
            const auto body = function_body(high, *fn);
            if (body.second <= body.first) continue;
            for (const char* tier : kTiers) {
                const std::string want = std::string("e_armyType") + tier;
                for (std::size_t k = body.first; k + 6 < body.second; ++k) {
                    if (high[k] != "if" || high[k + 1] != "(" || high[k + 2] != "state" || high[k + 3] != "==" ||
                        high[k + 4] != want || high[k + 5] != ")" || high[k + 6] != "{")
                        continue;
                    stock_rows(high, k + 6, close_brace(high, k + 6), true, b.rows);
                    break;
                }
            }
            t.lates.push_back(std::move(b));
        } else {
            const std::string* fn = lookup(open_mid, id);
            if (!fn) continue;
            const auto body = function_body(diff, *fn);
            if (body.second <= body.first) continue;
            stock_rows(diff, body.first, body.second, false, b.rows);
            b.deadline_s = deadline_of(diff, body.first, body.second);
            (phase == "Open" ? t.openings : t.mids).push_back(std::move(b));
        }
    }
    const std::string L = race;
    t.open_menus = menus_of(shared_script, "OpenChooseHD" + L + "BuildFromType", prefix);
    t.mid_menus = menus_of(shared_script, "MidChooseHD" + L + "BuildFromType", prefix);
    t.late_menus = menus_of(shared_script, "LateChooseHD" + L + "BuildFromType", prefix);
    return t;
}

std::uint64_t digest(const RaceTables& t) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const std::vector<Build>* v : {&t.openings, &t.mids, &t.lates}) {
        h = mix(h, int(v->size()));
        for (const Build& b : *v) {
            h = mix(mix(mix(h, b.id), b.style), b.deadline_s);
            for (const Row& r : b.rows) h = mix(mix(h, r.unit), r.count);
        }
    }
    for (const std::vector<Menu>* v : {&t.open_menus, &t.mid_menus, &t.late_menus}) {
        h = mix(h, int(v->size()));
        for (const Menu& m : *v) {
            h = mix(h, m.style);
            for (const Pick& p : m.picks)
                h = mix(mix(mix(mix(mix(h, p.build), p.w_random), p.w_protoss), p.w_terran), p.w_zerg);
        }
    }
    return h;
}

}
