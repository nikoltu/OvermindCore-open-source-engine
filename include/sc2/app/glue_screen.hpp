#pragma once

#include <optional>
#include <string>
#include <vector>

#include "sc2/ui/layout.hpp"

namespace sc2::casc {
class Storage;
}

namespace sc2::app {

class GlueScreen {
public:
    static std::optional<GlueScreen> harvest(casc::Storage& store,
                                             const std::vector<std::string>& layout_paths,
                                             const ui::Rect& canvas);

    const std::vector<ui::SolvedFrame>& controls() const { return controls_; }

    const ui::Rect& hit_rect(std::size_t i) const { return hits_[i]; }

    int documents() const { return documents_; }

    const ui::SolvedFrame* hit(float x, float y) const;
    std::size_t hit_index(float x, float y) const;

private:
    std::vector<ui::SolvedFrame> controls_;
    std::vector<ui::Rect> hits_;
    int documents_ = 0;
};

inline constexpr char kGlueLayoutRoot[] = "mods/core.sc2mod/base.sc2data/UI/Layout/";

}
