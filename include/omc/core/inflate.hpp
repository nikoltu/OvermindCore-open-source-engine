#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace omc::core {

std::vector<uint8_t> raw_inflate(const uint8_t* data, size_t size);
std::vector<uint8_t> zlib_inflate(const uint8_t* data, size_t size);

}
