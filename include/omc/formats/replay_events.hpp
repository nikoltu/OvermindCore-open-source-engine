#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace omc::formats {

inline constexpr uint32_t kReplayProtocolBuild = 97563;

struct ReplayVersion {
    uint32_t flags = 0;
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t revision = 0;
    uint32_t build = 0;
    uint32_t base_build = 0;
    uint32_t elapsed_game_loops = 0;
};

class ReplayValue {
public:
    enum class Kind { Null, Int, Bytes, Bool, Array, Struct };

    ReplayValue() = default;

    Kind kind() const { return kind_; }
    bool is_null() const { return kind_ == Kind::Null; }

    int64_t as_int() const { return int_; }
    bool as_bool() const { return int_ != 0; }
    const std::string& bytes() const { return bytes_; }

    const std::vector<ReplayValue>& items() const { return items_; }
    const std::vector<std::pair<std::string, ReplayValue>>& fields() const { return fields_; }

    const ReplayValue& field(const std::string& name) const;

    static ReplayValue make_int(int64_t v);
    static ReplayValue make_bool(bool v);
    static ReplayValue make_bytes(std::string v);
    static ReplayValue make_bits(int64_t bit_count, std::string v);
    static ReplayValue make_array(std::vector<ReplayValue> v);
    static ReplayValue make_struct(std::vector<std::pair<std::string, ReplayValue>> v);

    void add_field(std::string name, ReplayValue v);

private:
    Kind kind_ = Kind::Null;
    int64_t int_ = 0;
    std::string bytes_;
    std::vector<ReplayValue> items_;
    std::vector<std::pair<std::string, ReplayValue>> fields_;
};

struct ReplayEvent {
    uint32_t gameloop = 0;
    uint32_t user_id = 0;
    uint32_t event_id = 0;
    std::string name;
    ReplayValue data;
    uint64_t bit_offset = 0;
    uint64_t bit_length = 0;
};

ReplayVersion decode_replay_version(const std::vector<uint8_t>& replay_image);

void require_protocol_build(const ReplayVersion& version);

ReplayValue decode_versioned(const std::vector<uint8_t>& bytes, uint16_t type_id);

ReplayValue decode_replay_details(const std::vector<uint8_t>& bytes);

std::vector<ReplayEvent> decode_game_events(const std::vector<uint8_t>& stream);

std::string game_event_name(uint32_t event_id);

}
