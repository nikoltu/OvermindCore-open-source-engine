#include "sc2/app/ui_layout_source.hpp"

#include <exception>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/ui/layout.hpp"

namespace sc2::app {

namespace {

struct SetCache {
    bool tried = false;
    std::unique_ptr<ui::LayoutSet> set;
};

}

const ui::LayoutSet* layout_namespace(casc::Storage& store, const std::string& mod_prefix) {
    static std::map<std::pair<const casc::Storage*, std::string>, SetCache> cache;
    SetCache& c = cache[{&store, mod_prefix}];
    if (c.tried) return c.set.get();
    c.tried = true;
    try {
        auto read = [&](const std::string& p) {
            auto b = store.read_by_name(p);
            return std::string(b.begin(), b.end());
        };
        const std::vector<std::string> incs = ui::LayoutSet::parse_includes(
            read(mod_prefix + "UI/Layout/DescIndex.SC2Layout"));
        if (incs.empty()) return nullptr;
        auto set = std::make_unique<ui::LayoutSet>();
        for (const std::string& inc : incs) {
            try {
                set->add_xml(inc, read(mod_prefix + inc));
            } catch (const std::exception&) {
            }
        }
        if (set->doc_count() == 0) return nullptr;
        c.set = std::move(set);
    } catch (const std::exception&) {
        return nullptr;
    }
    return c.set.get();
}

}
