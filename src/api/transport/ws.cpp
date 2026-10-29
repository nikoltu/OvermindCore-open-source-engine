#include "omc/api/ws.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace omc::api::ws {

namespace {

constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct Sha1 {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::uint8_t block[64] = {};
    std::size_t block_len = 0;
    std::uint64_t total_bits = 0;

    static std::uint32_t rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void compress() {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(block[i * 4]) << 24) | (std::uint32_t(block[i * 4 + 1]) << 16) |
                   (std::uint32_t(block[i * 4 + 2]) << 8) | std::uint32_t(block[i * 4 + 3]);
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    void update(const std::uint8_t* p, std::size_t n) {
        total_bits += std::uint64_t(n) * 8u;
        while (n > 0) {
            const std::size_t take = std::min(n, std::size_t(64) - block_len);
            std::memcpy(block + block_len, p, take);
            block_len += take;
            p += take;
            n -= take;
            if (block_len == 64) {
                compress();
                block_len = 0;
            }
        }
    }

    void finish(std::uint8_t out[20]) {
        const std::uint64_t bits = total_bits;
        const std::uint8_t one = 0x80;
        update(&one, 1);
        const std::uint8_t zero = 0;
        while (block_len != 56) update(&zero, 1);
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = std::uint8_t((bits >> (56 - i * 8)) & 0xFFu);
        total_bits = bits;
        std::memcpy(block + block_len, len, 8);
        block_len = 64;
        compress();
        block_len = 0;
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = std::uint8_t((h[i] >> (24 - j * 8)) & 0xFFu);
    }
};

std::string base64(const std::uint8_t* p, std::size_t n) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        const std::uint32_t a = p[i];
        const std::uint32_t b = (i + 1 < n) ? p[i + 1] : 0u;
        const std::uint32_t c = (i + 2 < n) ? p[i + 2] : 0u;
        const std::uint32_t v = (a << 16) | (b << 8) | c;
        out.push_back(kAlphabet[(v >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(v >> 12) & 0x3Fu]);
        out.push_back(i + 1 < n ? kAlphabet[(v >> 6) & 0x3Fu] : '=');
        out.push_back(i + 2 < n ? kAlphabet[v & 0x3Fu] : '=');
    }
    return out;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void write_length(std::vector<std::uint8_t>& out, std::size_t n) {
    if (n < 126) {
        out.push_back(std::uint8_t(n));
    } else if (n <= 0xFFFFu) {
        out.push_back(126);
        out.push_back(std::uint8_t((n >> 8) & 0xFFu));
        out.push_back(std::uint8_t(n & 0xFFu));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i) out.push_back(std::uint8_t((std::uint64_t(n) >> (i * 8)) & 0xFFu));
    }
}

}

std::string accept_key(const std::string& client_key) {
    Sha1 s;
    s.update(reinterpret_cast<const std::uint8_t*>(client_key.data()), client_key.size());
    s.update(reinterpret_cast<const std::uint8_t*>(kGuid), std::strlen(kGuid));
    std::uint8_t digest[20];
    s.finish(digest);
    return base64(digest, sizeof digest);
}

bool handshake_response(const std::string& request_head, std::string& response, std::string& error) {
    std::string key, upgrade, connection, version;
    std::size_t pos = 0;
    bool first = true;
    while (pos < request_head.size()) {
        std::size_t eol = request_head.find("\r\n", pos);
        if (eol == std::string::npos) eol = request_head.size();
        const std::string line = request_head.substr(pos, eol - pos);
        pos = eol + 2;
        if (first) {
            first = false;
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (name == "sec-websocket-key") key = value;
        else if (name == "upgrade") upgrade = lower(value);
        else if (name == "connection") connection = lower(value);
        else if (name == "sec-websocket-version") version = value;
    }
    if (upgrade.find("websocket") == std::string::npos) {
        error = "no `Upgrade: websocket` header in the request";
        return false;
    }
    if (connection.find("upgrade") == std::string::npos) {
        error = "no `Connection: Upgrade` header in the request";
        return false;
    }
    if (key.empty()) {
        error = "no Sec-WebSocket-Key header in the request";
        return false;
    }
    if (!version.empty() && version != "13") {
        error = "unsupported Sec-WebSocket-Version " + version + " (this endpoint speaks 13)";
        return false;
    }
    response = "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: " + accept_key(key) + "\r\n\r\n";
    return true;
}

std::vector<std::uint8_t> encode_binary(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    out.reserve(payload.size() + 10);
    out.push_back(std::uint8_t(0x80u | std::uint8_t(Op::Binary)));
    write_length(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> encode_control(std::uint8_t opcode,
                                         const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    out.push_back(std::uint8_t(0x80u | opcode));
    write_length(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Decode decode(const std::uint8_t* buf, std::size_t len, Frame& out, std::size_t& consumed) {
    if (len < 2) return Decode::NeedMore;
    const std::uint8_t b0 = buf[0];
    const std::uint8_t b1 = buf[1];
    if ((b0 & 0x70u) != 0) return Decode::Protocol;
    out.fin = (b0 & 0x80u) != 0;
    out.op = Op(b0 & 0x0Fu);
    out.masked = (b1 & 0x80u) != 0;
    std::uint64_t n = b1 & 0x7Fu;
    std::size_t at = 2;
    if (n == 126) {
        if (len < at + 2) return Decode::NeedMore;
        n = (std::uint64_t(buf[at]) << 8) | buf[at + 1];
        at += 2;
    } else if (n == 127) {
        if (len < at + 8) return Decode::NeedMore;
        n = 0;
        for (int i = 0; i < 8; ++i) n = (n << 8) | buf[at + std::size_t(i)];
        at += 8;
    }
    const bool control = (std::uint8_t(out.op) & 0x08u) != 0;
    if (control && (n > 125 || !out.fin)) return Decode::Protocol;
    if (!out.masked) return Decode::Protocol;
    if (len < at + 4) return Decode::NeedMore;
    std::uint8_t mask[4];
    std::memcpy(mask, buf + at, 4);
    at += 4;
    if (len < at + n) return Decode::NeedMore;
    out.payload.resize(std::size_t(n));
    for (std::size_t i = 0; i < std::size_t(n); ++i) out.payload[i] = buf[at + i] ^ mask[i & 3u];
    consumed = at + std::size_t(n);
    return Decode::Ok;
}

}
