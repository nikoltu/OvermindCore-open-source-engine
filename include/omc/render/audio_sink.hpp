#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "omc/formats/m3.hpp"

namespace omc::render {

struct AudioSink {
    struct Cue {
        std::string name;
        uint32_t unit;
        float x, y, z;
    };

    std::vector<Cue> queue;

    void emit(std::string name, uint32_t unit = 0, float x = 0, float y = 0, float z = 0) {
        queue.push_back({std::move(name), unit, x, y, z});
    }
    std::vector<Cue> drain() {
        std::vector<Cue> out;
        out.swap(queue);
        return out;
    }
    void clear() { queue.clear(); }
    size_t pending() const { return queue.size(); }
};

inline void emit_event_cues(AudioSink& sink,
                            const std::vector<formats::M3Event>& events, uint32_t unit) {
    for (const formats::M3Event& e : events) sink.emit(e.name, unit);
}

}
