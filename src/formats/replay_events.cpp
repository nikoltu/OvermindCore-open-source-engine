#include "sc2/formats/replay_events.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace sc2::formats {
namespace {

enum class Kind : uint8_t {
    Int,
    Blob,
    Bool,
    Array,
    BitArray,
    Optional,
    Struct,
    Choice,
    FourCC,
    Real32,
    Real64,
    Null,
};

struct FieldSpec {
    const char* name;
    uint16_t type;
    int32_t tag;
};

struct TypeSpec {
    Kind kind;
    int64_t offset;
    uint8_t bits;
    uint16_t elem;
    uint16_t first_field;
    uint16_t field_count;
};

struct EventSpec {
    uint16_t id;
    uint16_t type;
    const char* core;
};

constexpr const char* kEventNamePrefix = "NNet.Game.S";
constexpr const char* kEventNameSuffix = "Event";

#include "replay_schema_97563.inc"

constexpr uint16_t kLoopDeltaType = 7;
constexpr uint16_t kUserIdType = 8;
constexpr uint16_t kEventIdType = 0;
constexpr uint16_t kHeaderType = 18;
constexpr uint16_t kDetailsType = 40;

constexpr int kMaxDepth = 64;

const TypeSpec& type_at(uint16_t id) {
    if (id >= sizeof(kTypes) / sizeof(kTypes[0])) {
        throw std::runtime_error("replay: schema type id " + std::to_string(id) + " out of range");
    }
    return kTypes[id];
}

const EventSpec* find_event(uint32_t id) {
    for (const EventSpec& e : kGameEvents) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool done() const { return next_bits_ == 0 && used_ >= size_; }
    uint64_t used_bits() const { return uint64_t(used_) * 8 - uint64_t(next_bits_); }
    uint64_t total_bits() const { return uint64_t(size_) * 8; }

    uint64_t read_bits(unsigned bits) {
        uint64_t result = 0;
        unsigned got = 0;
        while (got != bits) {
            if (next_bits_ == 0) {
                if (used_ >= size_) truncated(bits);
                next_ = data_[used_++];
                next_bits_ = 8;
            }
            unsigned copy = std::min(bits - got, unsigned(next_bits_));
            uint64_t chunk = uint64_t(next_ & ((1u << copy) - 1u));
            result |= chunk << (bits - got - copy);
            next_ = uint8_t(next_ >> copy);
            next_bits_ = uint8_t(next_bits_ - copy);
            got += copy;
        }
        return result;
    }

    void align() {
        if (next_bits_ != 0 && (next_ & ((1u << next_bits_) - 1u)) != 0) {
            throw std::runtime_error("replay: non-zero padding at bit " +
                                     std::to_string(used_bits()) +
                                     " (" + std::to_string(next_bits_) + " bits)");
        }
        next_bits_ = 0;
    }

    std::string read_aligned_bytes(uint64_t count) {
        align();
        if (count > uint64_t(size_ - used_)) truncated(count * 8);
        std::string out(reinterpret_cast<const char*>(data_ + used_), size_t(count));
        used_ += size_t(count);
        return out;
    }

    std::string read_unaligned_bytes(uint64_t count) {
        std::string out;
        for (uint64_t i = 0; i < count; ++i) out.push_back(char(read_bits(8)));
        return out;
    }

    std::string read_bit_string(uint64_t count) {
        std::string out;
        uint64_t left = count;
        while (left > 0) {
            unsigned take = unsigned(std::min<uint64_t>(left, 8));
            out.push_back(char(read_bits(take)));
            left -= take;
        }
        return out;
    }

private:
    [[noreturn]] void truncated(uint64_t wanted) const {
        throw std::runtime_error("replay: truncated stream, wanted " + std::to_string(wanted) +
                                 " bits at bit " + std::to_string(used_bits()) + " of " +
                                 std::to_string(total_bits()));
    }

    const uint8_t* data_;
    size_t size_;
    size_t used_ = 0;
    uint8_t next_ = 0;
    uint8_t next_bits_ = 0;
};

void attach(ReplayValue& out, const FieldSpec& spec, ReplayValue value, uint16_t field_count) {
    if (std::string(spec.name) != "__parent") {
        out.add_field(spec.name, std::move(value));
        return;
    }
    if (value.kind() == ReplayValue::Kind::Struct) {
        for (const auto& kv : value.fields()) out.add_field(kv.first, kv.second);
    } else if (field_count == 1) {
        out = std::move(value);
    } else {
        out.add_field(spec.name, std::move(value));
    }
}

class PackedDecoder {
public:
    explicit PackedDecoder(BitReader& reader) : r_(reader) {}

    ReplayValue instance(uint16_t type_id, int depth = 0) {
        if (depth > kMaxDepth) throw std::runtime_error("replay: schema nesting too deep");
        const TypeSpec& t = type_at(type_id);
        switch (t.kind) {
            case Kind::Int:
                return ReplayValue::make_int(read_int(t));
            case Kind::Bool:
                return ReplayValue::make_bool(r_.read_bits(1) != 0);
            case Kind::Blob:
                return ReplayValue::make_bytes(r_.read_aligned_bytes(uint64_t(read_int(t))));
            case Kind::FourCC:
                return ReplayValue::make_bytes(r_.read_unaligned_bytes(4));
            case Kind::BitArray: {
                int64_t count = read_int(t);
                return ReplayValue::make_bits(count, r_.read_bit_string(uint64_t(count)));
            }
            case Kind::Array: {
                int64_t count = read_int(t);
                std::vector<ReplayValue> items;
                for (int64_t i = 0; i < count; ++i) items.push_back(instance(t.elem, depth + 1));
                return ReplayValue::make_array(std::move(items));
            }
            case Kind::Optional:
                if (r_.read_bits(1) == 0) return ReplayValue();
                return instance(t.elem, depth + 1);
            case Kind::Choice: {
                int64_t tag = read_int(t);
                const FieldSpec* f = choice_field(t, tag);
                if (f == nullptr) {
                    throw std::runtime_error("replay: choice tag " + std::to_string(tag) +
                                             " not in schema at bit " +
                                             std::to_string(r_.used_bits()));
                }
                ReplayValue out = ReplayValue::make_struct({});
                out.add_field(f->name, instance(f->type, depth + 1));
                return out;
            }
            case Kind::Struct: {
                ReplayValue out = ReplayValue::make_struct({});
                for (uint16_t i = 0; i < t.field_count; ++i) {
                    const FieldSpec& f = kFields[t.first_field + i];
                    attach(out, f, instance(f.type, depth + 1), t.field_count);
                }
                return out;
            }
            case Kind::Real32:
                return ReplayValue::make_bytes(r_.read_unaligned_bytes(4));
            case Kind::Real64:
                return ReplayValue::make_bytes(r_.read_unaligned_bytes(8));
            case Kind::Null:
                return ReplayValue();
        }
        throw std::runtime_error("replay: unhandled schema shape");
    }

private:
    int64_t read_int(const TypeSpec& t) {
        return t.offset + int64_t(r_.read_bits(t.bits));
    }

    static const FieldSpec* choice_field(const TypeSpec& t, int64_t tag) {
        for (uint16_t i = 0; i < t.field_count; ++i) {
            if (kFields[t.first_field + i].tag == tag) return &kFields[t.first_field + i];
        }
        return nullptr;
    }

    BitReader& r_;
};

class VersionedDecoder {
public:
    explicit VersionedDecoder(BitReader& reader) : r_(reader) {}

    ReplayValue instance(uint16_t type_id, int depth = 0) {
        if (depth > kMaxDepth) throw std::runtime_error("replay: schema nesting too deep");
        const TypeSpec& t = type_at(type_id);
        switch (t.kind) {
            case Kind::Int:
                expect(9);
                return ReplayValue::make_int(vint());
            case Kind::Bool:
                expect(6);
                return ReplayValue::make_bool(r_.read_bits(8) != 0);
            case Kind::Blob: {
                expect(2);
                int64_t len = vint();
                return ReplayValue::make_bytes(r_.read_aligned_bytes(uint64_t(len)));
            }
            case Kind::FourCC:
                expect(7);
                return ReplayValue::make_bytes(r_.read_aligned_bytes(4));
            case Kind::BitArray: {
                expect(1);
                int64_t count = vint();
                return ReplayValue::make_bits(count, r_.read_aligned_bytes(uint64_t((count + 7) / 8)));
            }
            case Kind::Array: {
                expect(0);
                int64_t count = vint();
                std::vector<ReplayValue> items;
                for (int64_t i = 0; i < count; ++i) items.push_back(instance(t.elem, depth + 1));
                return ReplayValue::make_array(std::move(items));
            }
            case Kind::Optional: {
                expect(4);
                if (r_.read_bits(8) == 0) return ReplayValue();
                return instance(t.elem, depth + 1);
            }
            case Kind::Choice: {
                expect(3);
                int64_t tag = vint();
                const FieldSpec* f = nullptr;
                for (uint16_t i = 0; i < t.field_count; ++i) {
                    if (kFields[t.first_field + i].tag == tag) f = &kFields[t.first_field + i];
                }
                if (f == nullptr) {
                    skip();
                    return ReplayValue::make_struct({});
                }
                ReplayValue out = ReplayValue::make_struct({});
                out.add_field(f->name, instance(f->type, depth + 1));
                return out;
            }
            case Kind::Struct: {
                expect(5);
                int64_t count = vint();
                ReplayValue out = ReplayValue::make_struct({});
                for (int64_t i = 0; i < count; ++i) {
                    int64_t tag = vint();
                    const FieldSpec* f = nullptr;
                    for (uint16_t j = 0; j < t.field_count; ++j) {
                        if (kFields[t.first_field + j].tag == tag) f = &kFields[t.first_field + j];
                    }
                    if (f == nullptr) {
                        skip();
                        continue;
                    }
                    attach(out, *f, instance(f->type, depth + 1), t.field_count);
                }
                return out;
            }
            case Kind::Real32:
                expect(7);
                return ReplayValue::make_bytes(r_.read_aligned_bytes(4));
            case Kind::Real64:
                expect(8);
                return ReplayValue::make_bytes(r_.read_aligned_bytes(8));
            case Kind::Null:
                return ReplayValue();
        }
        throw std::runtime_error("replay: unhandled schema shape");
    }

private:
    void expect(uint8_t tag) {
        uint64_t got = r_.read_bits(8);
        if (got != tag) {
            throw std::runtime_error("replay: versioned tag " + std::to_string(got) +
                                     " where " + std::to_string(tag) + " was required, at bit " +
                                     std::to_string(r_.used_bits()));
        }
    }

    int64_t vint() {
        uint64_t b = r_.read_bits(8);
        bool negative = (b & 1u) != 0;
        int64_t result = int64_t((b >> 1) & 0x3fu);
        int shift = 6;
        while ((b & 0x80u) != 0) {
            b = r_.read_bits(8);
            result |= int64_t(b & 0x7fu) << shift;
            shift += 7;
            if (shift > 63) throw std::runtime_error("replay: variable integer too long");
        }
        return negative ? -result : result;
    }

    void skip(int depth = 0) {
        if (depth > kMaxDepth) throw std::runtime_error("replay: schema nesting too deep");
        uint64_t tag = r_.read_bits(8);
        switch (tag) {
            case 0: {
                int64_t n = vint();
                for (int64_t i = 0; i < n; ++i) skip(depth + 1);
                break;
            }
            case 1: {
                int64_t n = vint();
                r_.read_aligned_bytes(uint64_t((n + 7) / 8));
                break;
            }
            case 2: {
                int64_t n = vint();
                r_.read_aligned_bytes(uint64_t(n));
                break;
            }
            case 3:
                vint();
                skip(depth + 1);
                break;
            case 4:
                if (r_.read_bits(8) != 0) skip(depth + 1);
                break;
            case 5: {
                int64_t n = vint();
                for (int64_t i = 0; i < n; ++i) {
                    vint();
                    skip(depth + 1);
                }
                break;
            }
            case 6:
                r_.read_aligned_bytes(1);
                break;
            case 7:
                r_.read_aligned_bytes(4);
                break;
            case 8:
                r_.read_aligned_bytes(8);
                break;
            case 9:
                vint();
                break;
            default:
                throw std::runtime_error("replay: unknown versioned shape tag " +
                                         std::to_string(tag));
        }
    }

    BitReader& r_;
};

uint32_t read_le32(const std::vector<uint8_t>& b, size_t at) {
    return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) |
           (uint32_t(b[at + 3]) << 24);
}

uint32_t as_u32(const ReplayValue& v) {
    return uint32_t(v.as_int());
}

}

const ReplayValue& ReplayValue::field(const std::string& name) const {
    static const ReplayValue kNothing;
    for (const auto& kv : fields_) {
        if (kv.first == name) return kv.second;
    }
    return kNothing;
}

ReplayValue ReplayValue::make_int(int64_t v) {
    ReplayValue r;
    r.kind_ = Kind::Int;
    r.int_ = v;
    return r;
}

ReplayValue ReplayValue::make_bool(bool v) {
    ReplayValue r;
    r.kind_ = Kind::Bool;
    r.int_ = v ? 1 : 0;
    return r;
}

ReplayValue ReplayValue::make_bytes(std::string v) {
    ReplayValue r;
    r.kind_ = Kind::Bytes;
    r.bytes_ = std::move(v);
    return r;
}

ReplayValue ReplayValue::make_bits(int64_t bit_count, std::string v) {
    ReplayValue r;
    r.kind_ = Kind::Bytes;
    r.int_ = bit_count;
    r.bytes_ = std::move(v);
    return r;
}

ReplayValue ReplayValue::make_array(std::vector<ReplayValue> v) {
    ReplayValue r;
    r.kind_ = Kind::Array;
    r.items_ = std::move(v);
    return r;
}

ReplayValue ReplayValue::make_struct(std::vector<std::pair<std::string, ReplayValue>> v) {
    ReplayValue r;
    r.kind_ = Kind::Struct;
    r.fields_ = std::move(v);
    return r;
}

void ReplayValue::add_field(std::string name, ReplayValue v) {
    kind_ = Kind::Struct;
    for (auto& kv : fields_) {
        if (kv.first == name) {
            kv.second = std::move(v);
            return;
        }
    }
    fields_.emplace_back(std::move(name), std::move(v));
}

std::string game_event_name(uint32_t event_id) {
    const EventSpec* e = find_event(event_id);
    if (e == nullptr) return std::string();
    return std::string(kEventNamePrefix) + e->core + kEventNameSuffix;
}

ReplayValue decode_versioned(const std::vector<uint8_t>& bytes, uint16_t type_id) {
    BitReader reader(bytes.data(), bytes.size());
    VersionedDecoder decoder(reader);
    return decoder.instance(type_id);
}

ReplayValue decode_replay_details(const std::vector<uint8_t>& bytes) {
    return decode_versioned(bytes, kDetailsType);
}

ReplayVersion decode_replay_version(const std::vector<uint8_t>& replay_image) {
    constexpr size_t kShuntHeader = 16;
    if (replay_image.size() < kShuntHeader || replay_image[0] != 'M' || replay_image[1] != 'P' ||
        replay_image[2] != 'Q' || replay_image[3] != 0x1B) {
        throw std::runtime_error("replay: no user-data block at the head of the image");
    }
    uint32_t block_size = read_le32(replay_image, 12);
    if (block_size == 0 || uint64_t(kShuntHeader) + block_size > replay_image.size()) {
        throw std::runtime_error("replay: user-data block runs past the end of the image");
    }
    BitReader reader(replay_image.data() + kShuntHeader, block_size);
    VersionedDecoder decoder(reader);
    ReplayValue header = decoder.instance(kHeaderType);

    const ReplayValue& v = header.field("m_version");
    ReplayVersion out;
    out.flags = as_u32(v.field("m_flags"));
    out.major = as_u32(v.field("m_major"));
    out.minor = as_u32(v.field("m_minor"));
    out.revision = as_u32(v.field("m_revision"));
    out.build = as_u32(v.field("m_build"));
    out.base_build = as_u32(v.field("m_baseBuild"));
    out.elapsed_game_loops = as_u32(header.field("m_elapsedGameLoops"));
    return out;
}

void require_protocol_build(const ReplayVersion& version) {
    if (version.base_build != kReplayProtocolBuild) {
        throw std::runtime_error("replay: INCOMPARABLE, recorded on base build " +
                                 std::to_string(version.base_build) + " and the only schema present "
                                 "is " + std::to_string(kReplayProtocolBuild));
    }
}

std::vector<ReplayEvent> decode_game_events(const std::vector<uint8_t>& stream) {
    BitReader reader(stream.data(), stream.size());
    PackedDecoder decoder(reader);
    std::vector<ReplayEvent> events;
    uint64_t gameloop = 0;

    while (!reader.done()) {
        const uint64_t start = reader.used_bits();
        const size_t index = events.size();

        ReplayValue delta = decoder.instance(kLoopDeltaType);
        if (delta.fields().empty()) {
            throw std::runtime_error("replay: record " + std::to_string(index) +
                                     " has no loop delta at bit " + std::to_string(start));
        }
        gameloop += uint64_t(delta.fields().front().second.as_int());

        ReplayValue user = decoder.instance(kUserIdType);
        uint64_t event_id = uint64_t(decoder.instance(kEventIdType).as_int());

        const EventSpec* spec = find_event(uint32_t(event_id));
        if (spec == nullptr) {
            throw std::runtime_error("replay: record " + std::to_string(index) + " has event id " +
                                     std::to_string(event_id) +
                                     ", which this build's schema does not assign, at bit " +
                                     std::to_string(start));
        }

        ReplayEvent event;
        event.gameloop = uint32_t(gameloop);
        event.user_id = as_u32(user.field("m_userId"));
        event.event_id = uint32_t(event_id);
        event.name = std::string(kEventNamePrefix) + spec->core + kEventNameSuffix;
        event.data = decoder.instance(spec->type);

        reader.align();
        event.bit_offset = start;
        event.bit_length = reader.used_bits() - start;
        events.push_back(std::move(event));
    }

    if (reader.used_bits() != reader.total_bits()) {
        throw std::runtime_error("replay: stream ended at bit " +
                                 std::to_string(reader.used_bits()) + " of " +
                                 std::to_string(reader.total_bits()) +
                                 ", not on its last record's boundary");
    }
    return events;
}

}
