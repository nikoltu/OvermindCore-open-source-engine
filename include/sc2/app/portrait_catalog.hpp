#pragma once
#include <string>
#include <vector>

#include "sc2/app/sandbox_game.hpp"
#include "sc2/app/ui_icons.hpp"

namespace sc2::casc {
class Storage;
}
namespace sc2::data {
class Catalog;
}

namespace sc2::app {

struct PortraitRef {
    enum class Kind { RaceDefault, Unit, Hero };
    Kind kind = Kind::RaceDefault;
    std::string id;
    static PortraitRef parse(const std::string& s);
    std::string format() const;
    bool operator==(const PortraitRef& o) const { return kind == o.kind && id == o.id; }
    bool operator!=(const PortraitRef& o) const { return !(*this == o); }
};

class PortraitCatalog {
public:
    struct Entry {
        PortraitRef ref;
        std::string name;
        char race = 'N';
        std::string texture;
        std::uint32_t w = 0, h = 0;
    };

    static const char* worker_unit(Race r);
    static std::string unit_portrait_model(const data::Catalog& cat, const std::string& unit_id);
    static std::string unit_texture(const data::Catalog& cat, const std::string& portrait_model_id);

    const IconAtlas::Icon* image(const data::Catalog& cat, casc::Storage& store, const PortraitRef& ref, Race race);
    const IconAtlas::Icon* ai_avatar(casc::Storage& store);
    const IconAtlas::Icon* race_mark(casc::Storage& store, LobbyRace race);
    const IconAtlas::Icon* texture(casc::Storage& store, const std::string& name);

    bool scan(const data::Catalog& cat, casc::Storage& store, int budget);
    bool scanned() const { return scan_done_; }
    int scan_done() const { return int(cursor_); }
    int scan_total() const { return int(todo_.size()); }
    const std::vector<Entry>& units() const { return units_; }
    const std::vector<Entry>& heroes() const { return heroes_; }

private:
    struct Todo {
        bool hero = false;
        std::string id, name, texture;
        char race = 'N';
    };
    void plan(const data::Catalog& cat);
    IconAtlas atlas_;
    std::vector<Entry> units_, heroes_;
    std::vector<Todo> todo_;
    std::size_t cursor_ = 0;
    bool planned_ = false, scan_done_ = false;
};

}
