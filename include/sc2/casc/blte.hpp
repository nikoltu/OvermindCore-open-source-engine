#pragma once

#include <cstdint>
#include <vector>

namespace sc2::casc {

struct BlteEncrypted {
    std::vector<uint8_t> partial;
};

std::vector<uint8_t> blte_decode(const uint8_t* data, size_t size);
std::vector<uint8_t> blte_decode(const std::vector<uint8_t>& data);

}
