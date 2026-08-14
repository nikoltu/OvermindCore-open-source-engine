#pragma once

#include <cstdint>
#include <string>

namespace sc2::sim {

class Fixed {
public:
    static constexpr int kShift = 12;
    static constexpr int32_t kOne = 1 << kShift;

    constexpr Fixed() : raw_(0) {}

    static constexpr Fixed from_raw(int32_t raw) {
        Fixed f;
        f.raw_ = raw;
        return f;
    }
    static constexpr Fixed from_int(int32_t v) { return from_raw(v << kShift); }
    static constexpr Fixed from_ratio(int32_t num, int32_t den) {
        return from_raw(int32_t((int64_t(num) << kShift) / den));
    }
    static Fixed from_double(double v) {
        return from_raw(int32_t(v * kOne + (v >= 0 ? 0.5 : -0.5)));
    }

    constexpr int32_t raw() const { return raw_; }

    constexpr int32_t trunc() const { return raw_ / kOne; }
    constexpr int32_t floor() const {
        return (raw_ >= 0) ? raw_ >> kShift : -((-raw_ + kOne - 1) >> kShift);
    }
    constexpr int32_t round() const {
        return from_raw(raw_ + (raw_ >= 0 ? kOne / 2 : -(kOne / 2))).trunc();
    }

    double to_double() const { return double(raw_) / kOne; }

    constexpr Fixed operator+(Fixed o) const { return from_raw(raw_ + o.raw_); }
    constexpr Fixed operator-(Fixed o) const { return from_raw(raw_ - o.raw_); }
    constexpr Fixed operator-() const { return from_raw(-raw_); }
    constexpr Fixed operator*(Fixed o) const {
        return from_raw(int32_t((int64_t(raw_) * o.raw_) >> kShift));
    }
    constexpr Fixed operator/(Fixed o) const {
        return from_raw(int32_t((int64_t(raw_) << kShift) / o.raw_));
    }

    constexpr bool operator==(Fixed o) const { return raw_ == o.raw_; }
    constexpr bool operator!=(Fixed o) const { return raw_ != o.raw_; }
    constexpr bool operator<(Fixed o) const { return raw_ < o.raw_; }
    constexpr bool operator<=(Fixed o) const { return raw_ <= o.raw_; }
    constexpr bool operator>(Fixed o) const { return raw_ > o.raw_; }
    constexpr bool operator>=(Fixed o) const { return raw_ >= o.raw_; }

    static Fixed parse(const std::string& s) {
        size_t i = 0;
        bool neg = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = (s[i++] == '-');
        int64_t int_part = 0;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i)
            int_part = int_part * 10 + (s[i] - '0');
        int64_t raw = int_part << kShift;
        if (i < s.size() && s[i] == '.') {
            ++i;
            int64_t frac = 0, scale = 1;
            for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
                frac = frac * 10 + (s[i] - '0');
                scale *= 10;
            }
            raw += (frac * kOne + scale / 2) / scale;
        }
        return from_raw(int32_t(neg ? -raw : raw));
    }

private:
    int32_t raw_;
};

inline Fixed isqrt(Fixed v) {
    if (v.raw() <= 0) return Fixed();
    uint64_t n = uint64_t(v.raw()) << Fixed::kShift;
    uint64_t x = n, res = 0, bit = uint64_t(1) << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return Fixed::from_raw(int32_t(res));
}

namespace trig_detail {
inline constexpr int32_t kAtan[13] = {3217, 1899, 1003, 509, 256, 128, 64,
                                      32,   16,   8,    4,   2,   1};
inline constexpr int32_t kPi = 12868;
inline constexpr int32_t kHalfPi = 6434;
inline constexpr int32_t kTwoPi = 25736;
inline constexpr int32_t kInvGain = 2487;
inline constexpr int32_t asr(int32_t v, int i) { return v >= 0 ? (v >> i) : ~((~v) >> i); }
}

inline void fixed_sincos(Fixed a, Fixed& cos_out, Fixed& sin_out) {
    using namespace trig_detail;
    int32_t z = a.raw() % kTwoPi;
    if (z > kPi) z -= kTwoPi;
    else if (z < -kPi) z += kTwoPi;
    bool flip = false;
    if (z > kHalfPi) { z -= kPi; flip = true; }
    else if (z < -kHalfPi) { z += kPi; flip = true; }
    int32_t x = kInvGain, y = 0;
    for (int i = 0; i < 13; ++i) {
        const int32_t dx = asr(x, i), dy = asr(y, i);
        if (z >= 0) { x -= dy; y += dx; z -= kAtan[i]; }
        else        { x += dy; y -= dx; z += kAtan[i]; }
    }
    if (flip) { x = -x; y = -y; }
    cos_out = Fixed::from_raw(x);
    sin_out = Fixed::from_raw(y);
}
inline Fixed fixed_sin(Fixed a) { Fixed c, s; fixed_sincos(a, c, s); return s; }
inline Fixed fixed_cos(Fixed a) { Fixed c, s; fixed_sincos(a, c, s); return c; }

inline Fixed fixed_atan2(Fixed y_in, Fixed x_in) {
    using namespace trig_detail;
    int32_t x = x_in.raw(), y = y_in.raw();
    if (x == 0 && y == 0) return Fixed();
    int32_t add = 0;
    if (x < 0) {
        add = (y >= 0) ? kPi : -kPi;
        x = -x;
        y = -y;
    }
    int32_t z = 0;
    for (int i = 0; i < 13; ++i) {
        const int32_t dx = asr(x, i), dy = asr(y, i);
        if (y >= 0) { x += dy; y -= dx; z += kAtan[i]; }
        else        { x -= dy; y += dx; z -= kAtan[i]; }
    }
    return Fixed::from_raw(z + add);
}

}
