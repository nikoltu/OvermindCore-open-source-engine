#pragma once

#include <cstdlib>

#include "sc2/sim/fixed.hpp"

namespace sc2::data {

inline double authored_seconds(const char* text) {
    return sim::Fixed::from_double(std::strtod(text, nullptr)).to_double();
}

}
