#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc2::casc {
class Storage;
}

namespace sc2::campaign {

struct TrigLibRef {
    std::string library;
    std::string id;
};

enum class TrigParamKind {
    Empty,
    Literal,
    GameLink,
    GameId,
    Preset,
    Variable,
    Function,
    ElementRef,
};

struct TrigFunction;

struct TrigParam {
    TrigLibRef def;
    TrigParamKind kind = TrigParamKind::Empty;
    std::string value_type;
    std::string literal;
    std::string game_type;
    TrigLibRef ref;
    std::string element_type;
    std::string preset_name;
    bool has_array_index = false;
    std::vector<TrigFunction> sub;
};

struct TrigFunction {
    std::string role;
    TrigLibRef def;
    std::string name;
    bool is_comment = false;
    std::string comment;
    std::vector<TrigParam> params;

    std::string sub_slot;
    std::vector<TrigFunction> subs;
};

struct Trigger {
    std::string id;
    std::string name;
    bool init_off = false;
    std::vector<TrigFunction> events;
    std::vector<TrigFunction> conditions;
    std::vector<TrigFunction> actions;
};

struct TriggerModel {
    bool parsed = false;
    std::vector<Trigger> triggers;
    std::size_t element_count = 0;
    std::size_t function_calls = 0;
    std::size_t comments = 0;
};

struct TriggerStrings {
    std::unordered_map<std::string, std::string> names;

    void merge_txt(const std::vector<std::uint8_t>& txt);

    std::string name_of(const std::string& category, const TrigLibRef& ref) const;
};

TriggerModel parse_triggers(const std::vector<std::uint8_t>& triggers_xml,
                            const TriggerStrings* strings);

TriggerStrings load_mission_trigger_strings(const casc::Storage& store, const std::string& map_prefix);

TriggerModel load_mission_triggers(const casc::Storage& store, const std::string& map_prefix);

}
