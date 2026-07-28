#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sc2::formats {

class Mpq {
public:
    static Mpq open(const std::vector<uint8_t>& bytes);
    static Mpq open(std::vector<uint8_t>&& bytes);

    std::vector<uint8_t> read_file(const std::string& name) const;

    bool contains(const std::string& name) const;

    std::vector<std::string> list() const;

    uint16_t format_version() const { return format_version_; }
    uint32_t sector_size() const { return sector_size_; }
    uint32_t hash_table_size() const { return uint32_t(hash_.size()); }
    uint32_t block_table_size() const { return uint32_t(block_.size()); }
    size_t file_count() const;

private:
    struct HashEntry {
        uint32_t name1;
        uint32_t name2;
        uint32_t locale_platform;
        uint32_t block_index;
    };
    struct BlockEntry {
        uint32_t file_pos;
        uint32_t comp_size;
        uint32_t file_size;
        uint32_t flags;
    };

    void parse();
    long find(const std::string& name) const;

    std::vector<uint8_t> image_;
    size_t base_ = 0;
    uint16_t format_version_ = 0;
    uint32_t sector_size_ = 4096;
    std::vector<HashEntry> hash_;
    std::vector<BlockEntry> block_;
};

}
