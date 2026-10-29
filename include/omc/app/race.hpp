#pragma once

#include <cstdint>

namespace omc::app {

enum class Race { Terran, Protoss, Zerg };

enum class LobbyRace { Terran, Protoss, Zerg, Random };
inline LobbyRace lobby_race_cycle(LobbyRace r) {
    return r == LobbyRace::Terran ? LobbyRace::Protoss : r == LobbyRace::Protoss ? LobbyRace::Zerg
         : r == LobbyRace::Zerg ? LobbyRace::Random : LobbyRace::Terran;
}
inline const char* lobby_race_name(LobbyRace r) {
    return r == LobbyRace::Terran ? "Terran" : r == LobbyRace::Protoss ? "Protoss" : r == LobbyRace::Zerg ? "Zerg" : "Random";
}
inline Race resolve_lobby_race(LobbyRace pick, std::uint64_t seed, int slot) {
    if (pick != LobbyRace::Random) return Race(int(pick));
    std::uint64_t z = seed + 0x9E3779B97F4A7C15ull * std::uint64_t(slot + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return Race(int(z % 3));
}

}
