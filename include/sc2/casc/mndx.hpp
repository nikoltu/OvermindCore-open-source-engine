#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sc2/casc/encoding.hpp"

namespace sc2::casc {

class Mndx {
public:
    void parse(const std::vector<uint8_t>& root_blob);

    class Range {
    public:
        class iterator {
        public:
            using value_type = std::pair<std::string, CKey>;
            using reference = const value_type&;
            iterator(const Mndx* m, std::size_t i) : m_(m), i_(i) { load(); }
            reference operator*() const { return cur_; }
            const value_type* operator->() const { return &cur_; }
            iterator& operator++() { ++i_; load(); return *this; }
            bool operator==(const iterator& o) const { return i_ == o.i_; }
            bool operator!=(const iterator& o) const { return i_ != o.i_; }

        private:
            void load();
            const Mndx* m_;
            std::size_t i_;
            std::size_t pos_ = 0;
            value_type cur_;
        };
        explicit Range(const Mndx* m) : m_(m) {}
        iterator begin() const { return iterator(m_, 0); }
        iterator end() const { return iterator(m_, m_->size()); }
        std::size_t size() const { return m_->size(); }
        std::pair<std::string, CKey> operator[](std::size_t i) const { return {m_->name(i), m_->key(i)}; }

    private:
        const Mndx* m_;
    };

    Range enumerate() const { return Range(this); }
    std::size_t size() const { return fc_count_ ? fc_count_ : entries_.size(); }
    std::string name(std::size_t i) const;
    const CKey& key(std::size_t i) const;

    const CKey* resolve(const std::string& path) const;

    std::size_t memory_bytes() const;

private:
    friend class Range::iterator;
    static constexpr std::size_t kRestart = 16;
    std::vector<std::pair<std::string, CKey>> entries_;
    std::vector<std::uint8_t> fc_;
    std::vector<std::uint32_t> restart_;
    std::vector<std::pair<std::uint32_t, CKey>> first_ck_;
    std::size_t fc_count_ = 0;
    std::unordered_map<std::string, CKey> by_path_;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> by_hash_;
    std::vector<CKey> resolve_ck_;
};

inline std::uint64_t fnv1a64(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

}
