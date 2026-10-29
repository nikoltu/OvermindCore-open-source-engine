#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "omc/casc/encoding.hpp"
#include "omc/casc/idx.hpp"
#include "omc/casc/mndx.hpp"

namespace omc::casc {

class Storage {
public:
    void open(const std::string& data_dir, const std::string& install_root = "");

    const IdxIndex& index() const { return idx_; }
    const Encoding& encoding() const { return encoding_; }
    bool has_encoding() const { return has_encoding_; }

    const Mndx& mndx() const { return mndx_; }
    bool has_mndx() const { return has_mndx_; }

    const std::string& root_ckey_hex() const { return root_ckey_hex_; }
    const std::string& encoding_ekey_hex() const { return encoding_ekey_hex_; }

    std::vector<uint8_t> read_by_ekey(const EKey9& ekey) const;
    std::vector<uint8_t> read_by_ekey_hex(const std::string& hex) const;

    std::vector<uint8_t> read_by_ckey(const CKey& ckey) const;
    std::vector<uint8_t> read_by_ckey_hex(const std::string& hex) const;

    std::vector<uint8_t> read_by_name(const std::string& path) const;

    const std::string* resolve_name_ci(const std::string& path) const;

    void set_overlay(std::unordered_map<std::string, std::vector<uint8_t>> files);
    std::size_t overlay_size() const { return overlay_.size(); }

    std::size_t memory_bytes(std::size_t* idx, std::size_t* enc, std::size_t* names, std::size_t* ci) const;

    std::vector<uint8_t> read_root() const;

    std::vector<uint8_t> read_blte_stream(const Location& loc) const;

private:
    std::string data_dir_;
    IdxIndex idx_;
    Encoding encoding_;
    bool has_encoding_ = false;
    Mndx mndx_;
    bool has_mndx_ = false;
    struct NameLock {
        std::mutex m;
        NameLock() = default;
        NameLock(const NameLock&) {}
        NameLock& operator=(const NameLock&) { return *this; }
    };
    mutable NameLock names_lock_;
    mutable std::unordered_map<std::string, std::string> ci_names_;
    mutable std::vector<std::pair<std::uint64_t, std::uint32_t>> ci_hash_;
    mutable std::unordered_map<std::uint32_t, std::string> ci_hits_;
    mutable bool ci_built_ = false;
    std::unordered_map<std::string, std::vector<uint8_t>> overlay_;
    mutable std::unordered_map<std::string, std::string> overlay_names_;
    const std::vector<uint8_t>* overlay_find(const std::string& path, std::string* key = nullptr) const;
    const std::string* resolve_name_ci_locked(const std::string& path) const;
    std::string root_ckey_hex_;
    std::string encoding_ekey_hex_;

    std::string data_file_path(uint32_t file_index) const;
    void load_build_config(const std::string& install_root);
};

}
