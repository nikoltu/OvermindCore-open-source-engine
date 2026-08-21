#pragma once

#include <array>
#include <cmath>

namespace sc2::render {

struct Mat4 {
    std::array<float, 16> m{};

    static Mat4 identity() {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    friend Mat4 operator*(const Mat4& a, const Mat4& b) {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
                r.m[c * 4 + row] = s;
            }
        return r;
    }
};

inline Mat4 translation(float x, float y, float z) {
    Mat4 r = Mat4::identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

inline Mat4 scaling(float x, float y, float z) {
    Mat4 r = Mat4::identity();
    r.m[0] = x; r.m[5] = y; r.m[10] = z;
    return r;
}

inline Mat4 rotation_z(float rad) {
    Mat4 r = Mat4::identity();
    float c = std::cos(rad), s = std::sin(rad);
    r.m[0] = c;  r.m[1] = s;
    r.m[4] = -s; r.m[5] = c;
    return r;
}

inline Mat4 from_quat(float x, float y, float z, float w) {
    float n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n < 1e-8f) return Mat4::identity();
    x /= n; y /= n; z /= n; w /= n;
    Mat4 r = Mat4::identity();
    r.m[0] = 1 - 2 * (y * y + z * z);
    r.m[1] = 2 * (x * y + z * w);
    r.m[2] = 2 * (x * z - y * w);
    r.m[4] = 2 * (x * y - z * w);
    r.m[5] = 1 - 2 * (x * x + z * z);
    r.m[6] = 2 * (y * z + x * w);
    r.m[8] = 2 * (x * z + y * w);
    r.m[9] = 2 * (y * z - x * w);
    r.m[10] = 1 - 2 * (x * x + y * y);
    return r;
}

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 normalize(const Vec3& a) {
    float l = std::sqrt(dot(a, a));
    if (l < 1e-8f) return {0, 0, 0};
    return {a.x / l, a.y / l, a.z / l};
}

inline Mat4 look_at(const Vec3& eye, const Vec3& center, const Vec3& up) {
    Vec3 f = normalize(sub(center, eye));
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 r = Mat4::identity();
    r.m[0] = s.x;  r.m[4] = s.y;  r.m[8] = s.z;
    r.m[1] = u.x;  r.m[5] = u.y;  r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    return r;
}

inline Mat4 ortho(float l, float r, float b, float t, float znear, float zfar) {
    Mat4 m;
    m.m[0] = 2.0f / (r - l);
    m.m[5] = 2.0f / (t - b);
    m.m[10] = -1.0f / (zfar - znear);
    m.m[12] = -(r + l) / (r - l);
    m.m[13] = -(t + b) / (t - b);
    m.m[14] = -znear / (zfar - znear);
    m.m[15] = 1.0f;
    return m;
}

inline Mat4 perspective(float fovy, float aspect, float znear, float zfar) {
    float t = std::tan(fovy * 0.5f);
    Mat4 r;
    r.m[0] = 1.0f / (aspect * t);
    r.m[5] = -1.0f / t;
    r.m[10] = zfar / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (znear * zfar) / (znear - zfar);
    return r;
}

}
