#include "sc2/platform/file.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace sc2::platform {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open: " + path);
    std::streamsize n = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    if (n > 0 && !f.read(reinterpret_cast<char*>(buf.data()), n))
        throw std::runtime_error("read failed: " + path);
    return buf;
}

std::vector<uint8_t> read_file_at(const std::string& path, uint64_t offset,
                                  size_t count) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open: " + path);
    f.seekg(std::streamoff(offset));
    std::vector<uint8_t> buf(count);
    if (count > 0 &&
        !f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(count)))
        throw std::runtime_error("short read: " + path);
    return buf;
}

uint64_t file_size(const std::string& path) {
    std::error_code ec;
    auto n = fs::file_size(path, ec);
    if (ec) throw std::runtime_error("stat failed: " + path);
    return n;
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::vector<std::string> list_dir(const std::string& dir,
                                  const std::string& suffix) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        std::string name = e.path().filename().string();
        if (suffix.empty() ||
            (name.size() >= suffix.size() &&
             name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0))
            out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}
