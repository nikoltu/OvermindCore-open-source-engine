#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace omc::api::ws {

std::string accept_key(const std::string& client_key);

bool handshake_response(const std::string& request_head, std::string& response, std::string& error);

std::vector<std::uint8_t> encode_binary(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_control(std::uint8_t opcode, const std::vector<std::uint8_t>& payload);

enum class Op : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

struct Frame {
    bool fin = false;
    Op op = Op::Continuation;
    bool masked = false;
    std::vector<std::uint8_t> payload;
};

enum class Decode {
    Ok,
    NeedMore,
    Protocol,
};

Decode decode(const std::uint8_t* buf, std::size_t len, Frame& out, std::size_t& consumed);

inline constexpr std::uint16_t kProtocolError = 1002;

}
