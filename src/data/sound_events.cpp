#include "sc2/data/sound_events.hpp"

#include "sc2/data/actor_catalog.hpp"
#include "sc2/data/audio.hpp"

namespace sc2::data {

const char* to_string(UnitEvent event) {
    switch (event) {
        case UnitEvent::Birth:   return "Birth";
        case UnitEvent::Attack:  return "Attack";
        case UnitEvent::Death:   return "Death";
        case UnitEvent::Ready:   return "Ready";
        case UnitEvent::Select:  return "Select";
        case UnitEvent::Confirm: return "Confirm";
    }
    return "";
}

std::optional<UnitEvent> parse_unit_event(const std::string& name) {
    if (name == "Birth") return UnitEvent::Birth;
    if (name == "Attack") return UnitEvent::Attack;
    if (name == "Death") return UnitEvent::Death;
    if (name == "Ready") return UnitEvent::Ready;
    if (name == "Select") return UnitEvent::Select;
    if (name == "Confirm") return UnitEvent::Confirm;
    return std::nullopt;
}

namespace {

struct EventSpec {
    std::vector<std::string> heads;
    std::vector<std::string> suffixes;
};

EventSpec spec_for(UnitEvent e) {
    switch (e) {
        case UnitEvent::Birth:   return {{"UnitBirth", "ActorCreation"}, {"Birth"}};
        case UnitEvent::Attack:  return {{"WeaponStart"}, {"AttackLaunch", "Attack"}};
        case UnitEvent::Death:   return {{"UnitDeath"}, {"Death"}};
        case UnitEvent::Ready:   return {{}, {"Ready"}};
        case UnitEvent::Select:  return {{}, {"What"}};
        case UnitEvent::Confirm: return {{}, {"Yes"}};
    }
    return {};
}

bool rule_matches(const ActorRule& r, UnitEvent e, const EventSpec& spec) {
    for (const std::string& h : spec.heads)
        if (r.event_head == h) return true;
    if (e == UnitEvent::Ready && r.event.find("Ready") != std::string::npos) return true;
    return false;
}

}

std::vector<SoundCue> resolve_sound_cues(const ActorCatalog& actors,
                                         const AudioCatalog& audio,
                                         const std::string& unit, UnitEvent event) {
    std::vector<SoundCue> out;

    const CActorUnit* actor = actors.actor_for_unit(unit);
    if (!actor) return out;

    const std::string event_name = to_string(event);
    const std::string& prefix = actor->unit_name;
    const EventSpec spec = spec_for(event);

    std::vector<std::string> seen;
    auto emit = [&](const std::string& sound_id) {
        if (sound_id.empty()) return;
        for (const std::string& s : seen)
            if (s == sound_id) return;
        std::vector<std::string> waves = audio.resolve(sound_id);
        if (waves.empty()) return;
        seen.push_back(sound_id);
        for (const std::string& w : waves)
            out.push_back(SoundCue{unit, event_name, sound_id, w});
    };

    for (const ActorRule& r : actor->on) {
        if (!rule_matches(r, event, spec)) continue;
        if (r.verb == "PlayUnitSound" && !r.args.empty())
            emit(prefix + "_" + r.args[0]);
    }

    if (event == UnitEvent::Death)
        for (const ActorDeath& d : actor->death_array)
            emit(d.sound_link);

    for (const std::string& suf : spec.suffixes)
        emit(prefix + "_" + suf);

    return out;
}

std::vector<SoundCue> resolve_sound_cues(const ActorCatalog& actors,
                                         const AudioCatalog& audio,
                                         const std::string& unit,
                                         const std::string& event_name) {
    std::optional<UnitEvent> e = parse_unit_event(event_name);
    if (!e) return {};
    return resolve_sound_cues(actors, audio, unit, *e);
}

}
