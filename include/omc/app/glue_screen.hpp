#pragma once

#include <optional>
#include <string>
#include <vector>

#include "omc/ui/layout.hpp"

namespace omc::casc {
class Storage;
}

namespace omc::app {

class GlueScreen {
public:
    static std::optional<GlueScreen> harvest(casc::Storage& store,
                                             const std::vector<std::string>& layout_paths,
                                             const ui::Rect& canvas);
    struct HarvestOptions {
        std::size_t screen_docs = std::size_t(-1);
        std::vector<std::string> shown;
        ui::DataResolver data;
    };
    static std::optional<GlueScreen> harvest(casc::Storage& store,
                                             const std::vector<std::string>& layout_paths,
                                             const ui::Rect& canvas, const HarvestOptions& opts);

    const std::vector<ui::SolvedFrame>& controls() const { return controls_; }

    const ui::Rect& hit_rect(std::size_t i) const { return hits_[i]; }

    int documents() const { return documents_; }

    void scale(float s) {
        for (ui::SolvedFrame& c : controls_) c.rect = {c.rect.x * s, c.rect.y * s, c.rect.w * s, c.rect.h * s};
        for (ui::Rect& h : hits_) h = {h.x * s, h.y * s, h.w * s, h.h * s};
    }

    const ui::SolvedFrame* hit(float x, float y) const;
    std::size_t hit_index(float x, float y) const;

private:
    std::vector<ui::SolvedFrame> controls_;
    std::vector<ui::Rect> hits_;
    int documents_ = 0;
};

inline constexpr char kGlueLayoutRoot[] = "mods/core.sc2mod/base.sc2data/UI/Layout/";

}
