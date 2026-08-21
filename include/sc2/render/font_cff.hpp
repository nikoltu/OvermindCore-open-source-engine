#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace sc2::render {

class CffFont {
public:
    using Contour = std::vector<std::pair<float, float>>;

    bool parse(const std::uint8_t* data, std::size_t n, std::uint32_t cff_off, std::uint32_t cff_len);
    bool valid() const { return valid_; }

    int num_glyphs() const { return int(charstrings_.count()); }

    void glyph_contours(int gid, std::vector<Contour>& out) const;

private:
    struct Index {
        std::vector<std::uint32_t> offsets;
        std::size_t count() const { return offsets.empty() ? 0 : offsets.size() - 1; }
        std::pair<std::size_t, std::size_t> span(std::size_t i) const {
            if (i + 1 >= offsets.size()) return {0, 0};
            return {offsets[i], offsets[i + 1]};
        }
    };

    bool read_index(std::size_t pos, Index& out, std::size_t& end) const;
    static int bias_for(std::size_t n);
    const Index& local_subrs(int gid, int& bias) const;

    struct Interp;
    void run_charstring(Interp& st, std::size_t begin, std::size_t end, int depth) const;

    std::vector<std::uint8_t> buf_;
    Index charstrings_;
    Index gsubrs_;
    int gbias_ = 0;

    Index lsubrs_;
    int lbias_ = 0;
    bool is_cid_ = false;
    std::vector<Index> fd_lsubrs_;
    std::vector<int> fd_lbias_;
    std::vector<std::uint8_t> fdselect_;

    bool valid_ = false;
};

}
