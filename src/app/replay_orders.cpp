#include "sc2/app/replay_orders.hpp"

#include <cstring>
#include <fstream>
#include <map>

namespace sc2::app {
namespace {

using formats::ReplayValue;

const EventRow kEventTable[] = {
    {"NNet.Game.SCmdEvent", EventBucket::Translated, ""},
    {"NNet.Game.SCmdUpdateTargetPointEvent", EventBucket::Translated, ""},
    {"NNet.Game.SCmdUpdateTargetUnitEvent", EventBucket::Translated, ""},
    {"NNet.Game.SCommandManagerStateEvent", EventBucket::Translated, ""},

    {"NNet.Game.SUserFinishedLoadingSyncEvent", EventBucket::Inert,
     "loading handshake; no sim state and no command"},
    {"NNet.Game.SUserOptionsEvent", EventBucket::Inert,
     "per-user client options; nothing in the sim reads them"},
    {"NNet.Game.SCameraSaveEvent", EventBucket::Inert, "camera bookmark; the camera is not sim state"},
    {"NNet.Game.SCameraUpdateEvent", EventBucket::Inert, "camera motion; the camera is not sim state"},
    {"NNet.Game.SSelectionDeltaEvent", EventBucket::Inert,
     "emits no command; maintains the user's selection, which the translator tracks as the acting units of "
     "every later SCmdEvent (a command names no units of its own)"},
    {"NNet.Game.SControlGroupUpdateEvent", EventBucket::Inert,
     "emits no command; maintains the user's control groups, recalled into the selection the translator tracks"},
    {"NNet.Game.SSelectionSyncCheckEvent", EventBucket::Inert,
     "a lockstep checksum of the selection; carries no intent"},
    {"NNet.Game.SCommandManagerResetEvent", EventBucket::Inert,
     "does not occur anywhere in the corpus, so nothing is measured and nothing is claimed"},
    {"NNet.Game.STriggerChatMessageEvent", EventBucket::Inert, "chat text; no sim state"},
    {"NNet.Game.STriggerPingEvent", EventBucket::Inert, "a map ping; no sim state"},
    {"NNet.Game.SUnitClickEvent", EventBucket::Inert,
     "does not occur anywhere in the corpus, so nothing is measured and nothing is claimed"},
    {"NNet.Game.SUnitHighlightEvent", EventBucket::Inert,
     "does not occur anywhere in the corpus, so nothing is measured and nothing is claimed"},
    {"NNet.Game.SSetAbsoluteGameSpeedEvent", EventBucket::Inert,
     "game speed is a wall-clock rate, not a sim concept; the sim advances by loops"},
    {"NNet.Game.SAddAbsoluteGameSpeedEvent", EventBucket::Inert, "as SSetAbsoluteGameSpeedEvent"},
    {"NNet.Game.SSetSyncLoadingTimeEvent", EventBucket::Inert, "wall-clock loading time; no sim state"},
    {"NNet.Game.SSetSyncPlayingTimeEvent", EventBucket::Inert, "wall-clock playing time; no sim state"},
    {"NNet.Game.SPeerSetSyncLoadingTimeEvent", EventBucket::Inert, "as SSetSyncLoadingTimeEvent"},
    {"NNet.Game.SPeerSetSyncPlayingTimeEvent", EventBucket::Inert, "as SSetSyncPlayingTimeEvent"},
    {"NNet.Game.SGameUserJoinEvent", EventBucket::Inert, "lobby membership; the roster is set up front"},
    {"NNet.Game.SGameUserLeaveEvent", EventBucket::Inert,
     "a participant leaving; the bodies stay in the observation and the match end is decided by the "
     "sim, not by this record"},
    {"NNet.Game.STriggerSoundLengthSyncEvent", EventBucket::Inert,
     "publishes sound hashes and durations only (m_syncInfo = m_soundHash + m_length) with no field "
     "that can address a body, a player or an ability, and two participants of one lockstep match "
     "are measured to disagree about the durations, so the simulation does not read them"},
    {"NNet.Game.SDecrementGameTimeRemainingEvent", EventBucket::Inert,
     "measured at its only two mid-game occurrences to republish no action and to move no compared "
     "column beyond the ordinary per-loop spread, in a run where the same instrument placed 29 "
     "republications on their own loops and none elsewhere; that match ran on to a decided result"},

    {"NNet.Game.SGameCheatEvent", EventBucket::Refused,
     "spawns or alters sim state out of nothing; our sim could only follow by copying retail state, "
     "which destroys the independence the diff exists to provide"},
    {"NNet.Game.SBroadcastCheatEvent", EventBucket::Refused,
     "as SGameCheatEvent, applied to every participant"},
    {"NNet.Game.SCatalogModifyEvent", EventBucket::Refused,
     "mutates the DATA layer under a running match; no counterpart and no honest approximation"},
    {"NNet.Game.SAllianceEvent", EventBucket::Refused, "changes who may shoot whom; no counterpart"},
    {"NNet.Game.SResourceTradeEvent", EventBucket::Refused,
     "moves resources between players; no counterpart"},
    {"NNet.Game.SResourceRequestEvent", EventBucket::Refused,
     "moves resources between players; no counterpart"},
    {"NNet.Game.SResourceRequestFulfillEvent", EventBucket::Refused,
     "moves resources between players; no counterpart"},
    {"NNet.Game.SResourceRequestCancelEvent", EventBucket::Refused,
     "moves resources between players; no counterpart"},
    {"NNet.Game.SHijackReplayGameEvent", EventBucket::Refused,
     "the recording changes hands mid-stream; the loop numbering after it is a different game"},
    {"NNet.Game.STriggerDialogControlEvent", EventBucket::Refused,
     "a dialog control press on a map that carries triggers; what the trigger does is unmeasured, and "
     "both replays carrying it are refused upstream for a missing map, so neither channel can see it"},
};

const char* bucket_name(EventBucket b) {
    switch (b) {
        case EventBucket::Translated: return "TRANSLATED";
        case EventBucket::Inert: return "INERT";
        case EventBucket::Refused: return "REFUSED";
        case EventBucket::Unlisted: break;
    }
    return "UNLISTED";
}

const std::map<std::string, const EventRow*>& index() {
    static const std::map<std::string, const EventRow*> m = [] {
        std::map<std::string, const EventRow*> out;
        for (const EventRow& row : kEventTable) out[row.name] = &row;
        return out;
    }();
    return m;
}

int64_t int_or(const ReplayValue& v, int64_t fallback) {
    return v.kind() == ReplayValue::Kind::Int || v.kind() == ReplayValue::Kind::Bool ? v.as_int()
                                                                                    : fallback;
}

void read_point(const ReplayValue& p, ReplayCommand& c) {
    c.target_x = int_or(p.field("x"), 0);
    c.target_y = int_or(p.field("y"), 0);
    c.target_z = int_or(p.field("z"), 0);
}

void read_target_unit(const ReplayValue& u, ReplayCommand& c) {
    c.target_unit_flags = uint32_t(int_or(u.field("m_targetUnitFlags"), 0));
    c.target_unit_tag = uint32_t(int_or(u.field("m_tag"), 0));
    c.target_unit_link = uint32_t(int_or(u.field("m_snapshotUnitLink"), 0));
    read_point(u.field("m_snapshotPoint"), c);
}

void read_data_choice(const ReplayValue& data, ReplayCommand& c) {
    if (data.kind() != ReplayValue::Kind::Struct || data.fields().empty()) return;
    const std::string& arm = data.fields().front().first;
    const ReplayValue& body = data.fields().front().second;
    if (arm == "TargetPoint") {
        c.target = ReplayTargetKind::Point;
        read_point(body, c);
    } else if (arm == "TargetUnit") {
        c.target = ReplayTargetKind::Unit;
        read_target_unit(body, c);
    } else if (arm == "Data") {
        c.target = ReplayTargetKind::Data;
        c.target_data = uint32_t(int_or(body, 0));
    } else {
        c.target = ReplayTargetKind::None;
    }
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v & 0xFFu));
    out.push_back(uint8_t((v >> 8) & 0xFFu));
    out.push_back(uint8_t((v >> 16) & 0xFFu));
    out.push_back(uint8_t((v >> 24) & 0xFFu));
}

void put64(std::vector<uint8_t>& out, uint64_t v) {
    put32(out, uint32_t(v & 0xFFFFFFFFu));
    put32(out, uint32_t(v >> 32));
}

class Reader {
public:
    Reader(const uint8_t* data, size_t len) : d_(data), n_(len) {}
    bool get32(uint32_t& v) {
        if (n_ - p_ < 4) return false;
        v = uint32_t(d_[p_]) | (uint32_t(d_[p_ + 1]) << 8) | (uint32_t(d_[p_ + 2]) << 16) |
            (uint32_t(d_[p_ + 3]) << 24);
        p_ += 4;
        return true;
    }
    bool get64(uint64_t& v) {
        uint32_t lo = 0;
        uint32_t hi = 0;
        if (!get32(lo) || !get32(hi)) return false;
        v = uint64_t(lo) | (uint64_t(hi) << 32);
        return true;
    }
    bool get8(uint8_t& v) {
        if (n_ - p_ < 1) return false;
        v = d_[p_++];
        return true;
    }
    bool at_end() const { return p_ == n_; }
    size_t left() const { return n_ - p_; }

private:
    const uint8_t* d_;
    size_t n_;
    size_t p_ = 0;
};

}

const std::vector<EventRow>& replay_event_table() {
    static const std::vector<EventRow> rows(std::begin(kEventTable), std::end(kEventTable));
    return rows;
}

const char* replay_event_bucket_name(EventBucket bucket) { return bucket_name(bucket); }

EventBucket replay_event_bucket(const std::string& name) {
    const auto it = index().find(name);
    return it == index().end() ? EventBucket::Unlisted : it->second->bucket;
}

std::string replay_event_reason(const std::string& name) {
    const auto it = index().find(name);
    return it == index().end() ? std::string() : std::string(it->second->why);
}

std::vector<std::string> replay_events_in_bucket(EventBucket bucket) {
    std::vector<std::string> out;
    for (const EventRow& row : kEventTable) {
        if (row.bucket == bucket) out.push_back(row.name);
    }
    return out;
}

bool ReplayCommand::operator==(const ReplayCommand& o) const {
    return gameloop == o.gameloop && user_id == o.user_id && kind == o.kind &&
           cmd_flags == o.cmd_flags && has_abil == o.has_abil && abil_link == o.abil_link &&
           abil_cmd_index == o.abil_cmd_index && has_abil_cmd_data == o.has_abil_cmd_data &&
           abil_cmd_data == o.abil_cmd_data && target == o.target && target_x == o.target_x &&
           target_y == o.target_y && target_z == o.target_z && target_unit_tag == o.target_unit_tag &&
           target_unit_flags == o.target_unit_flags && target_unit_link == o.target_unit_link &&
           target_data == o.target_data && has_sequence == o.has_sequence && sequence == o.sequence &&
           manager_state == o.manager_state && actor_tags == o.actor_tags &&
           has_other_unit == o.has_other_unit && other_unit_tag == o.other_unit_tag &&
           has_unit_group == o.has_unit_group && unit_group == o.unit_group;
}

ReplayCommand translate_command(const formats::ReplayEvent& e) {
    if (replay_event_bucket(e.name) != EventBucket::Translated) {
        throw ReplayRefusal("not_a_command: " + e.name + " at loop " + std::to_string(e.gameloop) +
                            " is not in the TRANSLATED bucket and carries no command to read");
    }
    ReplayCommand c;
    c.gameloop = e.gameloop;
    c.user_id = e.user_id;
    if (e.name == "NNet.Game.SCmdEvent") {
        c.kind = ReplayCommandKind::Cmd;
        c.cmd_flags = uint32_t(int_or(e.data.field("m_cmdFlags"), 0));
        const ReplayValue& abil = e.data.field("m_abil");
        if (!abil.is_null()) {
            c.has_abil = true;
            c.abil_link = uint32_t(int_or(abil.field("m_abilLink"), 0));
            c.abil_cmd_index = uint32_t(int_or(abil.field("m_abilCmdIndex"), 0));
            const ReplayValue& cd = abil.field("m_abilCmdData");
            if (!cd.is_null()) {
                c.has_abil_cmd_data = true;
                c.abil_cmd_data = uint32_t(int_or(cd, 0));
            }
        }
        read_data_choice(e.data.field("m_data"), c);
        const ReplayValue& seq = e.data.field("m_sequence");
        if (!seq.is_null()) {
            c.has_sequence = true;
            c.sequence = uint32_t(int_or(seq, 0));
        }
        const ReplayValue& ou = e.data.field("m_otherUnit");
        if (!ou.is_null()) {
            c.has_other_unit = true;
            c.other_unit_tag = uint32_t(int_or(ou, 0));
        }
        const ReplayValue& ug = e.data.field("m_unitGroup");
        if (!ug.is_null()) {
            c.has_unit_group = true;
            c.unit_group = uint32_t(int_or(ug, 0));
        }
    } else if (e.name == "NNet.Game.SCmdUpdateTargetPointEvent") {
        c.kind = ReplayCommandKind::UpdateTargetPoint;
        c.target = ReplayTargetKind::Point;
        read_point(e.data.field("m_target"), c);
    } else if (e.name == "NNet.Game.SCmdUpdateTargetUnitEvent") {
        c.kind = ReplayCommandKind::UpdateTargetUnit;
        c.target = ReplayTargetKind::Unit;
        read_target_unit(e.data.field("m_target"), c);
    } else {
        c.kind = ReplayCommandKind::ManagerState;
        c.manager_state = uint32_t(int_or(e.data.field("m_state"), 0));
        const ReplayValue& seq = e.data.field("m_sequence");
        if (!seq.is_null()) {
            c.has_sequence = true;
            c.sequence = uint32_t(int_or(seq, 0));
        }
    }
    return c;
}

namespace {
struct SelectionTracker {
    std::map<uint32_t, std::array<std::vector<uint32_t>, 11>> users;
    static void apply_mask(std::vector<uint32_t>& v, const ReplayValue& mask) {
        if (mask.kind() != ReplayValue::Kind::Struct || mask.fields().empty()) return;
        const std::string& arm = mask.fields().front().first;
        const ReplayValue& body = mask.fields().front().second;
        if (arm == "Mask") {
            const std::string& bits = body.bytes();
            std::vector<uint32_t> kept;
            for (size_t i = 0; i < v.size(); ++i) {
                const bool set = i / 8 < bits.size() && ((uint8_t(bits[i / 8]) >> (i % 8)) & 1u);
                if (!set) kept.push_back(v[i]);
            }
            v.swap(kept);
        } else if (arm == "OneIndices" || arm == "ZeroIndices") {
            std::vector<bool> listed(v.size(), false);
            for (const ReplayValue& it : body.items()) {
                const int64_t idx = it.as_int();
                if (idx >= 0 && size_t(idx) < v.size()) listed[size_t(idx)] = true;
            }
            std::vector<uint32_t> kept;
            for (size_t i = 0; i < v.size(); ++i)
                if (listed[i] == (arm == "ZeroIndices")) kept.push_back(v[i]);
            v.swap(kept);
        }
    }
    void selection_delta(const formats::ReplayEvent& e) {
        auto& groups = users[e.user_id];
        const int64_t cg = int_or(e.data.field("m_controlGroupId"), 10);
        if (cg < 0 || cg > 10) return;
        std::vector<uint32_t>& v = groups[size_t(cg)];
        const ReplayValue& delta = e.data.field("m_delta");
        if (delta.is_null()) return;
        apply_mask(v, delta.field("m_removeMask"));
        for (const ReplayValue& t : delta.field("m_addUnitTags").items()) v.push_back(uint32_t(t.as_int()));
    }
    void control_group_update(const formats::ReplayEvent& e) {
        auto& groups = users[e.user_id];
        const int64_t idx = int_or(e.data.field("m_controlGroupIndex"), -1);
        const int64_t upd = int_or(e.data.field("m_controlGroupUpdate"), -1);
        if (idx < 0 || idx > 9) return;
        std::vector<uint32_t>& g = groups[size_t(idx)];
        std::vector<uint32_t>& sel = groups[10];
        if (upd == 0) g = sel;
        else if (upd == 1) g.insert(g.end(), sel.begin(), sel.end());
        else if (upd == 2) { apply_mask(g, e.data.field("m_mask")); sel = g; }
        else if (upd == 3) g.clear();
    }
    const std::vector<uint32_t>& selection(uint32_t user) { return users[user][10]; }
};
}

std::vector<ReplayCommand> translate_commands(const std::vector<formats::ReplayEvent>& events,
                                              size_t* inert_count) {
    std::vector<ReplayCommand> out;
    size_t inert = 0;
    SelectionTracker tracker;
    for (const formats::ReplayEvent& e : events) {
        const EventBucket bucket = replay_event_bucket(e.name);
        if (bucket == EventBucket::Inert) {
            ++inert;
            if (e.name == "NNet.Game.SSelectionDeltaEvent") tracker.selection_delta(e);
            else if (e.name == "NNet.Game.SControlGroupUpdateEvent") tracker.control_group_update(e);
            continue;
        }
        if (bucket == EventBucket::Unlisted) {
            throw ReplayRefusal("unclassified_event: " + e.name + " at loop " +
                                std::to_string(e.gameloop) +
                                " has no row in the closed event table. An unlisted event is NOT "
                                "ignored: a person adds the row and argues its bucket on a "
                                "measurement. The table never guesses that an unknown event cannot "
                                "touch sim state.");
        }
        if (bucket == EventBucket::Refused) {
            throw ReplayRefusal("event_without_counterpart: " + e.name + " at loop " +
                                std::to_string(e.gameloop) + ": " + replay_event_reason(e.name));
        }

        out.push_back(translate_command(e));
        if (out.back().kind == ReplayCommandKind::Cmd) out.back().actor_tags = tracker.selection(e.user_id);
    }
    if (inert_count != nullptr) *inert_count = inert;
    return out;
}

void write_replay_commands(const std::vector<ReplayCommand>& commands, std::vector<uint8_t>& out) {
    out.insert(out.end(), kReplayCommandLogMagic, kReplayCommandLogMagic + sizeof(kReplayCommandLogMagic));
    put32(out, kReplayCommandLogVersion);
    put32(out, uint32_t(commands.size()));
    for (const ReplayCommand& c : commands) {
        put32(out, c.gameloop);
        put32(out, c.user_id);
        out.push_back(uint8_t(c.kind));
        out.push_back(uint8_t(c.target));
        out.push_back(uint8_t(c.has_abil ? 1 : 0));
        out.push_back(uint8_t(c.has_abil_cmd_data ? 1 : 0));
        out.push_back(uint8_t(c.has_sequence ? 1 : 0));
        put32(out, c.cmd_flags);
        put32(out, c.abil_link);
        put32(out, c.abil_cmd_index);
        put32(out, c.abil_cmd_data);
        put64(out, uint64_t(c.target_x));
        put64(out, uint64_t(c.target_y));
        put64(out, uint64_t(c.target_z));
        put32(out, c.target_unit_tag);
        put32(out, c.target_unit_flags);
        put32(out, c.target_unit_link);
        put32(out, c.target_data);
        put32(out, c.sequence);
        put32(out, c.manager_state);
        out.push_back(uint8_t(c.has_other_unit ? 1 : 0));
        put32(out, c.other_unit_tag);
        out.push_back(uint8_t(c.has_unit_group ? 1 : 0));
        put32(out, c.unit_group);
        put32(out, uint32_t(c.actor_tags.size()));
        for (uint32_t t : c.actor_tags) put32(out, t);
    }
}

bool read_replay_commands(const uint8_t* data, size_t len, std::vector<ReplayCommand>& out) {
    out.clear();
    if (data == nullptr || len < sizeof(kReplayCommandLogMagic)) return false;
    if (std::memcmp(data, kReplayCommandLogMagic, sizeof(kReplayCommandLogMagic)) != 0) return false;
    Reader r(data + sizeof(kReplayCommandLogMagic), len - sizeof(kReplayCommandLogMagic));
    uint32_t version = 0;
    uint32_t count = 0;
    if (!r.get32(version) || version != kReplayCommandLogVersion) return false;
    if (!r.get32(count)) return false;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ReplayCommand c;
        uint8_t kind = 0;
        uint8_t target = 0;
        uint8_t has_abil = 0;
        uint8_t has_cd = 0;
        uint8_t has_seq = 0;
        uint64_t x = 0;
        uint64_t y = 0;
        uint64_t z = 0;
        if (!r.get32(c.gameloop) || !r.get32(c.user_id) || !r.get8(kind) || !r.get8(target) ||
            !r.get8(has_abil) || !r.get8(has_cd) || !r.get8(has_seq) || !r.get32(c.cmd_flags) ||
            !r.get32(c.abil_link) || !r.get32(c.abil_cmd_index) || !r.get32(c.abil_cmd_data) ||
            !r.get64(x) || !r.get64(y) || !r.get64(z) || !r.get32(c.target_unit_tag) ||
            !r.get32(c.target_unit_flags) || !r.get32(c.target_unit_link) || !r.get32(c.target_data) ||
            !r.get32(c.sequence) || !r.get32(c.manager_state)) {
            out.clear();
            return false;
        }
        if (kind > uint8_t(ReplayCommandKind::ManagerState) ||
            target > uint8_t(ReplayTargetKind::Data)) {
            out.clear();
            return false;
        }
        c.kind = ReplayCommandKind(kind);
        c.target = ReplayTargetKind(target);
        c.has_abil = has_abil != 0;
        c.has_abil_cmd_data = has_cd != 0;
        c.has_sequence = has_seq != 0;
        c.target_x = int64_t(x);
        c.target_y = int64_t(y);
        c.target_z = int64_t(z);
        uint8_t has_ou = 0, has_ug = 0;
        uint32_t n_actor = 0;
        if (!r.get8(has_ou) || !r.get32(c.other_unit_tag) || !r.get8(has_ug) || !r.get32(c.unit_group) || !r.get32(n_actor) ||
            n_actor > 4096) {
            out.clear();
            return false;
        }
        c.has_other_unit = has_ou != 0;
        c.has_unit_group = has_ug != 0;
        c.actor_tags.resize(n_actor);
        for (uint32_t k = 0; k < n_actor; ++k)
            if (!r.get32(c.actor_tags[k])) {
                out.clear();
                return false;
            }
        out.push_back(c);
    }
    if (!r.at_end()) {
        out.clear();
        return false;
    }
    return true;
}

bool write_replay_command_file(const std::vector<ReplayCommand>& commands, const std::string& path) {
    std::vector<uint8_t> bytes;
    write_replay_commands(commands, bytes);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!bytes.empty()) f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(f);
}

bool read_replay_command_file(const std::string& path, std::vector<ReplayCommand>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!f && !f.eof()) return false;
    return read_replay_commands(bytes.data(), bytes.size(), out);
}

OrdersOutcome events_to_orders(const std::vector<ReplayCommand>& commands, OrderLog& out) {
    (void)commands;
    (void)out;
    OrdersOutcome result;
    result.ok = false;
    result.blockers = {
        "ability_join_unproven: the join from (m_abilLink, m_abilCmdIndex) to an ability identity is "
        "not measured; picking one would invent it. The SECOND half this blocker used to claim - "
        "identity to a sim::OrderType - is built and shipped in api/bridge/orders.cpp, so it is no "
        "longer a reason to refuse; what it cannot name is Gather, a worker right-click, which is why "
        "an unmapped == 0 criterion is unreachable and must be a closed nameable/refused table instead",
        "queue_flag_unproven: which bit of m_cmdFlags means queued is not measured, so the queued "
        "flag of every produced order would be a guess",
        "unit_target_unmapped: a command's target unit is a retail tag, and the mapping from a "
        "retail tag to a sim unit id is not built. The ACTING units are now addressed (Lane 9.14: "
        "ReplayCommand::actor_tags from the tracked selection, since SCmdEvent names no units of its "
        "own) but they are the same retail tags, so this one map closes both halves",
        "command_latency_unproven: the loop a command is APPLIED on is measured only as "
        "delta_publish = 0 against the replay's own stamp; whether the retail sim executes on that "
        "loop or later is not separated from the observing instrument's cadence, so no offset is "
        "applied and none is assumed",
    };
    return result;
}

}
