#include "omc/campaign/flow.hpp"

#include "omc/app/install_ids.hpp"
#include "omc/casc/storage.hpp"
#include "omc/core/xml.hpp"
#include "omc/data/strings.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>

namespace omc::campaign {

namespace {

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Tok {
    char k;
    std::string s;
};

std::vector<Tok> lex(const std::string& t) {
    std::vector<Tok> out;
    std::size_t i = 0;
    const std::size_t n = t.size();
    while (i < n) {
        const char c = t[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (ident_char(c) && !std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t j = i;
            while (j < n && ident_char(t[j])) ++j;
            out.push_back({'i', t.substr(i, j - i)});
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(t[j])) || t[j] == '.')) ++j;
            out.push_back({'n', t.substr(i, j - i)});
            i = j;
        } else if (c == '"') {
            std::string v;
            std::size_t j = i + 1;
            while (j < n && t[j] != '"') {
                if (t[j] == '\\' && j + 1 < n) ++j;
                v.push_back(t[j]);
                ++j;
            }
            out.push_back({'s', v});
            i = j + 1;
        } else {
            static const char* const kTwo[] = {"&&", "||", ">=", "<=", "==", "!="};
            bool two = false;
            for (const char* op : kTwo)
                if (t.compare(i, 2, op) == 0) {
                    out.push_back({'o', op});
                    i += 2;
                    two = true;
                    break;
                }
            if (!two) {
                out.push_back({'o', std::string(1, c)});
                ++i;
            }
        }
    }
    out.push_back({'e', {}});
    return out;
}

struct Expr {
    char k;
    std::string s;
    std::vector<Expr> kids;
};

class Parser {
public:
    explicit Parser(const std::vector<Tok>& toks) : t_(toks) {}

    std::vector<Expr> args() {
        std::vector<Expr> out;
        out.push_back(parse_or());
        while (at(",")) {
            ++p_;
            out.push_back(parse_or());
        }
        return out;
    }

private:
    const std::vector<Tok>& t_;
    std::size_t p_ = 0;

    bool at(const char* op) const { return t_[p_].k == 'o' && t_[p_].s == op; }

    Expr parse_or() {
        Expr e = parse_and();
        if (!at("||")) return e;
        Expr o{'|', {}, {std::move(e)}};
        while (at("||")) {
            ++p_;
            o.kids.push_back(parse_and());
        }
        return o;
    }

    Expr parse_and() {
        Expr e = parse_rel();
        if (!at("&&")) return e;
        Expr a{'&', {}, {std::move(e)}};
        while (at("&&")) {
            ++p_;
            a.kids.push_back(parse_rel());
        }
        return a;
    }

    Expr parse_rel() {
        Expr l = parse_prim();
        static const char* const kRel[] = {">=", "<=", "==", "!=", ">", "<"};
        for (const char* r : kRel)
            if (at(r)) {
                ++p_;
                Expr rhs = parse_prim();
                return Expr{'r', r, {std::move(l), std::move(rhs)}};
            }
        return l;
    }

    Expr parse_prim() {
        const Tok& k = t_[p_];
        if (k.k == 'o' && k.s == "(") {
            ++p_;
            Expr e = parse_or();
            if (at(")")) ++p_;
            return e;
        }
        if (k.k == 'o' && k.s == "!") {
            ++p_;
            Expr e = parse_prim();
            return Expr{'!', {}, {std::move(e)}};
        }
        if (k.k == 'i') {
            ++p_;
            if (k.s == "true" || k.s == "false") return Expr{'l', k.s, {}};
            if (at("(")) {
                ++p_;
                Expr c{'c', k.s, {}};
                if (!at(")")) {
                    c.kids.push_back(parse_or());
                    while (at(",")) {
                        ++p_;
                        c.kids.push_back(parse_or());
                    }
                }
                if (at(")")) ++p_;
                return c;
            }
            return Expr{'v', k.s, {}};
        }
        if (k.k == 'n') {
            ++p_;
            return Expr{'n', k.s, {}};
        }
        if (k.k == 's') {
            ++p_;
            return Expr{'s', k.s, {}};
        }
        if (k.k != 'e') ++p_;
        return Expr{'?', k.s, {}};
    }
};

std::size_t match_close(const std::string& t, std::size_t open, char o, char c) {
    int depth = 0;
    for (std::size_t i = open; i < t.size(); ++i) {
        const char ch = t[i];
        if (ch == '"') {
            for (++i; i < t.size() && t[i] != '"'; ++i)
                if (t[i] == '\\') ++i;
        } else if (ch == o) {
            ++depth;
        } else if (ch == c) {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

std::string script_function(const std::string& ident) {
    const std::size_t at = ident.rfind("_gf_");
    return at == std::string::npos ? ident : ident.substr(at + 4);
}

bool is_call(const Expr& e, const char* name) { return e.k == 'c' && script_function(e.s) == name; }

bool completed_status(const Expr& e) { return e.k == 'v' && ends_with(e.s, "_MissionStatus_Completed"); }

int32_t number(const Expr& e) {
    try {
        return static_cast<int32_t>(std::stol(e.s));
    } catch (const std::exception&) {
        return 0;
    }
}

std::string describe(const Expr& e) {
    if (e.k == 'c') {
        const std::string f = script_function(e.s);
        if (f != "MissionStatusCheck" && f != "CountMissions" && f != "NumberOfMissionArcsCompleted") return f;
    }
    for (const Expr& k : e.kids) {
        std::string d = describe(k);
        if (!d.empty()) return d;
    }
    return {};
}

struct Terms {
    std::vector<MissionId> all;
    std::vector<MissionId> any;
    int32_t gate = 0;
    int32_t arc = 0;
    bool always = false;
    std::vector<std::string> dropped;
};

void merge(Terms& into, const Terms& from) {
    into.all.insert(into.all.end(), from.all.begin(), from.all.end());
    into.any.insert(into.any.end(), from.any.begin(), from.any.end());
    into.gate = std::max(into.gate, from.gate);
    into.arc = std::max(into.arc, from.arc);
    into.always = into.always || from.always;
    into.dropped.insert(into.dropped.end(), from.dropped.begin(), from.dropped.end());
}

class Scripts {
public:
    explicit Scripts(const std::vector<std::string>& texts) : texts_(texts) {}

    const std::vector<std::string>& texts() const { return texts_; }

    MissionId mission(const Expr& e) {
        if (e.k == 's') return e.s;
        if (e.k != 'v' || e.s.find("_ge_") == std::string::npos) return {};
        auto it = alias_.find(e.s);
        if (it != alias_.end()) return it->second;
        std::set<std::string> found;
        const std::string needle = "_InitializeMapDataSingle(" + e.s + ", \"";
        for (const std::string& t : texts_) {
            std::size_t pos = 0;
            while ((pos = t.find(needle, pos)) != std::string::npos) {
                const std::size_t b = pos + needle.size();
                const std::size_t q = t.find('"', b);
                if (q == std::string::npos) break;
                found.insert(t.substr(b, q - b));
                pos = q;
            }
        }
        MissionId id = found.size() == 1 ? *found.begin() : MissionId{};
        alias_[e.s] = id;
        return id;
    }

private:
    const std::vector<std::string>& texts_;
    std::map<std::string, MissionId> alias_;
};

bool atom(const Expr& e, Scripts& sc, Terms& t) {
    if (e.k == 'l') {
        if (e.s != "true") return false;
        t.always = true;
        return true;
    }
    if (is_call(e, "MissionStatusCheck") && e.kids.size() == 2 && completed_status(e.kids[1])) {
        MissionId m = sc.mission(e.kids[0]);
        if (m.empty()) return false;
        t.all.push_back(std::move(m));
        return true;
    }
    if (e.k == 'r' && e.s == ">=" && e.kids.size() == 2 && e.kids[1].k == 'n') {
        const Expr& l = e.kids[0];
        if (is_call(l, "CountMissions") && l.kids.size() == 1 && completed_status(l.kids[0])) {
            t.gate = std::max(t.gate, number(e.kids[1]));
            return true;
        }
        if (is_call(l, "NumberOfMissionArcsCompleted") && l.kids.empty()) {
            t.arc = std::max(t.arc, number(e.kids[1]));
            return true;
        }
    }
    return false;
}

bool single_prereq(const Terms& t) {
    return t.all.size() == 1 && t.any.empty() && !t.always && t.gate == 0 && t.arc == 0 && t.dropped.empty();
}

std::string dropped_name(const Expr& e) {
    std::string d = describe(e);
    return d.empty() ? std::string("condition") : d;
}

bool reduce(const Expr& e, Scripts& sc, Terms& t) {
    if (e.k == '&') {
        bool any = false;
        for (const Expr& kid : e.kids) {
            Terms k;
            if (reduce(kid, sc, k)) {
                merge(t, k);
                any = true;
            } else {
                t.dropped.push_back(dropped_name(kid));
            }
        }
        return any;
    }
    if (e.k == '|') {
        std::vector<MissionId> ids;
        bool plain = true;
        for (const Expr& kid : e.kids) {
            Terms k;
            if (!atom(kid, sc, k) || !single_prereq(k)) {
                plain = false;
                break;
            }
            ids.push_back(k.all.front());
        }
        if (plain) {
            t.any.insert(t.any.end(), ids.begin(), ids.end());
            return true;
        }
        std::vector<Terms> kept;
        std::vector<std::string> dropped;
        for (const Expr& kid : e.kids) {
            Terms k;
            if (reduce(kid, sc, k) && k.dropped.empty())
                kept.push_back(std::move(k));
            else
                dropped.push_back(dropped_name(kid));
        }
        if (kept.size() != 1) return false;
        merge(t, kept.front());
        t.dropped.insert(t.dropped.end(), dropped.begin(), dropped.end());
        return true;
    }
    return atom(e, sc, t);
}

std::vector<Expr> call_args(const std::string& text, std::size_t open, std::size_t* close_out) {
    const std::size_t close = match_close(text, open, '(', ')');
    if (close_out) *close_out = close;
    if (close == std::string::npos) return {};
    const std::vector<Tok> toks = lex(text.substr(open + 1, close - open - 1));
    Parser p(toks);
    return p.args();
}

std::vector<MissionId> arc_terminals(Scripts& sc) {
    const std::string name = "_gf_NumberOfMissionArcsCompleted";
    const std::string check = "_gf_MissionStatusCheck(";
    for (const std::string& t : sc.texts()) {
        std::size_t pos = 0;
        while ((pos = t.find(name, pos)) != std::string::npos) {
            std::size_t i = pos + name.size();
            pos = i;
            auto skip = [&] {
                while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
            };
            skip();
            if (i >= t.size() || t[i] != '(') continue;
            ++i;
            skip();
            if (i >= t.size() || t[i] != ')') continue;
            ++i;
            skip();
            if (i >= t.size() || t[i] != '{') continue;
            const std::size_t end = match_close(t, i, '{', '}');
            if (end == std::string::npos) continue;
            const std::string body = t.substr(i, end - i);
            std::vector<MissionId> out;
            std::size_t c = 0;
            while ((c = body.find(check, c)) != std::string::npos) {
                const std::size_t open = c + check.size() - 1;
                std::size_t close = 0;
                std::vector<Expr> a = call_args(body, open, &close);
                c = open + 1;
                if (a.size() != 2 || !completed_status(a[1])) continue;
                MissionId m = sc.mission(a[0]);
                if (!m.empty()) out.push_back(std::move(m));
            }
            return out;
        }
    }
    return {};
}

}

CampaignFlow derive_flow(const std::string& name, const std::vector<std::string>& scripts) {
    CampaignFlow flow;
    flow.name = name;
    Scripts sc(scripts);
    const std::string rule = "_LockUnlockMissionIfCondition(";
    std::vector<std::pair<MissionId, MissionId>> locks;
    for (const std::string& text : scripts) {
        std::size_t pos = 0;
        while ((pos = text.find(rule, pos)) != std::string::npos) {
            const std::size_t open = pos + rule.size() - 1;
            std::size_t close = 0;
            std::vector<Expr> a = call_args(text, open, &close);
            pos = open + 1;
            if (close == std::string::npos) break;
            if (a.size() != 3 || a[0].k != 'l') continue;
            MissionId m = sc.mission(a[1]);
            if (m.empty()) continue;
            if (a[0].s == "true") {
                Terms t;
                if (atom(a[2], sc, t) && single_prereq(t)) locks.emplace_back(m, t.all.front());
                continue;
            }
            if (flow.index.count(m)) continue;
            Terms t;
            if (!reduce(a[2], sc, t)) continue;
            if (!t.all.empty() && !t.any.empty()) continue;
            MissionNode n;
            n.id = m;
            n.always = t.always;
            if (!t.any.empty()) {
                n.prereqs = t.any;
                n.prereqs_any = true;
            } else {
                n.prereqs = t.all;
            }
            n.gate_count = t.gate;
            n.arc_gate = t.arc;
            n.unmodelled = t.dropped;
            flow.index[n.id] = flow.nodes.size();
            flow.nodes.push_back(std::move(n));
        }
    }
    if (flow.nodes.empty()) return flow;
    flow.arc_terminals = arc_terminals(sc);
    for (const auto& [a, b] : locks) {
        std::vector<MissionId>* group = nullptr;
        for (auto& g : flow.branches)
            if (std::find(g.begin(), g.end(), a) != g.end() || std::find(g.begin(), g.end(), b) != g.end()) {
                group = &g;
                break;
            }
        if (!group) {
            flow.branches.emplace_back();
            group = &flow.branches.back();
        }
        for (const MissionId& m : {a, b})
            if (std::find(group->begin(), group->end(), m) == group->end()) group->push_back(m);
    }
    return flow;
}

std::vector<CampaignFlow> load_campaign_flow(const casc::Storage& store) {
    std::vector<CampaignFlow> out;
    if (!store.has_mndx()) return out;
    const std::string ext = lower(app::install_ids::kCampaignExtension);
    const std::string data = std::string("/") + app::install_ids::kDataPackage + "/";
    const std::string list = "TriggerLibs/LibraryList.xml";
    const std::string list_tail = lower(data + list);
    std::set<std::string> packages;
    std::map<std::string, std::string> roots;
    for (const auto& entry : store.mndx().enumerate()) {
        const std::string l = lower(entry.first);
        const std::size_t e = l.find(ext + "/");
        if (e == std::string::npos) continue;
        const std::size_t slash = l.rfind('/', e);
        const std::size_t s = slash == std::string::npos ? 0 : slash + 1;
        const std::string stem = l.substr(s, e - s);
        packages.insert(stem);
        if (l.compare(e + ext.size(), std::string::npos, list_tail) == 0)
            roots[stem] = entry.first.substr(0, e + ext.size());
    }
    for (const auto& [stem, root] : roots) {
        std::vector<std::string> scripts;
        try {
            const std::vector<std::uint8_t> bytes = store.read_by_name(root + data + list);
            const core::XmlNode doc = core::xml_parse(bytes);
            for (const core::XmlNode* lib : doc.elements("library")) {
                const std::string* ext_name = lib->attr("external");
                if (!ext_name) continue;
                try {
                    const std::vector<std::uint8_t> b = store.read_by_name(root + data + *ext_name + ".galaxy");
                    scripts.emplace_back(b.begin(), b.end());
                } catch (const std::exception&) {
                }
            }
        } catch (const std::exception&) {
            continue;
        }
        CampaignFlow f = derive_flow(stem, scripts);
        if (f.nodes.empty()) continue;
        for (const std::string& p : packages)
            if (p != stem && stem.compare(0, p.size(), p) == 0 && p.size() > f.title_stem.size()) f.title_stem = p;
        out.push_back(std::move(f));
    }
    return out;
}

std::string campaign_title(const std::vector<CampaignFlow>& flows, std::size_t i, const data::StringTable& text) {
    if (i >= flows.size()) return {};
    const std::string stem = lower(flows[i].title_stem);
    if (!stem.empty()) {
        const std::string* hit = nullptr;
        int matches = 0;
        for (const std::string& key : text.keys_with_prefix("UI/ExpansionLevel/"))
            if (ends_with(lower(key), stem)) {
                hit = text.find(key);
                ++matches;
            }
        if (matches == 1 && hit && !hit->empty()) return *hit;
    }
    return "Campaign " + std::to_string(i + 1);
}

const MissionNode* CampaignFlow::find(const MissionId& id) const {
    auto it = index.find(id);
    if (it == index.end()) return nullptr;
    return &nodes[it->second];
}

std::size_t CampaignFlow::count_completed(const CompletedSet& completed) const {
    std::size_t n = 0;
    for (const MissionId& m : completed)
        if (index.find(m) != index.end()) ++n;
    return n;
}

int32_t CampaignFlow::arcs_completed(const CompletedSet& completed) const {
    int32_t n = 0;
    for (const MissionId& t : arc_terminals)
        if (completed.count(t)) ++n;
    return n;
}

bool CampaignFlow::locked_by_branch_sibling(const MissionId& id,
                                            const CompletedSet& completed) const {
    for (const auto& group : branches) {
        bool in_group = false;
        for (const MissionId& m : group)
            if (m == id) { in_group = true; break; }
        if (!in_group) continue;
        for (const MissionId& m : group)
            if (m != id && completed.count(m)) return true;
    }
    return false;
}

bool CampaignFlow::available(const MissionId& id,
                             const CompletedSet& completed) const {
    const MissionNode* n = find(id);
    if (!n) return false;

    if (!n->prereqs.empty()) {
        if (n->prereqs_any) {
            bool any = false;
            for (const MissionId& r : n->prereqs)
                if (completed.count(r)) { any = true; break; }
            if (!any) return false;
        } else {
            for (const MissionId& r : n->prereqs)
                if (!completed.count(r)) return false;
        }
    }

    if (n->gate_count > 0 &&
        static_cast<int32_t>(count_completed(completed)) < n->gate_count)
        return false;

    if (n->arc_gate > 0 && arcs_completed(completed) < n->arc_gate) return false;

    if (locked_by_branch_sibling(id, completed)) return false;

    return true;
}

std::vector<MissionId> CampaignFlow::available_missions(
    const CompletedSet& completed) const {
    std::vector<MissionId> out;
    for (const MissionNode& n : nodes)
        if (!completed.count(n.id) && available(n.id, completed))
            out.push_back(n.id);
    return out;
}

}
