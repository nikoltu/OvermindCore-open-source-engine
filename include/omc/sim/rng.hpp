#pragma once

#include <cstdint>

namespace omc::sim {

class Rng {
public:
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull)
        : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}

    uint64_t next() {
        uint64_t x = state_;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state_ = x;
        return x * 0x2545F4914F6CDD1Dull;
    }

    uint32_t below(uint32_t n) { return uint32_t(next() % n); }

    uint64_t state() const { return state_; }

private:
    uint64_t state_;
};

}
