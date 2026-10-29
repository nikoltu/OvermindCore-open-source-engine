#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "omc/app/replay.hpp"
#include "omc/formats/replay_events.hpp"

namespace omc::app {

enum class EventBucket { Translated, Inert, Refused, Unlisted };

struct EventRow {
    const char* name;
    EventBucket bucket;
    const char* why;
};

const std::vector<EventRow>& replay_event_table();

EventBucket replay_event_bucket(const std::string& name);

std::string replay_event_reason(const std::string& name);

std::vector<std::string> replay_events_in_bucket(EventBucket bucket);

const char* replay_event_bucket_name(EventBucket bucket);

class ReplayRefusal : public std::runtime_error {
public:
    explicit ReplayRefusal(const std::string& what) : std::runtime_error(what) {}
};

enum class ReplayCommandKind : uint8_t {
    Cmd = 0,
    UpdateTargetPoint = 1,
    UpdateTargetUnit = 2,
    ManagerState = 3,
};

enum class ReplayTargetKind : uint8_t { None = 0, Point = 1, Unit = 2, Data = 3 };

struct ReplayCommand {
    uint32_t gameloop = 0;
    uint32_t user_id = 0;
    ReplayCommandKind kind = ReplayCommandKind::Cmd;

    uint32_t cmd_flags = 0;

    bool has_abil = false;
    uint32_t abil_link = 0;
    uint32_t abil_cmd_index = 0;
    bool has_abil_cmd_data = false;
    uint32_t abil_cmd_data = 0;

    ReplayTargetKind target = ReplayTargetKind::None;
    int64_t target_x = 0;
    int64_t target_y = 0;
    int64_t target_z = 0;
    uint32_t target_unit_tag = 0;
    uint32_t target_unit_flags = 0;
    uint32_t target_unit_link = 0;
    uint32_t target_data = 0;

    bool has_sequence = false;
    uint32_t sequence = 0;

    uint32_t manager_state = 0;
    std::vector<uint32_t> actor_tags;
    bool has_other_unit = false;
    uint32_t other_unit_tag = 0;
    bool has_unit_group = false;
    uint32_t unit_group = 0;

    bool operator==(const ReplayCommand& o) const;
    bool operator!=(const ReplayCommand& o) const { return !(*this == o); }
};

std::vector<ReplayCommand> translate_commands(const std::vector<formats::ReplayEvent>& events,
                                              size_t* inert_count = nullptr);

ReplayCommand translate_command(const formats::ReplayEvent& event);

inline constexpr char kReplayCommandLogMagic[8] = {'S', 'C', '2', 'R', 'C', 'M', 'D', '\0'};
inline constexpr uint32_t kReplayCommandLogVersion = 2;

void write_replay_commands(const std::vector<ReplayCommand>& commands, std::vector<uint8_t>& out);
bool read_replay_commands(const uint8_t* data, size_t len, std::vector<ReplayCommand>& out);
bool write_replay_command_file(const std::vector<ReplayCommand>& commands, const std::string& path);
bool read_replay_command_file(const std::string& path, std::vector<ReplayCommand>& out);

struct OrdersOutcome {
    bool ok = false;
    std::vector<std::string> blockers;
};

OrdersOutcome events_to_orders(const std::vector<ReplayCommand>& commands, OrderLog& out);

}
