#pragma once

#include <optional>
#include <string>
#include <vector>

namespace omc::data {

class ActorCatalog;
class AudioCatalog;

enum class UnitEvent { Birth, Attack, Death, Ready, Select, Confirm, AttackOrder, Pissed };

const char* to_string(UnitEvent event);

std::optional<UnitEvent> parse_unit_event(const std::string& name);

struct SoundCue {
    std::string unit;
    std::string event;
    std::string sound_id;
    std::string asset_path;
};

std::vector<SoundCue> resolve_sound_cues(const ActorCatalog& actors,
                                         const AudioCatalog& audio,
                                         const std::string& unit, UnitEvent event);

std::vector<SoundCue> resolve_sound_cues(const ActorCatalog& actors,
                                         const AudioCatalog& audio,
                                         const std::string& unit,
                                         const std::string& event_name);

}
