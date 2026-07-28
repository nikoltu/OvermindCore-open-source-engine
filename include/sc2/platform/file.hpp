#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sc2::platform {

std::vector<uint8_t> read_file(const std::string& path);

std::vector<uint8_t> read_file_at(const std::string& path, uint64_t offset,
                                  size_t count);

uint64_t file_size(const std::string& path);
bool file_exists(const std::string& path);

std::vector<std::string> list_dir(const std::string& dir,
                                  const std::string& suffix);

}
