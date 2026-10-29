#include "omc/render/actor_rules.hpp"

#include "omc/data/actor_catalog.hpp"

namespace omc::render {

namespace {

std::vector<std::string> split_props(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            size_t a = start, b = i;
            while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
            if (b > a) out.push_back(s.substr(a, b - a));
            start = i + 1;
        }
    }
    return out;
}

std::string anim_props_of(const data::ActorRule& r) {
    if (r.args.size() >= 2) return r.args[1];
    if (r.args.size() == 1) return r.args[0];
    return {};
}

bool is_number(const std::string& s) {
    if (s.empty()) return false;
    bool digit = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c >= '0' && c <= '9') { digit = true; continue; }
        if (c == '.' || ((c == '-' || c == '+') && i == 0)) continue;
        return false;
    }
    return digit;
}

}

bool anim_arg_is_flags(const std::string& arg) {
    static const char* kFlags[] = {
        "FullMatch", "PlayForever",       "NonLooping",        "Looping",
        "Instant",   "AsDuration",        "AsTimeScale",       "ClosingFull",
        "OpeningPlayForever", "ContentPlayOnce", "DontResetOnUnhide", "RandomStartOffset",
    };
    const std::vector<std::string> parts = split_props(arg);
    if (parts.empty()) return false;
    for (const std::string& p : parts) {
        bool known = false;
        for (const char* f : kFlags)
            if (p == f) { known = true; break; }
        if (!known) return false;
    }
    return true;
}

AnimArgs anim_args_of(const std::vector<std::string>& args) {
    AnimArgs out;
    if (args.size() < 2) return out;
    std::size_t end = args.size();
    bool non_looping = false, looping = false;
    while (end > 1) {
        const std::string& a = args[end - 1];
        if (!anim_arg_is_flags(a) && !is_number(a)) break;
        if (anim_arg_is_flags(a))
            for (const std::string& p : split_props(a)) {
                if (p == "NonLooping") non_looping = true;
                if (p == "Looping" || p == "PlayForever") looping = true;
            }
        --end;
    }
    out.looping = non_looping ? false : (looping ? true : true);
    for (std::size_t i = 1; i < end; ++i) out.props.push_back(args[i]);
    return out;
}

ActorRules ActorRules::from_actor(const data::CActorUnit& a) {
    ActorRules out;
    out.valid = true;
    out.walk_anim_move_speed = a.walk_anim_move_speed.value_or(0.0);

    for (const data::ActorRule& r : a.on) {
        if (r.event_head == "UnitBirth" && r.verb == "AnimPlay") {
            std::vector<std::string> t = split_props(anim_props_of(r));
            if (!t.empty()) { out.by_state[int(AnimState::Birth)] = std::move(t); break; }
        }
    }

    auto slot_by_name = [&out](const std::string& n) -> ActorRules::Slot& {
        for (ActorRules::Slot& s : out.slots)
            if (s.name == n) return s;
        out.slots.push_back(ActorRules::Slot{n, {}, true, {}, {}});
        return out.slots.back();
    };
    for (const data::ActorRule& r : a.on) {
        if (r.verb == "AnimPlay") {
            if (r.args.empty() || r.event_head == "UnitBirth") continue;
            ActorRules::Slot& s = slot_by_name(r.args[0]);
            if (s.props.empty()) {
                const AnimArgs a2 = anim_args_of(r.args);
                s.props = a2.props;
                s.looping = a2.looping;
                s.open_event = r.event_head;
            }
        } else if (r.verb == "AnimClear") {
            if (r.args.empty()) continue;
            ActorRules::Slot& s = slot_by_name(r.args[0]);
            bool dup = false;
            for (const std::string& e : s.close_events)
                if (e == r.event_head) dup = true;
            if (!dup) s.close_events.push_back(r.event_head);
        }
    }

    const data::ActorRule* atk = nullptr;
    for (const data::ActorRule& r : a.on) {
        if (r.event_head == "WeaponStart" && r.verb == "AnimBracketStart") {
            if (!atk) atk = &r;
            if (r.filters.empty()) { atk = &r; break; }
        }
    }
    if (atk) {
        std::vector<std::string> t = split_props(anim_props_of(*atk));
        if (!t.empty()) out.by_state[int(AnimState::Attack)] = std::move(t);
    }

    for (const data::ActorDeath& d : a.death_array) {
        if (d.index == "Normal") {
            std::vector<std::string> t = split_props(d.anim_props);
            if (!t.empty()) out.by_state[int(AnimState::Death)] = std::move(t);
            break;
        }
    }

    for (const std::pair<std::string, std::string>& b : a.baselines) {
        const std::string& idx = b.first;
        std::vector<std::string> t = split_props(b.second);
        if (t.empty()) t = {idx};
        if (idx == "Stand") out.by_state[int(AnimState::Stand)] = std::move(t);
        else if (idx == "Walk") out.by_state[int(AnimState::Walk)] = std::move(t);
    }

    static const char* const kDefault[5] = {"Birth", "Stand", "Walk", "Attack", "Death"};
    for (int i = 0; i < 5; ++i)
        if (out.by_state[i].empty()) out.by_state[i] = {kDefault[i]};

    return out;
}

}
