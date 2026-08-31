#pragma once

#include <cstdlib>
#include <string>
#include <unordered_map>

#include "sc2/casc/storage.hpp"

namespace sc2::app {

inline std::unordered_map<std::string, float> water_type_heights(casc::Storage& store) {
    std::unordered_map<std::string, float> out;
    static const char* kMods[] = {"void", "swarm", "liberty", "core"};
    for (const char* m : kMods) {
        std::string xml;
        try {
            auto b = store.read_by_name(std::string("mods/") + m +
                                        ".sc2mod/base.sc2data/GameData/WaterData.xml");
            xml.assign(b.begin(), b.end());
        } catch (const std::exception&) { continue; }

        std::size_t pos = 0;
        while (true) {
            std::size_t tag = xml.find("<CWater id=\"", pos);
            if (tag == std::string::npos) break;
            std::size_t idStart = tag + 12;
            std::size_t idEnd = xml.find('"', idStart);
            if (idEnd == std::string::npos) break;
            const std::string id = xml.substr(idStart, idEnd - idStart);
            pos = idEnd + 1;
            if (out.count(id)) continue;

            std::size_t gt = xml.find_first_of("/>", pos);
            if (gt == std::string::npos) break;
            if (xml[gt] == '/') { pos = gt + 2; continue; }
            pos = gt + 1;

            std::size_t blockEnd = xml.find("</CWater>", pos);
            if (blockEnd == std::string::npos) break;
            std::size_t stateStart = xml.find("<State", pos);
            if (stateStart != std::string::npos && stateStart < blockEnd) {
                std::size_t nextState = xml.find("<State", stateStart + 1);
                const std::size_t stateEnd = (nextState != std::string::npos && nextState < blockEnd)
                                                  ? nextState
                                                  : blockEnd;
                std::size_t h = xml.find("<Height value=\"", stateStart);
                if (h != std::string::npos && h < stateEnd) {
                    h += 15;
                    std::size_t hEnd = xml.find('"', h);
                    if (hEnd != std::string::npos)
                        out.emplace(id, std::strtof(xml.substr(h, hEnd - h).c_str(), nullptr));
                }
            }
            pos = blockEnd + 9;
        }
    }
    return out;
}

}
