#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace omc::core {

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    explicit ByteReader(const std::vector<uint8_t>& v)
        : data_(v.data()), size_(v.size()) {}

    size_t pos() const { return pos_; }
    size_t size() const { return size_; }
    size_t remaining() const { return size_ - pos_; }
    const uint8_t* ptr() const { return data_ + pos_; }

    void seek(size_t p) {
        if (p > size_) fail("seek past end");
        pos_ = p;
    }
    void skip(size_t n) {
        need(n);
        pos_ += n;
    }

    uint8_t u8() {
        need(1);
        return data_[pos_++];
    }

    uint16_t u16le() {
        need(2);
        uint16_t v = uint16_t(data_[pos_]) | uint16_t(data_[pos_ + 1]) << 8;
        pos_ += 2;
        return v;
    }
    uint32_t u32le() {
        need(4);
        uint32_t v = uint32_t(data_[pos_]) | uint32_t(data_[pos_ + 1]) << 8 |
                     uint32_t(data_[pos_ + 2]) << 16 |
                     uint32_t(data_[pos_ + 3]) << 24;
        pos_ += 4;
        return v;
    }

    uint16_t u16be() {
        need(2);
        uint16_t v = uint16_t(data_[pos_]) << 8 | uint16_t(data_[pos_ + 1]);
        pos_ += 2;
        return v;
    }
    uint32_t u24be() {
        need(3);
        uint32_t v = uint32_t(data_[pos_]) << 16 |
                     uint32_t(data_[pos_ + 1]) << 8 | uint32_t(data_[pos_ + 2]);
        pos_ += 3;
        return v;
    }
    uint32_t u32be() {
        need(4);
        uint32_t v = uint32_t(data_[pos_]) << 24 |
                     uint32_t(data_[pos_ + 1]) << 16 |
                     uint32_t(data_[pos_ + 2]) << 8 | uint32_t(data_[pos_ + 3]);
        pos_ += 4;
        return v;
    }

    uint64_t uint_be(size_t n) {
        need(n);
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v = (v << 8) | data_[pos_ + i];
        pos_ += n;
        return v;
    }
    uint64_t uint_le(size_t n) {
        need(n);
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v |= uint64_t(data_[pos_ + i]) << (8 * i);
        pos_ += n;
        return v;
    }

    std::vector<uint8_t> bytes(size_t n) {
        need(n);
        std::vector<uint8_t> out(data_ + pos_, data_ + pos_ + n);
        pos_ += n;
        return out;
    }

private:
    void need(size_t n) const {
        if (pos_ + n > size_) fail("read past end");
    }
    [[noreturn]] static void fail(const char* what) {
        throw std::runtime_error(std::string("ByteReader: ") + what);
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

}
