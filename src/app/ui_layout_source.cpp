#include "sc2/app/ui_layout_source.hpp"

#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/ui/layout.hpp"

namespace sc2::app {

namespace {

struct SetCache {
    bool tried = false;
    std::unique_ptr<ui::LayoutSet> set;
};

}

const ui::LayoutSet* layout_namespace(casc::Storage& store, const std::string& mod_prefix) {
    return layout_namespace(store, mod_prefix, ui::ConstScope{});
}

const ui::LayoutSet* layout_namespace(casc::Storage& store, const std::string& mod_prefix,
                                      const ui::ConstScope& scope) {
    static std::map<std::pair<const casc::Storage*, std::string>, SetCache> cache;
    SetCache& c = cache[{&store, mod_prefix + "|" + scope.race + "|" + scope.skin}];
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
        if (!std::getenv("SC2_LAYOUT_EAGER")) {
            casc::Storage* st = &store;
            const std::string prefix = mod_prefix;
            set->reader = [st, prefix](const std::string& p) {
                auto b = st->read_by_name(prefix + p);
                return std::string(b.begin(), b.end());
            };
        }
        for (const std::string& inc : incs) {
            try {
                set->add_xml(inc, read(mod_prefix + inc));
                set->drop_frames(set->docs.back());
            } catch (const std::exception&) {
            }
        }
        if (set->doc_count() == 0) return nullptr;
        set->set_const_scope(scope);
        c.set = std::move(set);
    } catch (const std::exception&) {
        return nullptr;
    }
    return c.set.get();
}

ui::TextureFetcher layout_texture_fetcher(casc::Storage& store, const data::Catalog& cat,
                                          IconAtlas& atlas) {
    return [&store, &cat, &atlas](const std::string& ref, std::uint32_t& w, std::uint32_t& h,
                                  std::vector<std::uint8_t>& rgba) {
        if (ref.empty()) return false;
        const std::string path = cat.resolve_asset(ref);
        if (path.empty() || path[0] == '@') return false;
        const IconAtlas::Icon& icon = atlas.get(store, path);
        if (!icon.ok()) return false;
        w = icon.w;
        h = icon.h;
        rgba = icon.rgba;
        return true;
    };
}

const ui::StyleSet* style_namespace(casc::Storage& store) {
    struct StyleCache {
        bool tried = false;
        std::unique_ptr<ui::StyleSet> set;
    };
    static std::map<const casc::Storage*, StyleCache> cache;
    StyleCache& c = cache[&store];
    if (c.tried) return c.set.get();
    c.tried = true;
    auto set = std::make_unique<ui::StyleSet>();
    bool any = false;
    for (const char* mod : {"mods/core.sc2mod/base.sc2data/", "mods/liberty.sc2mod/base.sc2data/"}) {
        try {
            const auto bytes = store.read_by_name(std::string(mod) + "UI/FontStyles.SC2Style");
            set->ingest(std::string(bytes.begin(), bytes.end()));
            any = true;
        } catch (const std::exception&) {
        }
    }
    if (!any || set->style_count() == 0) return nullptr;
    c.set = std::move(set);
    return c.set.get();
}

}
