#pragma once

#include <cstddef>

namespace sc2::app {

struct CasterComposition {
    const char* race;
    const char* caster;
    const char* tech;
    const char* ability;
};

inline constexpr CasterComposition kCasterCompositions[] = {
    {"Terran", "Ghost", "GhostAcademy", "ChannelSnipe"},
    {"Protoss", "HighTemplar", "TemplarArchive", "PsiStorm"},
    {"Zerg", "Infestor", "InfestationPit", "FungalGrowth"},
};
inline constexpr std::size_t kNumCasterCompositions =
    sizeof(kCasterCompositions) / sizeof(kCasterCompositions[0]);

}
