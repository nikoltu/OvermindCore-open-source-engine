#include <algorithm>
#include <cstdlib>
#include "sc2/render/actor.hpp"

#include <cmath>

#include "sc2/render/actor_rules.hpp"
#include "sc2/render/anim_select.hpp"
#include "sc2/render/audio_sink.hpp"
#include "sc2/render/frame_prof.hpp"

namespace sc2::render {

namespace {
const std::vector<std::string>& tokens_for(AnimState s) {
    static const std::vector<std::string> birth{"Birth"}, stand{"Stand"}, walk{"Walk"},
        attack{"Attack"}, death{"Death"},
        gather{"Work", "Stand Gather", "Gather"},
        cast{"Spell", "Spell Channel", "Channel"},
        fly_start{"Fly", "Start"}, fly_stand{"Fly", "Stand"},
        fly_walk{"Fly", "Walk"}, fly_end{"Fly", "End"};
    switch (s) {
        case AnimState::Birth: return birth;
        case AnimState::Walk: return walk;
        case AnimState::Attack: return attack;
        case AnimState::Death: return death;
        case AnimState::Gather: return gather;
        case AnimState::Cast: return cast;
        case AnimState::FlyStart: return fly_start;
        case AnimState::FlyStand: return fly_stand;
        case AnimState::FlyWalk: return fly_walk;
        case AnimState::FlyEnd: return fly_end;
        case AnimState::Stand: default: return stand;
    }
}
float clip_len(const formats::Model& m, int seq) {
    if (seq < 0 || seq >= int(m.sequences.size())) return 0;
    float l = float(m.sequences[seq].length_ms());
    return l > 1 ? l : 1000.0f;
}
}

const std::vector<std::string>& ActorInstance::tokens_of(AnimState s) const {
    if (rules && rules->valid && !rules->tokens(s).empty()) return rules->tokens(s);
    return tokens_for(s);
}

int ActorInstance::sequence_of(AnimState s) {
    int i = int(s);
    if (cached_seq[i] == -2) {
        int r = -1;
        if (model) {
            const std::vector<std::string>& base = tokens_of(s);
            if (s == AnimState::Attack)
                r = select_sequence(*model, swing_tokens(base, swing), seed);
            if (r < 0 && !anim_groups.empty()) {
                std::vector<std::string> grouped = base;
                grouped.insert(grouped.end(), anim_groups.begin(), anim_groups.end());
                r = select_sequence(*model, grouped, seed);
            }
            if (r < 0) r = select_sequence(*model, base, seed);
            if (r < 0 && (s == AnimState::FlyStart || s == AnimState::FlyWalk ||
                          s == AnimState::FlyEnd))
                r = select_sequence(*model, tokens_of(AnimState::FlyStand), seed);
        }
        if (r < 0 && model)
            r = select_sequence(*model, tokens_of(AnimState::Stand), seed);
        if (r < 0 && model && !model->sequences.empty()) r = 0;
        if (r >= 0 && model && idle_loops > 0 && (s == AnimState::Stand || s == AnimState::FlyStand)) r = select_take(*model, r, seed, idle_loops);
        if (r >= 0 && model && walk_loops > 0 && (s == AnimState::Walk || s == AnimState::FlyWalk)) r = select_take(*model, r, seed, walk_loops);
        static const bool kOneshotTakes = std::getenv("SC2_NO_ONESHOT_TAKES") == nullptr;
        if (kOneshotTakes && r >= 0 && model && oneshot_plays > 0 && (s == AnimState::Attack || s == AnimState::Death || s == AnimState::Cast))
            r = select_take(*model, r, seed, oneshot_plays);
        cached_seq[i] = r;
    }
    return cached_seq[i];
}

namespace {

bool trigger_field_matches(const std::string& authored, const std::string& actual) {
    return authored.empty() || authored == "*" || authored == actual;
}

bool filters_match(const ActorEventRule& r, const ActorEvent& ev, std::size_t* unanswerable,
                   const std::map<std::string, int>* status) {
    for (const auto& f : r.filters) {
        const bool negated = !f.first.empty() && f.first[0] == '!';
        const std::string fkey = negated ? f.first.substr(1) : f.first;
        if (status && (fkey == "HasStatus" || fkey == "IsStatus")) {
            std::string name = f.second;
            int want = 0;
            bool truth;
            if (fkey == "IsStatus") {
                const std::size_t sp = f.second.find(' ');
                name = f.second.substr(0, sp);
                want = sp == std::string::npos ? 0 : std::atoi(f.second.c_str() + sp + 1);
            }
            const auto it = status->find(name);
            const int have = it == status->end() ? 0 : it->second;
            truth = fkey == "HasStatus" ? have > 0 : have == want;
            if (truth == negated) return false;
            continue;
        }
        if (!negated) {
            bool ok = false;
            for (const auto& c : ev.ctx)
                if (c.first == f.first && c.second == f.second) { ok = true; break; }
            if (!ok) return false;
            continue;
        }
        const std::string key = f.first.substr(1);
        bool carried = false, same = false;
        for (const auto& c : ev.ctx)
            if (c.first == key) {
                carried = true;
                same = c.second == f.second;
                break;
            }
        if (!carried) {
            if (unanswerable) ++*unanswerable;
            return false;
        }
        if (same) return false;
    }
    return true;
}

float timer_delay_ms(const std::string& arg) {
    const std::size_t comma = arg.find(',');
    const std::string hi = comma == std::string::npos ? arg : arg.substr(comma + 1);
    return float(std::atof(hi.c_str()) * 1000.0);
}

}

void ActorInstance::bind_event_rules(std::vector<ActorEventRule> rules) {
    event_rules = std::move(rules);
    std::vector<int> ladder;
    for (const ActorEventRule& r : event_rules)
        for (const auto& f : r.filters) {
            const std::string k = (!f.first.empty() && f.first[0] == '!') ? f.first.substr(1) : f.first;
            if (k != "ModelVariation") continue;
            ladder.push_back(std::atoi(f.second.c_str()));
        }
    std::sort(ladder.begin(), ladder.end());
    ladder.erase(std::unique(ladder.begin(), ladder.end()), ladder.end());
    model_variation = ladder.empty() ? -1 : ladder[seed % ladder.size()];
    if (model_variation >= 0) frame_prof_count("modelvar.bound", 1.0);
}

int ActorInstance::apply_birth_groups(const std::string& unit_id) {
    if (unit_id.empty() || std::getenv("SC2_NO_BIRTH_GROUPS")) return 0;
    int n = 0;
    static const bool kBirthForever = std::getenv("SC2_NO_BIRTH_FOREVER_CLIPS") == nullptr;
    if (const char* bt = std::getenv("SC2_BIRTH_TRACE"); bt && unit_id == bt) {
        std::fprintf(stderr, "[birth] %s: %zu rules bound\n", unit_id.c_str(), event_rules.size());
        for (const ActorEventRule& r : event_rules)
            if (r.head.find("Birth") != std::string::npos || r.verb == "AnimPlay") {
                std::string al;
                for (const std::string& a : r.args) al += " [" + a + "]";
                std::fprintf(stderr, "[birth]   head '%s' subject '%s' sub '%s' filters %zu verb %s%s\n", r.head.c_str(), r.subject.c_str(), r.sub.c_str(), r.filters.size(), r.verb.c_str(), al.c_str());
            }
    }
    if (kBirthForever)
        for (const ActorEventRule& r : event_rules) {
            if (r.head != "UnitBirth" || !(r.subject.empty() || r.subject == unit_id) || !r.sub.empty() || !r.filters.empty()) continue;
            if (r.verb != "AnimPlay" || r.args.size() < 3 || r.args[0].empty()) continue;
            if (std::find(r.args.begin() + 2, r.args.end(), "PlayForever") == r.args.end()) continue;
            if (std::find(birth_clips.begin(), birth_clips.end(), r.args[0]) != birth_clips.end()) continue;
            birth_clips.push_back(r.args[0]);
            const AnimArgs a2 = anim_args_of(r.args);
            anim_play(r.args[0], a2.props, true,   1);
            ++n;
        }
    for (const ActorEventRule& r : event_rules) {
        if (r.head != "UnitBirth" || r.subject != unit_id || !r.sub.empty() || !r.filters.empty()) continue;
        if (r.verb != "AnimGroupApply" || r.args.empty() || r.args[0].empty()) continue;
        const bool bare = r.args.size() == 1 || (r.args.size() >= 3 && r.args[1] == "{}" && r.args[2] == "{}");
        if (!bare || std::find(anim_groups.begin(), anim_groups.end(), r.args[0]) != anim_groups.end()) continue;
        anim_groups.push_back(r.args[0]);
        for (int& c : cached_seq) c = -2;
        ++n;
    }
    return n;
}

void ActorInstance::raise(const std::string& head, const std::string& subject,
                          const std::string& sub,
                          std::vector<std::pair<std::string, std::string>> ctx) {
    if (event_rules.empty()) return;
    if (model_variation >= 0) {
        bool carried = false;
        for (const auto& c : ctx)
            if (c.first == "ModelVariation") { carried = true; break; }
        if (!carried) ctx.emplace_back("ModelVariation", std::to_string(model_variation));
        frame_prof_count("modelvar.answered", 1.0);
    }
    ActorEvent ev;
    ev.head = head;
    ev.subject = subject;
    ev.sub = sub;
    ev.ctx = std::move(ctx);
    queued.push_back(std::move(ev));
}

bool ActorInstance::has_child(const std::string& name) const {
    for (const ActorChild& c : children)
        if (c.name == name) return true;
    return false;
}

bool ActorInstance::timer_armed(const std::string& name) const {
    for (const ActorTimer& t : timers)
        if (t.name == name) return true;
    return false;
}

void ActorInstance::pump(float dt_ms) {
    if (event_rules.empty()) return;

    for (std::size_t i = 0; i < timers.size();) {
        timers[i].left_ms -= dt_ms;
        if (timers[i].left_ms <= 0.0f) {
            const std::string name = timers[i].name;
            timers.erase(timers.begin() + long(i));
            raise("TimerExpired", std::string(), std::string(), {{"TimerName", name}});
        } else {
            ++i;
        }
    }

    constexpr std::size_t kMaxDispatchPerTick = 64;
    std::size_t done = 0;
    for (std::size_t qi = 0; qi < queued.size(); ++qi) {
        if (done >= kMaxDispatchPerTick) {
            bus_overflow += queued.size() - qi;
            break;
        }
        const ActorEvent ev = queued[qi];
        for (const ActorEventRule& r : event_rules) {
            if (!trigger_field_matches(r.head, ev.head)) continue;
            if (!trigger_field_matches(r.subject, ev.subject)) continue;
            if (!trigger_field_matches(r.sub, ev.sub)) continue;
            if (!filters_match(r, ev, &unanswerable_filters, &status)) continue;
            static const bool kTargetOnHost = std::getenv("SC2_ACTOR_TARGET_ON_HOST") != nullptr;
            if (!kTargetOnHost && !r.target.empty() && r.target != "::Host" && r.target != "::Self" && r.target != "::Main") {
                for (std::size_t ci = 0; ci < children.size(); ++ci) {
                    if (children[ci].name != r.target) continue;
                    if (r.verb == "Destroy" && r.args.empty()) {
                        children.erase(children.begin() + long(ci));
                    } else if (r.verb == "AnimPlay" && r.args.size() >= 2 && children[ci].rig) {
                        const AnimArgs ta = anim_args_of(r.args);
                        children[ci].rig->anim_play(r.args[0], ta.props, ta.looping, 2);
                    }
                    break;
                }
                ++done;
                continue;
            }
            ++done;
            ++rules_fired;
            if (std::getenv("SC2_BUS_TRACE")) {
                std::fprintf(stderr, "[bus] %s.%s.%s -> %s", ev.head.c_str(), ev.subject.c_str(), ev.sub.c_str(), r.verb.c_str());
                for (const std::string& a : r.args) std::fprintf(stderr, " %s", a.c_str());
                std::fprintf(stderr, " | slots before:");
                for (const AnimSlot& sl : slots) std::fprintf(stderr, " %s(seq %d%s)", sl.name.c_str(), sl.sequence, sl.closing_phase ? " closing" : "");
                std::fprintf(stderr, "\n");
            }
            if (r.verb == "TimerSet" && r.args.size() >= 2) {
                const std::string& name = r.args[1];
                for (std::size_t t = 0; t < timers.size(); ++t)
                    if (timers[t].name == name) { timers.erase(timers.begin() + long(t)); break; }
                ActorTimer nt;
                nt.name = name;
                nt.left_ms = timer_delay_ms(r.args[0]);
                timers.push_back(nt);
            } else if (r.verb == "TimerKill" && !r.args.empty()) {
                for (std::size_t t = 0; t < timers.size(); ++t)
                    if (timers[t].name == r.args[0]) {
                        timers.erase(timers.begin() + long(t));
                        break;
                    }
            } else if (r.verb == "AnimPlay" && r.args.size() >= 2) {
                const AnimArgs a2 = anim_args_of(r.args);
                anim_play(r.args[0], a2.props, a2.looping,   1);
            } else if (r.verb == "AnimClear" && !r.args.empty()) {
                anim_clear(r.args[0]);
            } else if (r.verb == "AnimGroupApply" || r.verb == "AnimBracketStart") {
                static const bool kBracesEmpty = std::getenv("SC2_ANIM_GROUP_BRACES_AS_CLIP") == nullptr;
                const bool bare_group = kBracesEmpty && r.verb == "AnimGroupApply" && r.args.size() >= 3 && !r.args[0].empty() &&
                                        r.args[1] == "{}" && r.args[2] == "{}";
                if (r.args.size() < 2 || bare_group) {
                    if (r.verb == "AnimGroupApply" && (r.args.size() == 1 || bare_group) && !r.args[0].empty()) {
                        if (std::find(anim_groups.begin(), anim_groups.end(), r.args[0]) == anim_groups.end()) {
                            anim_groups.push_back(r.args[0]);
                            for (int& c : cached_seq) c = -2;
                        }
                        continue;
                    }
                    ++unhandled_verbs;
                    continue;
                }
                auto split = [](const std::string& in) {
                    std::vector<std::string> out;
                    std::string tok;
                    for (char c : in) {
                        if (c == ',') {
                            if (!tok.empty()) out.push_back(tok);
                            tok.clear();
                        } else {
                            tok.push_back(c);
                        }
                    }
                    if (!tok.empty()) out.push_back(tok);
                    return out;
                };
                const std::string slot_name = r.args[0];
                const std::vector<std::string> opening = split(r.args[1]);
                std::vector<std::string> closing;
                if (r.verb == "AnimGroupApply") {
                    if (r.args.size() >= 3) closing = split(r.args[2]);
                } else if (r.args.size() >= 4) {
                    closing = split(r.args[3]);
                }
                std::vector<std::string> content;
                if (r.verb == "AnimBracketStart" && r.args.size() >= 3 && r.args[2] != "IGNORE")
                    content = split(r.args[2]);
                anim_play(slot_name, opening,   false,   1);
                for (AnimSlot& sl : slots)
                    if (sl.name == slot_name) {
                        sl.group = slot_name;
                        sl.closing = closing;
                        sl.content = content;
                        if (sl.global && sl.sequence < 0 && model && !content.empty()) {
                            const int cseq = select_sequence(*model, content, seed);
                            if (cseq >= 0) {
                                sl.props = content;
                                sl.sequence = cseq;
                                sl.looping = true;
                                sl.content.clear();
                            }
                        }
                    }
            } else if ((r.verb == "AnimGroupRemove" || r.verb == "AnimBracketStop") &&
                       !r.args.empty()) {
                const std::string tgt = r.args[0];
                static const bool kRemoveClosing = std::getenv("SC2_ANIM_REMOVE_CLOSING") != nullptr;
                if (!kRemoveClosing) {
                    bool any = false, all_closing = true;
                    for (const AnimSlot& sl : slots)
                        if (sl.name == tgt || sl.group == tgt) {
                            any = true;
                            if (!sl.closing_phase) all_closing = false;
                        }
                    if (any && all_closing) continue;
                }
                if (r.verb == "AnimGroupRemove")
                    if (auto git = std::find(anim_groups.begin(), anim_groups.end(), tgt); git != anim_groups.end()) {
                        anim_groups.erase(git);
                        for (int& c : cached_seq) c = -2;
                    }
                std::vector<std::string> closing;
                for (const AnimSlot& sl : slots)
                    if (sl.name == tgt || sl.group == tgt) {
                        closing = sl.closing;
                        break;
                    }
                for (std::size_t k = 0; k < slots.size();) {
                    if (slots[k].name == tgt || slots[k].group == tgt)
                        slots.erase(slots.begin() + long(k));
                    else
                        ++k;
                }
                if (!closing.empty()) {
                    anim_play(tgt, closing,   false,   1);
                    for (AnimSlot& sl : slots)
                        if (sl.name == tgt) {
                            sl.group = tgt;
                            sl.closing_phase = true;
                        }
                }
            } else if ((r.verb == "StatusSet" || r.verb == "StatusIncrement" || r.verb == "StatusDecrement") &&
                       !r.args.empty()) {
                const auto it = status.find(r.args[0]);
                const int cur = it == status.end() ? 0 : it->second;
                if (r.verb == "StatusSet")
                    set_status(r.args[0], r.args.size() >= 2 ? std::atoi(r.args[1].c_str()) : 1, true);
                else if (r.verb == "StatusIncrement")
                    set_status(r.args[0], cur + 1, false);
                else
                    set_status(r.args[0], cur > 0 ? cur - 1 : 0, false);
            } else if (r.verb == "SetTintColor" && !r.args.empty() && std::getenv("SC2_NO_BUS_TINT") == nullptr) {
                int tr = 255, tg = 255, tb = 255;
                if (std::sscanf(r.args[0].c_str(), "%d,%d,%d", &tr, &tg, &tb) == 3) {
                    bus_tint[0] = float(tr) / 255.0f;
                    bus_tint[1] = float(tg) / 255.0f;
                    bus_tint[2] = float(tb) / 255.0f;
                    bus_tint_set = true;
                }
            } else if ((r.verb == "AnimSetPaused" && !r.args.empty()) || r.verb == "AnimSetPausedAll") {
                static const bool kPause = std::getenv("SC2_NO_ANIM_PAUSE") == nullptr;
                if (!kPause) {
                    ++unhandled_verbs;
                    continue;
                }
                if (r.verb == "AnimSetPausedAll") {
                    paused_all = r.args.empty() || r.args[0] != "0";
                } else {
                    const bool on = r.args.size() < 2 || r.args[1] != "0";
                    const auto it = std::find(paused_names.begin(), paused_names.end(), r.args[0]);
                    if (on && it == paused_names.end()) paused_names.push_back(r.args[0]);
                    if (!on && it != paused_names.end()) paused_names.erase(it);
                }
            } else if (r.verb == "Signal" && !r.args.empty() && std::getenv("SC2_NO_ACTOR_SIGNAL") == nullptr) {
                raise("Signal", "Self", r.args[0]);
            } else if (r.verb == "Create" && !r.args.empty()) {
                if (!has_child(r.args[0])) {
                    ActorChild ch;
                    ch.name = r.args[0];
                    if (child_rules) ch.rules = child_rules(ch.name);
                    children.push_back(std::move(ch));
                }
            } else {
                ++unhandled_verbs;
            }
        }
        for (std::size_t ci = 0; ci < children.size();) {
            bool die = false;
            for (const ActorEventRule& cr : children[ci].rules) {
                if (cr.verb != "Destroy" || !cr.args.empty()) continue;
                if (!trigger_field_matches(cr.head, ev.head)) continue;
                if (!trigger_field_matches(cr.subject, ev.subject)) continue;
                if (!trigger_field_matches(cr.sub, ev.sub)) continue;
                if (!filters_match(cr, ev, &unanswerable_filters, nullptr)) continue;
                die = true;
                break;
            }
            if (die)
                children.erase(children.begin() + long(ci));
            else
                ++ci;
        }
    }
    queued.clear();
}

void ActorInstance::set_status(const std::string& name, int value, bool raise_set) {
    const auto it = status.find(name);
    const int old = it == status.end() ? 0 : it->second;
    status[name] = value;
    if (old <= 0 && value > 0) raise("StatusOn", std::string(), name);
    if (old > 0 && value <= 0) raise("StatusOff", std::string(), name);
    if (raise_set) raise("StatusSet", std::string(), name);
}

static bool is_global_set(const std::vector<std::string>& props) {
    static const bool kOff = std::getenv("SC2_NO_GLOBAL_LAYER") != nullptr;
    return !kOff && !props.empty() && props[0].size() > 2 && props[0].compare(0, 2, "GL") == 0;
}

void ActorInstance::anim_play(const std::string& name, const std::vector<std::string>& props,
                              bool looping, int priority) {
    if (dead) return;
    for (size_t i = 0; i < slots.size();) {
        if (slots[i].name == name)
            slots.erase(slots.begin() + long(i));
        else
            ++i;
    }
    AnimSlot s;
    s.name = name;
    s.props = props;
    s.looping = looping;
    s.priority = priority;
    s.time_ms = 0;
    s.sequence = model ? select_sequence(*model, props, seed) : -1;
    static const bool kSlotTakes = std::getenv("SC2_NO_SLOT_TAKES") == nullptr;
    if (kSlotTakes && model && s.sequence >= 0) s.sequence = select_take(*model, s.sequence, seed, ++slot_plays);
    s.global = is_global_set(props);
    slots.push_back(s);
}

void ActorInstance::anim_clear(const std::string& name) {
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].name == name && !slots[i].closing_phase) {
            slots.erase(slots.begin() + long(i));
            return;
        }
    }
}

bool ActorInstance::anim_playing(const std::string& name) const {
    for (const AnimSlot& s : slots)
        if (s.name == name) return true;
    return false;
}

const AnimSlot* ActorInstance::top_slot() const {
    const AnimSlot* best = nullptr;
    for (const AnimSlot& s : slots) {
        if (s.sequence < 0) continue;
        if (s.global) continue;
        if (!best || s.priority >= best->priority) best = &s;
    }
    return best;
}

AnimSlot* ActorInstance::top_slot() {
    return const_cast<AnimSlot*>(static_cast<const ActorInstance*>(this)->top_slot());
}

int ActorInstance::current_sequence() {
    if (const AnimSlot* s = top_slot()) return s->sequence;
    return sequence_of(current());
}

bool ActorInstance::anim_paused(const std::string& slot) const {
    return paused_all || std::find(paused_names.begin(), paused_names.end(), slot) != paused_names.end();
}

ClipLayers ActorInstance::global_layers() const {
    ClipLayers out;
    for (const AnimSlot& s : slots)
        if (s.global && s.sequence >= 0) out.push_back({uint32_t(s.sequence), s.time_ms});
    return out;
}

float ActorInstance::current_time() const {
    if (const AnimSlot* s = top_slot()) return s->time_ms;
    return clock;
}

void ActorInstance::trigger(AnimState s) {
    if (dead) return;
    if (s == AnimState::Death) dead = true;
    if (s == AnimState::Attack) {
        ++swing;
        cached_seq[int(AnimState::Attack)] = -2;
    }
    if (s == AnimState::Attack || s == AnimState::Death || s == AnimState::Cast) {
        ++oneshot_plays;
        cached_seq[int(s)] = -2;
    }
    oneshot = s;
    has_oneshot = true;
    clock = 0;
}

void ActorInstance::set_air_activity(bool moving) {
    if (dead) return;
    baseline = moving ? AnimState::FlyWalk : AnimState::FlyStand;
}

void ActorInstance::set_activity(bool moving, bool gathering) {
    if (dead) return;
    baseline = moving ? AnimState::Walk
                      : (gathering ? AnimState::Gather : AnimState::Stand);
}

void ActorInstance::note_pose(int seq, float time_ms) {
    if (noted_seq >= 0 && seq != noted_seq && seq >= 0 && blend_time_ms > 0.0f) {
        prev_seq = noted_seq;
        prev_time_ms = clock;
        blend_total_ms = blend_time_ms;
        blend_left_ms = blend_time_ms;
    }
    noted_seq = seq;
    (void)time_ms;
}

void ActorInstance::advance(float dt_ms) {
    for (ActorChild& ch : children)
        if (ch.rig) ch.rig->advance(dt_ms);
    last_events.clear();
    if (turret_bone >= 0 && turret_rate_rad > 0.0f) {
        const float dt = dt_ms / 1000.0f;
        const float two_pi = 6.2831853f;
        auto wrap = [&](float a) { while (a > 3.1415927f) a -= two_pi; while (a < -3.1415927f) a += two_pi; return a; };
        if (!turret_has_goal && turret_spin) {
            turret_yaw = wrap(turret_yaw + turret_idle_rate_rad * dt);
        } else {
            if (turret_has_goal) {
                turret_fidget_offset = 0.0f;
                turret_fidget_turning = false;
                turret_fidget_wait_s = -1.0f;
            } else if (turret_had_goal && turret_hold) {
                turret_fidget_offset = wrap(turret_yaw - turret_start_rad);
                turret_fidget_turning = false;
                turret_fidget_wait_s = -1.0f;
            } else if (turret_fidget_max_s > 0.0f && turret_fidget_rate_rad > 0.0f) {
                auto roll = [&]() {
                    if (turret_fidget_rng == 0) turret_fidget_rng = std::uint32_t(seed * 2654435761ull) | 1u;
                    turret_fidget_rng ^= turret_fidget_rng << 13;
                    turret_fidget_rng ^= turret_fidget_rng >> 17;
                    turret_fidget_rng ^= turret_fidget_rng << 5;
                    return float(turret_fidget_rng >> 8) / 16777216.0f;
                };
                if (turret_fidget_wait_s < 0.0f) turret_fidget_wait_s = turret_fidget_min_s + (turret_fidget_max_s - turret_fidget_min_s) * roll();
                turret_fidget_wait_s -= dt;
                if (turret_fidget_wait_s <= 0.0f) {
                    turret_fidget_wait_s = turret_fidget_min_s + (turret_fidget_max_s - turret_fidget_min_s) * roll();
                    if (roll() < turret_fidget_chance) {
                        turret_fidget_offset = wrap(turret_fidget_offset + (roll() * 2.0f - 1.0f) * turret_fidget_turn_rad);
                        turret_fidget_turning = true;
                    }
                }
            }
            float goal = turret_has_goal ? turret_goal : turret_start_rad + turret_fidget_offset;
            if (turret_arc_rad < two_pi - 1e-3f) {
                const float half = turret_arc_rad * 0.5f;
                const float rel = wrap(goal - turret_start_rad);
                goal = turret_start_rad + std::max(-half, std::min(half, rel));
            }
            const float d = wrap(goal - turret_yaw);
            const float step = (turret_fidget_turning ? turret_fidget_rate_rad : turret_rate_rad) * dt;
            if (std::fabs(d) <= step) turret_fidget_turning = false;
            turret_yaw = wrap(std::fabs(d) <= step ? goal : turret_yaw + (d > 0.0f ? step : -step));
        }
        turret_had_goal = turret_has_goal;
    }
    if (blend_left_ms > 0.0f) {
        blend_left_ms -= dt_ms;
        if (model && prev_seq >= 0 && std::size_t(prev_seq) < model->sequences.size()) {
            const float len = float(model->sequences[std::size_t(prev_seq)].length_ms());
            prev_time_ms = std::min(prev_time_ms + dt_ms, len);
        }
        if (blend_left_ms <= 0.0f) { blend_left_ms = 0.0f; prev_seq = -1; }
    }
    if (!model) return;
    for (AnimSlot& gl : slots) {
        if (!gl.global || gl.sequence < 0 || anim_paused(gl.name)) continue;
        const float len = clip_len(*model, gl.sequence);
        float to = gl.time_ms + dt_ms;
        if (len > 0.0f && to >= len) {
            if (gl.looping) {
                to -= len * float(int(to / len));
            } else {
                to = len;
                if (!gl.closing_phase && !gl.content.empty()) {
                    const int cseq = select_sequence(*model, gl.content, seed);
                    if (cseq >= 0) {
                        gl.props = gl.content;
                        gl.sequence = cseq;
                        gl.looping = true;
                        to = 0.0f;
                    }
                    gl.content.clear();
                }
            }
        }
        gl.time_ms = to;
    }
    if (paused_all) return;
    if (const AnimSlot* ps = top_slot(); ps && anim_paused(ps->name)) return;
    if (AnimSlot* sl = top_slot()) {
        const float len = clip_len(*model, sl->sequence);
        const float from = sl->time_ms;
        float to = from + dt_ms;
        if (to <= len) {
            collect_events(*model, uint32_t(sl->sequence), from, to, last_events);
        } else {
            collect_events(*model, uint32_t(sl->sequence), from, len, last_events);
            if (sl->looping) {
                float rem = to - len;
                if (rem > len) rem = len;
                collect_events(*model, uint32_t(sl->sequence), 0.0f, rem, last_events);
            }
        }
        const bool just_finished = !sl->looping && from < len && to >= len;
        const std::string finished_slot = just_finished ? sl->name : std::string();
        if (to >= len) to = sl->looping ? to - len * float(int(to / len)) : len;
        sl->time_ms = to;
        if (just_finished && !sl->closing_phase && !sl->content.empty()) {
            int cseq = select_sequence(*model, sl->content, seed);
            static const bool kContentTakes = std::getenv("SC2_NO_SLOT_TAKES") == nullptr;
            if (kContentTakes && cseq >= 0) cseq = select_take(*model, cseq, seed, ++slot_plays);
            if (cseq >= 0) {
                sl->props = sl->content;
                sl->sequence = cseq;
                sl->looping = true;
                sl->time_ms = 0.0f;
            }
            sl->content.clear();
        }
        if (just_finished)
            raise("AnimDone", std::string(), "Complete", {{"AnimName", finished_slot}});
        return;
    }
    const int seq = sequence_of(current());
    const float len = clip_len(*model, seq);
    const float dt = (current() == AnimState::Walk) ? dt_ms * walk_scale : dt_ms;
    const float from = clock;
    const float to = clock + dt;
    if (to <= len) {
        collect_events(*model, uint32_t(seq), from, to, last_events);
    } else {
        collect_events(*model, uint32_t(seq), from, len, last_events);
        if (!has_oneshot) {
            float rem = to - len;
            if (rem > len) rem = len;
            collect_events(*model, uint32_t(seq), 0.0f, rem, last_events);
        }
    }
    clock = to;
    if (has_oneshot) {
        if (current() == AnimState::Death) {
            if (clock > len) clock = len;
        } else if (clock >= len) {
            has_oneshot = false;
            clock = 0;
        }
    } else if (clock >= len) {
        clock -= len * float(int(clock / len));
        static const bool kIdleTakes = std::getenv("SC2_NO_IDLE_TAKES") == nullptr;
        const AnimState st = current();
        if (kIdleTakes && (st == AnimState::Stand || st == AnimState::FlyStand)) {
            ++idle_loops;
            const int before = cached_seq[int(st)];
            cached_seq[int(st)] = -2;
            if (sequence_of(st) != before) clock = 0.0f;
        }
        static const bool kWalkTakes = std::getenv("SC2_NO_WALK_TAKES") == nullptr;
        if (kWalkTakes && (st == AnimState::Walk || st == AnimState::FlyWalk)) {
            ++walk_loops;
            const int before = cached_seq[int(st)];
            cached_seq[int(st)] = -2;
            if (sequence_of(st) != before) clock = 0.0f;
        }
    }
}

std::vector<Mat4> ActorInstance::pose_now() {
    if (!model) return {};
    return pose(*model, uint32_t(current_sequence()), current_time());
}

bool ActorInstance::attachment(const std::string& name, Mat4& out) {
    if (!model) return false;
    return attachment_transform(*model, name, uint32_t(current_sequence()), current_time(), out);
}

void ActorDriver::observe(const sim::World& world, float dt_ms, bool sim_ticked) {
    const std::vector<sim::Unit>& units = world.units();
    if (shadows.size() < units.size()) shadows.resize(units.size());
    if (actors.size() < units.size()) actors.resize(units.size());

    for (size_t i = 0; i < units.size(); ++i) {
        const sim::Unit& u = units[i];
        Shadow& s = shadows[i];
        ActorInstance& a = actors[i];

        if (!s.known) {
            a.model = (u.id < models.size() && models[u.id]) ? models[u.id] : default_model;
            a.rules = (u.id < rules.size() && rules[u.id]) ? rules[u.id] : default_rules;
            a.seed = u.id + 1;
            if (unit_rules && !u.catalog_id.empty()) a.bind_event_rules(unit_rules(u.catalog_id));
            if (sub_actor_rules) a.child_rules = sub_actor_rules;
            if (harvest_ability && !u.catalog_id.empty())
                a.harvest_abil = harvest_ability(u.catalog_id);
            if (a.model && select_sequence(*a.model, a.tokens_of(AnimState::Birth), a.seed) >= 0)
                a.trigger(AnimState::Birth);
            else
                a.has_oneshot = false;
            if (!a.has_oneshot && a.model) {
                const int seq = a.sequence_of(a.current());
                const float len = clip_len(*a.model, seq);
                uint64_t h = (uint64_t(u.id) + 1) * 0x9E3779B97F4A7C15ull;
                h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
                a.clock = float(h & 0xFFFFFFu) / float(0x1000000u) * len;
            }
            if (u.id < spawn_facing_set.size() && spawn_facing_set[u.id]) {
                a.facing = spawn_facing[u.id];
                a.facing_known = true;
            }
            if (!s.created_raised) {
                a.raise("ActorCreation", u.catalog_id, std::string());
                s.created_raised = true;
                if (u.constructing) a.raise("UnitConstruction", u.catalog_id, "Start");
            }
            s.known = true;
            s.alive = u.alive;
            s.constructing = u.constructing;
            s.moving = false;
            s.x = u.x; s.y = u.y; s.cooldown = u.cooldown; s.cast_count = u.cast_count;
            s.flying = u.flying;
            s.burrowed = u.burrowed;
            s.lowered = u.lowered;
            if (u.lowered && depot_pair && unit_rules && !u.catalog_id.empty()) {
                const BurrowPair dp = depot_pair(u.catalog_id);
                if (!dp.variant.empty()) {
                    a.bind_event_rules(unit_rules(dp.variant));
                    a.raise("UnitBirth", dp.variant, std::string());
                }
            }
            if (u.flying && u.alive) a.set_air_activity(false);
            if (u.alive && !u.constructing && !u.lowered) a.apply_birth_groups(u.catalog_id);
        }

        if (u.id < models.size() && models[u.id] && models[u.id] != a.model) {
            a.model = models[u.id];
            for (int& c : a.cached_seq) c = -2;
            for (AnimSlot& sl : a.slots)
                sl.sequence = select_sequence(*a.model, sl.props, a.seed);
        }

        if (sim_ticked) {
            if (s.alive && !u.alive) a.trigger(AnimState::Death);
            if (s.alive && !u.alive && !s.destroyed_raised) {
                a.raise("UnitDeath", u.catalog_id, std::string());
                a.raise("ActorDestruction", u.catalog_id, std::string());
                s.destroyed_raised = true;
            }
            if (u.constructing != s.constructing) {
                if (u.constructing)
                    a.raise("UnitConstruction", u.catalog_id, "Start");
                else
                    a.raise("UnitConstruction", u.catalog_id, u.alive ? "Finish" : "Cancel");
                s.constructing = u.constructing;
            }

            if (u.cooldown > s.cooldown && s.cooldown == sim::Fixed::from_int(0) && u.alive)
                a.trigger(AnimState::Attack);

            if (u.cast_count != s.cast_count && u.alive) a.trigger(AnimState::Cast);

            if (u.alive && u.flying != s.flying)
                a.trigger(u.flying ? AnimState::FlyStart : AnimState::FlyEnd);
            if (u.alive && !u.flying && s.flying && u.speed.raw() == 0 &&
                u.id < spawn_facing_set.size() && spawn_facing_set[u.id]) {
                a.facing = spawn_facing[u.id];
                a.facing_known = true;
            }

            if (u.alive && u.burrowed != s.burrowed && burrow_pair && !u.catalog_id.empty()) {
                const BurrowPair bp = burrow_pair(u.catalog_id);
                if (!bp.down_ability.empty() && !bp.variant.empty()) {
                    const bool down = u.burrowed;
                    const std::string& abil = down ? bp.down_ability : bp.up_ability;
                    const std::string& key = down ? bp.down_key : bp.up_key;
                    const std::string& from = down ? u.catalog_id : bp.variant;
                    const std::string& to = down ? bp.variant : u.catalog_id;
                    if (down) a.raise("UnitBirth", bp.variant, std::string());
                    const std::vector<std::pair<std::string, std::string>> ctx = {
                        {"AbilKey", key}, {"MorphFrom", from}, {"MorphTo", to}};
                    if (!abil.empty()) {
                        a.raise("AbilMorph", abil, "Start", ctx);
                        s.burrow_finish_ms = float(down ? bp.down_ms : bp.up_ms);
                        s.burrow_finish_down = down;
                    }
                }
            }

            if (s.burrow_finish_ms >= 0.0f && burrow_pair && !u.catalog_id.empty()) {
                s.burrow_finish_ms -= dt_ms;
                if (s.burrow_finish_ms <= 0.0f) {
                    s.burrow_finish_ms = -1.0f;
                    const BurrowPair bp = burrow_pair(u.catalog_id);
                    const bool down = s.burrow_finish_down;
                    const std::string& abil = down ? bp.down_ability : bp.up_ability;
                    if (!abil.empty())
                        a.raise("AbilMorph", abil, "Finish",
                                {{"AbilKey", down ? bp.down_key : bp.up_key},
                                 {"MorphFrom", down ? u.catalog_id : bp.variant},
                                 {"MorphTo", down ? bp.variant : u.catalog_id}});
                }
            }

            {
                auto depot_finish = [&](const BurrowPair& dp) {
                    const bool down = s.depot_finish_down;
                    s.depot_finish_ms = -1.0f;
                    const std::string& from = down ? u.catalog_id : dp.variant;
                    const std::string& to = down ? dp.variant : u.catalog_id;
                    if (unit_rules) a.bind_event_rules(unit_rules(to));
                    a.raise("AbilMorph", down ? dp.down_ability : dp.up_ability, "Finish",
                            {{"MorphFrom", from}, {"MorphTo", to}});
                };
                if (depot_pair && !u.catalog_id.empty() &&
                    (u.lowered != s.lowered || s.depot_finish_ms >= 0.0f)) {
                    const BurrowPair dp = depot_pair(u.catalog_id);
                    if (!dp.variant.empty()) {
                        if (u.alive && u.lowered != s.lowered) {
                            if (s.depot_finish_ms >= 0.0f) depot_finish(dp);
                            const bool down = u.lowered;
                            a.raise("AbilMorph", down ? dp.down_ability : dp.up_ability, "Start",
                                    {{"MorphFrom", down ? u.catalog_id : dp.variant},
                                     {"MorphTo", down ? dp.variant : u.catalog_id}});
                            s.depot_finish_ms = float(down ? dp.down_ms : dp.up_ms);
                            s.depot_finish_down = down;
                        } else if (s.depot_finish_ms >= 0.0f) {
                            s.depot_finish_ms -= dt_ms;
                            if (s.depot_finish_ms <= 0.0f) depot_finish(dp);
                        }
                    }
                }
                s.lowered = u.lowered;
            }

            bool moving = (u.x != s.x || u.y != s.y);
            if (moving != s.moving) {
                a.raise("UnitMovementUpdate", u.catalog_id, moving ? "Walk" : "Stand");
                s.moving = moving;
            }
            if (u.is_gas && !a.event_rules.empty() && (u.resource_left > 0 || s.res_depleted >= 0 || s.res_cap > 0)) {
                const int depleted = u.resource_left > 0 ? 0 : 1;
                if (depleted != s.res_depleted) {
                    const char* want = depleted ? "Deplete" : "Restore";
                    for (const ActorEventRule& rr : a.event_rules)
                        if (rr.head == "Behavior" && rr.sub == want) {
                            const std::string subject = rr.subject;
                            a.raise("Behavior", subject, want);
                            break;
                        }
                    s.res_depleted = depleted;
                }
            }
            static const bool kPowerLevel = std::getenv("SC2_NO_POWER_LEVEL") == nullptr;
            if (kPowerLevel && u.alive && !a.event_rules.empty() && u.needs_power && u.power_managed) {
                const int down = sim::power_disabled(u) ? 1 : 0;
                if (down != s.power_down && !(s.power_down < 0 && down == 0)) {
                    const std::string* subj = nullptr;
                    for (const ActorEventRule& rr : a.event_rules)
                        if (rr.head == "BehaviorLevel" && rr.subject.rfind("PowerUser", 0) == 0) { subj = &rr.subject; break; }
                    if (subj) {
                        const std::string subject = *subj;
                        a.raise("BehaviorLevel", subject, down ? "Start" : "Stop", {{"Level", "0"}});
                    }
                }
                s.power_down = down;
            }
            if (u.resource_left > 0 && !a.event_rules.empty()) {
                if (u.resource_left > s.res_cap) s.res_cap = u.resource_left;
                constexpr int kStages = 4;
                int level = int((std::int64_t(u.resource_left) * kStages) / std::max<std::int64_t>(1, s.res_cap));
                if (level >= kStages) level = kStages - 1;
                if (const char* force = std::getenv("SC2_RES_LEVEL")) level = std::atoi(force);
                if (level != s.res_level) {
                    const std::string* subj = nullptr;
                    for (const ActorEventRule& rr : a.event_rules)
                        if (rr.head == "BehaviorLevel" && rr.sub == "Start") { subj = &rr.subject; break; }
                    if (subj) {
                        const std::string subject = *subj;
                        if (s.res_level >= 0) a.raise("BehaviorLevel", subject, "Stop", {{"Level", std::to_string(s.res_level)}});
                        a.raise("BehaviorLevel", subject, "Start", {{"Level", std::to_string(level)}});
                    }
                    if (std::getenv("SC2_RES_TRACE"))
                        std::fprintf(stderr, "res level: %s left %d cap %d level %d rules %zu subject %s\n", u.catalog_id.c_str(), int(u.resource_left),
                                     int(s.res_cap), level, a.event_rules.size(), subj ? subj->c_str() : "(none)");
                    s.res_level = level;
                }
            }
            if (u.cooldown > s.cooldown && s.cooldown == sim::Fixed::from_int(0) && u.alive)
                a.raise("WeaponStart", u.catalog_id, "AttackStart");
            if (u.flying)
                a.set_air_activity(moving);
            else {
                const bool gathering_now = u.mine_cd.raw() > 0;
                a.set_activity(moving, gathering_now);
                if (!a.harvest_abil.empty() && gathering_now != s.gathering) {
                    a.raise("Abil", a.harvest_abil, gathering_now ? "Start" : "Stop");
                    s.gathering = gathering_now;
                }
                const std::string mt = (morph_target && s.catalog_id == u.catalog_id) ? morph_target(u.id) : std::string();
                if (!mt.empty() && s.morph_to.empty() && !u.catalog_id.empty()) {
                    a.raise("AbilMorph", std::string(), "Start", {{"MorphTo", mt}, {"MorphFrom", u.catalog_id}});
                    if (std::getenv("SC2_MORPH_TRACE")) std::fprintf(stderr, "[morph] tick %d unit %u %s start -> %s\n", world.tick(), unsigned(u.id), u.catalog_id.c_str(), mt.c_str());
                    s.morph_to = mt;
                } else if (mt.empty() && !s.morph_to.empty() && s.catalog_id == u.catalog_id) {
                    a.raise("AbilMorph", std::string(), "Cancel", {{"MorphTo", s.morph_to}, {"MorphFrom", u.catalog_id}});
                    s.morph_to.clear();
                }
                if (!u.catalog_id.empty() && s.catalog_id != u.catalog_id) {
                    if (!s.catalog_id.empty()) {
                        if (s.morph_to.empty())
                            a.raise("AbilMorph", std::string(), "Start",
                                    {{"MorphTo", u.catalog_id}, {"MorphFrom", s.catalog_id}});
                        bool reborn = false;
                        if (std::getenv("SC2_MORPH_TRACE"))
                            std::fprintf(stderr, "[morph] tick %d unit %u id %s -> %s (start raised for '%s', rules %zu, slots %zu, groups %zu)\n", world.tick(), unsigned(u.id),
                                         s.catalog_id.c_str(), u.catalog_id.c_str(), s.morph_to.c_str(), a.event_rules.size(), a.slots.size(), a.anim_groups.size());
                        if (unit_rules && !std::getenv("SC2_MORPH_KEEP_ACTOR")) {
                            const std::vector<ActorEventRule>& nr = unit_rules(u.catalog_id);
                            for (const ActorEventRule& r : nr) {
                                if (r.head != "AbilMorph" || r.sub != "Finish" || r.verb != "Create" || !r.args.empty()) continue;
                                bool to = false, from = false;
                                for (const auto& f : r.filters) {
                                    if (f.first == "MorphTo" && f.second == u.catalog_id) to = true;
                                    if (f.first == "MorphFrom" && f.second == s.catalog_id) from = true;
                                }
                                if (to && from) { reborn = true; break; }
                            }
                            if (reborn) {
                                a.slots.clear();
                                a.anim_groups.clear();
                                a.birth_clips.clear();
                                for (int& cs : a.cached_seq) cs = -2;
                                a.bind_event_rules(nr);
                                a.raise("ActorCreation", u.catalog_id, std::string());
                                a.apply_birth_groups(u.catalog_id);
                            }
                        }
                        if (!reborn)
                            a.raise("AbilMorph", std::string(), "Finish",
                                    {{"MorphTo", u.catalog_id}, {"MorphFrom", s.catalog_id}});
                    }
                    s.morph_to.clear();
                    s.catalog_id = u.catalog_id;
                }
            }

            const double authored_walk =
                a.rules && a.rules->walk_anim_move_speed > 0.0 ? a.rules->walk_anim_move_speed : 0.0;
            float authored = float(authored_walk > 0.0 ? authored_walk : u.speed.to_double());
            float actual = float(u.speed.to_double());
            if (authored > 0.0f) {
                if (world.creep().is_creep(u.x, u.y))
                    actual *= float(u.speed_mult_creep.to_double());
                for (const sim::BehaviorInstance& b : u.behaviors)
                    actual *= float(b.speed_mult.to_double());
            }
            a.walk_scale = walk_time_scale(authored, actual, u.flying);
            {
                const bool immobile_placed = u.speed.raw() == 0 && !u.flying &&
                                             u.id < spawn_facing_set.size() && spawn_facing_set[u.id];
                const double dx = immobile_placed ? 0.0 : (u.x - s.x).to_double();
                const double dy = immobile_placed ? 0.0 : (u.y - s.y).to_double();
                if (dx != 0.0 || dy != 0.0) {
                    a.facing = std::atan2(float(dy), float(dx));
                    a.facing_known = true;
                } else if (u.mine_cd.raw() > 0 && size_t(u.gather_target) < units.size() &&
                           !std::getenv("SC2_MINING_KEEPS_HEADING")) {
                    const double tx = (units[u.gather_target].x - u.x).to_double();
                    const double ty = (units[u.gather_target].y - u.y).to_double();
                    if (tx != 0.0 || ty != 0.0) {
                        a.facing = std::atan2(float(ty), float(tx));
                        a.facing_known = true;
                    }
                } else if (!immobile_placed && u.target != sim::kNoTarget &&
                           size_t(u.target) < units.size() && units[u.target].alive) {
                    const double tx = (units[u.target].x - u.x).to_double();
                    const double ty = (units[u.target].y - u.y).to_double();
                    if (tx != 0.0 || ty != 0.0) {
                        a.facing = std::atan2(float(ty), float(tx));
                        a.facing_known = true;
                    }
                }
            }

            s.alive = u.alive;
            s.x = u.x; s.y = u.y; s.cooldown = u.cooldown; s.cast_count = u.cast_count;
            s.flying = u.flying;
            s.burrowed = u.burrowed;
        }

        a.advance(dt_ms);
        a.pump(dt_ms);
        if (audio && !a.last_events.empty())
            emit_event_cues(*audio, a.last_events, u.id);
    }
}

}
