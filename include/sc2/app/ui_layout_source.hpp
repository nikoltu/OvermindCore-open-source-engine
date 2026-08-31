#pragma once

#include <string>

namespace sc2::casc {
class Storage;
}
namespace sc2::ui {
struct LayoutSet;
}

namespace sc2::app {

inline constexpr char kCoreLayoutRoot[] = "mods/core.sc2mod/base.sc2data/";

const ui::LayoutSet* layout_namespace(casc::Storage& store,
                                      const std::string& mod_prefix = kCoreLayoutRoot);

}
