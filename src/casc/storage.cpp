#include "omc/casc/storage.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "omc/casc/blte.hpp"
#include "omc/platform/file.hpp"

namespace fs = std::filesystem;

namespace omc::casc {

static constexpr size_t kBlobHeaderSize = 30;

static std::string nth_hex(const std::string& text, const std::string& key,
                           int n) {
    auto pos = text.find(key);
    if (pos == std::string::npos) return {};
    pos += key.size();
    std::vector<std::string> toks;
    std::string cur;
    while (pos < text.size() && text[pos] != '\n') {
        char c = text[pos++];
        if (std::isxdigit((unsigned char)c)) {
            cur += c;
        } else if (!cur.empty()) {
            toks.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    return (n >= 0 && size_t(n) < toks.size()) ? toks[n] : std::string{};
}

void Storage::load_build_config(const std::string& install_root) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(install_root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        auto sz = it->file_size(ec);
        if (ec || sz == 0 || sz > 8192) continue;
        std::string name = it->path().filename().string();
        if (name.size() != 32) continue;
        std::ifstream f(it->path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), {});
        if (text.find("root =") == std::string::npos ||
            text.find("encoding =") == std::string::npos)
            continue;
        root_ckey_hex_ = nth_hex(text, "root =", 0);
        encoding_ekey_hex_ = nth_hex(text, "encoding =", 1);
        if (!root_ckey_hex_.empty() && !encoding_ekey_hex_.empty()) return;
    }
}

void Storage::open(const std::string& data_dir, const std::string& install_root) {
    data_dir_ = data_dir;
    idx_.build(data_dir);

    std::string root = install_root;
    if (root.empty()) root = fs::path(data_dir).parent_path().string();
    load_build_config(root);

    if (!encoding_ekey_hex_.empty()) {
        auto enc_blob = read_by_ekey_hex(encoding_ekey_hex_);
        encoding_.parse(enc_blob);
        has_encoding_ = true;
    }

    if (has_encoding_ && !root_ckey_hex_.empty()) {
        auto root_blob = read_root();
        if (root_blob.size() >= 4 && std::memcmp(root_blob.data(), "MNDX", 4) == 0) {
            mndx_.parse(root_blob);
            has_mndx_ = true;
        }
    }
}

std::string Storage::data_file_path(uint32_t file_index) const {
    char name[32];
    std::snprintf(name, sizeof(name), "data.%03u", file_index);
    return data_dir_ + "/" + name;
}

std::vector<uint8_t> Storage::read_blte_stream(const Location& loc) const {
    auto blob = platform::read_file_at(data_file_path(loc.file_index), loc.offset,
                                       loc.size);
    auto is_blte = [&](size_t at) {
        return blob.size() >= at + 4 && blob[at] == 'B' && blob[at + 1] == 'L' &&
               blob[at + 2] == 'T' && blob[at + 3] == 'E';
    };
    size_t start;
    if (is_blte(kBlobHeaderSize))
        start = kBlobHeaderSize;
    else if (is_blte(0))
        start = 0;
    else
        throw std::runtime_error("storage: no BLTE stream in blob");
    return std::vector<uint8_t>(blob.begin() + start, blob.end());
}

std::vector<uint8_t> Storage::read_by_ekey(const EKey9& ekey) const {
    const Location* loc = idx_.find(ekey);
    if (!loc) throw std::runtime_error("storage: EKey not in index");
    return blte_decode(read_blte_stream(*loc));
}

std::vector<uint8_t> Storage::read_by_ekey_hex(const std::string& hex) const {
    return read_by_ekey(ekey9_from_hex(hex));
}

std::vector<uint8_t> Storage::read_by_ckey(const CKey& ckey) const {
    if (!has_encoding_) throw std::runtime_error("storage: encoding not loaded");
    const EKey* ek = encoding_.find(ckey);
    if (!ek) throw std::runtime_error("storage: CKey not in encoding");
    EKey9 k9;
    for (int i = 0; i < 9; ++i) k9[i] = (*ek)[i];
    return read_by_ekey(k9);
}

std::vector<uint8_t> Storage::read_by_ckey_hex(const std::string& hex) const {
    return read_by_ckey(Encoding::key_from_hex(hex));
}

namespace {
std::string to_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
}

void Storage::set_overlay(std::unordered_map<std::string, std::vector<uint8_t>> files) {
    std::lock_guard<std::mutex> lk(names_lock_.m);
    overlay_ = std::move(files);
    if (std::getenv("OMC_MAP_ASSETS_TRACE")) std::fprintf(stderr, "[mapassets] overlay set, %zu files\n", overlay_.size());
    overlay_names_.clear();
}

const std::vector<uint8_t>* Storage::overlay_find(const std::string& path, std::string* key) const {
    if (overlay_.empty()) return nullptr;
    std::string p = to_lower(path);
    for (char& c : p)
        if (c == '\\') c = '/';
    std::size_t at = p.rfind("/assets/");
    if (at != std::string::npos) p = p.substr(at + 1);
    else if (p.compare(0, 7, "assets/") != 0) return nullptr;
    auto it = overlay_.find(p);
    if (it == overlay_.end()) return nullptr;
    if (key) *key = p;
    return &it->second;
}

const std::string* Storage::resolve_name_ci(const std::string& path) const {
    std::lock_guard<std::mutex> lk(names_lock_.m);
    return resolve_name_ci_locked(path);
}

const std::string* Storage::resolve_name_ci_locked(const std::string& path) const {
    {
        std::string key;
        if (overlay_find(path, &key)) {
            auto ins = overlay_names_.insert_or_assign(key, path);
            return &ins.first->second;
        }
    }
    if (!has_mndx_) return nullptr;
    if (!casc_index_maps_old()) {
        if (!ci_built_) {
            ci_hash_.resize(mndx_.size());
            std::size_t i = 0;
            for (const auto& entry : mndx_.enumerate()) ci_hash_[i] = {fnv1a64(to_lower(entry.first)), std::uint32_t(i)}, ++i;
            std::stable_sort(ci_hash_.begin(), ci_hash_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            ci_built_ = true;
        }
        const std::string lp = to_lower(path);
        const std::uint64_t h = fnv1a64(lp);
        auto it = std::lower_bound(ci_hash_.begin(), ci_hash_.end(), h, [](const auto& e, std::uint64_t k) { return e.first < k; });
        for (; it != ci_hash_.end() && it->first == h; ++it) {
            std::string canon = mndx_.name(it->second);
            if (to_lower(canon) == lp) return &ci_hits_.emplace(it->second, std::move(canon)).first->second;
        }
        return nullptr;
    }
    if (!ci_built_) {
        for (const auto& entry : mndx_.enumerate())
            ci_names_.emplace(to_lower(entry.first), entry.first);
        ci_built_ = true;
    }
    auto it = ci_names_.find(to_lower(path));
    return it == ci_names_.end() ? nullptr : &it->second;
}

std::size_t Storage::memory_bytes(std::size_t* idx, std::size_t* enc, std::size_t* names, std::size_t* ci) const {
    const std::size_t a = idx_.memory_bytes(), b = encoding_.memory_bytes(), c = mndx_.memory_bytes();
    std::lock_guard<std::mutex> lk(names_lock_.m);
    std::size_t d = ci_hash_.capacity() * sizeof(ci_hash_[0]);
    for (const auto& kv : ci_names_) d += 64 + kv.first.capacity() + kv.second.capacity();
    for (const auto& kv : ci_hits_) d += 64 + kv.second.capacity();
    if (idx) *idx = a;
    if (enc) *enc = b;
    if (names) *names = c;
    if (ci) *ci = d;
    return a + b + c + d;
}

std::vector<uint8_t> Storage::read_by_name(const std::string& path) const {
    CKey key{};
    {
        std::lock_guard<std::mutex> lk(names_lock_.m);
        if (const std::vector<uint8_t>* ov = overlay_find(path)) {
            if (std::getenv("OMC_MAP_ASSETS_TRACE")) std::fprintf(stderr, "[mapassets] read %s from the map\n", path.c_str());
            return *ov;
        }
        if (std::getenv("OMC_MAP_ASSETS_TRACE") && to_lower(path).find(".m3") != std::string::npos)
            std::fprintf(stderr, "[mapassets] read %s from the stores\n", path.c_str());
        if (!has_mndx_) throw std::runtime_error("storage: MNDX names not loaded");
        const CKey* ck = mndx_.resolve(path);
        if (!ck) {
            if (const std::string* canon = resolve_name_ci_locked(path))
                ck = mndx_.resolve(*canon);
        }
        if (!ck) throw std::runtime_error("storage: name not in MNDX root");
        key = *ck;
    }
    return read_by_ckey(key);
}

std::vector<uint8_t> Storage::read_root() const {
    if (root_ckey_hex_.empty()) throw std::runtime_error("storage: no root ckey");
    return read_by_ckey_hex(root_ckey_hex_);
}

}
