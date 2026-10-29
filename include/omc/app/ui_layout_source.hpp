#pragma once

#include <string>

#include "omc/app/ui_icons.hpp"
#include "omc/ui/layout.hpp"

namespace omc::casc {
class Storage;
}
namespace omc::data {
class Catalog;
}
namespace omc::ui {
struct LayoutSet;
struct ConstScope;
struct StyleSet;
}

namespace omc::app {

inline constexpr char kCoreLayoutRoot[] = "mods/core.sc2mod/base.sc2data/";

const ui::LayoutSet* layout_namespace(casc::Storage& store,
                                      const std::string& mod_prefix = kCoreLayoutRoot);

const ui::LayoutSet* layout_namespace(casc::Storage& store, const std::string& mod_prefix,
                                      const ui::ConstScope& scope);

const ui::StyleSet* style_namespace(casc::Storage& store);

ui::TextureFetcher layout_texture_fetcher(casc::Storage& store, const data::Catalog& cat,
                                          IconAtlas& atlas);

}
