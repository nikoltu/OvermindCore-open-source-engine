#pragma once

#include <string>

#include "sc2/app/ui_icons.hpp"
#include "sc2/ui/layout.hpp"

namespace sc2::casc {
class Storage;
}
namespace sc2::data {
class Catalog;
}
namespace sc2::ui {
struct LayoutSet;
struct ConstScope;
struct StyleSet;
}

namespace sc2::app {

inline constexpr char kCoreLayoutRoot[] = "mods/core.sc2mod/base.sc2data/";

const ui::LayoutSet* layout_namespace(casc::Storage& store,
                                      const std::string& mod_prefix = kCoreLayoutRoot);

const ui::LayoutSet* layout_namespace(casc::Storage& store, const std::string& mod_prefix,
                                      const ui::ConstScope& scope);

const ui::StyleSet* style_namespace(casc::Storage& store);

ui::TextureFetcher layout_texture_fetcher(casc::Storage& store, const data::Catalog& cat,
                                          IconAtlas& atlas);

}
