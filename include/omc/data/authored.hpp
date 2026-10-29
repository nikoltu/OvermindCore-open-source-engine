#pragma once

#include <cstdlib>

#include "omc/sim/fixed.hpp"

namespace omc::data {

inline double authored_seconds(const char* text) {
    return sim::Fixed::from_double(std::strtod(text, nullptr)).to_double();
}

}
