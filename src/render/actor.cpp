#include <cstdlib>
#include "sc2/render/actor.hpp"

#include <cmath>

#include "sc2/render/actor_rules.hpp"
#include "sc2/render/anim_select.hpp"
#include "sc2/render/audio_sink.hpp"

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
            if (r < 0) r = select_sequence(*model, base, seed);
            if (r < 0 && (s == AnimState::FlyStart || s == AnimState::FlyWalk ||
                          s == AnimState::FlyEnd))
                r = select_sequence(*model, tokens_of(AnimState::FlyStand), seed);
        }
        if (r < 0 && model)
            r = select_sequence(*model, tokens_of(AnimState::Stand), seed);
        if (r < 0 && model && !model->sequences.empty()) r = 0;
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

void ActorInstance::raise(const std::string& head, const std::string& subject,
                          const std::string& sub,
                          std::vector<std::pair<std::string, std::string>> ctx) {
    if (event_rules.empty()) return;
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
            ++done;
            ++rules_fired;
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
                if (r.args.size() < 2) {
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
                    }
            } else if ((r.verb == "AnimGroupRemove" || r.verb == "AnimBracketStop") &&
                       !r.args.empty()) {
                const std::string tgt = r.args[0];
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

void ActorInstance::advance(float dt_ms) {
    for (ActorChild& ch : children)
        if (ch.rig) ch.rig->advance(dt_ms);
    last_events.clear();
    if (!model) return;
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
            const int cseq = select_sequence(*model, sl->content, seed);
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
            if (unit_rules && !u.catalog_id.empty()) a.event_rules = unit_rules(u.catalog_id);
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
                    a.event_rules = unit_rules(dp.variant);
                    a.raise("UnitBirth", dp.variant, std::string());
                }
            }
            if (u.flying && u.alive) a.set_air_activity(false);
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
                    if (unit_rules) a.event_rules = unit_rules(to);
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
                if (!u.catalog_id.empty() && s.catalog_id != u.catalog_id) {
                    if (!s.catalog_id.empty()) {
                        a.raise("AbilMorph", std::string(), "Start",
                                {{"MorphTo", u.catalog_id}, {"MorphFrom", s.catalog_id}});
                        a.raise("AbilMorph", std::string(), "Finish",
                                {{"MorphTo", u.catalog_id}, {"MorphFrom", s.catalog_id}});
                    }
                    s.catalog_id = u.catalog_id;
                }
            }

            float authored = float(u.speed.to_double());
            float actual = authored;
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
