#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sc2/core/xml.hpp"

namespace sc2::ui {

struct Frame {
    std::string type;
    std::string name;
    std::string tmpl;
    std::vector<core::XmlNode> props;
    std::vector<Frame> children;

    const core::XmlNode* prop(const std::string& tag) const;
    std::size_t count() const;
    const Frame* find(const std::string& name) const;
};

struct ConstScope {
    std::string race;
    std::string skin;
    bool empty() const { return race.empty() && skin.empty(); }
};

const char* layout_race_suffix(int race);

struct Layout {
    bool locked = false;
    std::vector<std::pair<std::string, std::string>> constants;
    std::vector<Frame> frames;

    const std::string* constant(const std::string& name) const;
    std::string resolve(const std::string& value, const ConstScope& scope = ConstScope{}) const;
    std::size_t frame_count() const;
    const Frame* find(const std::string& name) const;
};

Layout parse_layout(const std::string& xml);
Layout parse_layout(const std::vector<uint8_t>& xml);

std::string desc_name_of(const std::string& path);

struct FontStyle {
    std::string name;
    std::string font;
    int height = 0;
    int shadow_offset = 0;
    bool shadow = false;
    std::string vjustify, hjustify;
    bool has_color = false;
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
    bool ok() const { return height > 0; }
};

struct StyleSet {
    void ingest(const std::string& xml);
    FontStyle resolve(const std::string& name) const;
    std::size_t style_count() const { return styles_.size(); }
    std::string expand(std::string v) const;

private:
    struct Raw {
        std::vector<std::pair<std::string, std::string>> attrs;
    };
    std::map<std::string, std::string> consts_;
    std::map<std::string, std::string> font_groups_;
    std::map<std::string, Raw> styles_;
};

struct LayoutSet {
    struct Doc {
        std::string name;
        std::string path;
        Layout layout;
    };
    std::vector<Doc> docs;

    static std::vector<std::string> parse_includes(const std::string& xml);

    void add(const std::string& path, Layout layout);
    void add_xml(const std::string& path, const std::string& xml);

    std::size_t doc_count() const { return docs.size(); }

    const std::string* constant(const std::string& name) const;
    std::string resolve(const std::string& value) const;
    void set_const_scope(const ConstScope& s) { scope = s; }
    const ConstScope& const_scope() const { return scope; }
    ConstScope scope;

    const Frame* find_template(const std::string& path) const;

    Frame instantiate(const Frame& f) const;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

struct SolvedFrame {
    Rect rect;
    std::string type;
    std::string name;
    std::string text;
    std::string parent;
    int depth = 0;
    std::string texture;
    std::string visible;
    std::string accepts_mouse;
};

int measure_text(const std::string& text, int scale = 1);

using TextResolver = std::function<std::string(const std::string& ref)>;

using FrameCollapsed = std::function<bool(const Frame& frame)>;
using DataResolver = std::function<std::string(const std::string& target, const std::string& property)>;

std::vector<SolvedFrame> solve_layout(const Frame& root, const Rect& canvas);
std::vector<SolvedFrame> solve_layout(const Frame& root, const Rect& canvas,
                                      const TextResolver& resolve_text);
std::vector<SolvedFrame> solve_layout(const Frame& root, const Rect& canvas,
                                      const TextResolver& resolve_text,
                                      const FrameCollapsed& collapsed);
std::vector<SolvedFrame> solve_layout(const Frame& root, const Rect& canvas,
                                      const TextResolver& resolve_text,
                                      const FrameCollapsed& collapsed,
                                      const DataResolver& data);

using StyleHeight = std::function<int(const std::string& style_name)>;
std::vector<SolvedFrame> solve_layout(const Frame& root, const Rect& canvas,
                                      const TextResolver& resolve_text,
                                      const FrameCollapsed& collapsed,
                                      const DataResolver& data,
                                      const StyleHeight& style_height);

struct FrameState {
    std::string name;
    std::vector<core::XmlNode> whens;
    std::vector<core::XmlNode> actions;
};

struct FrameStateGroup {
    std::string name;
    std::string default_state;
    std::vector<FrameState> states;
};

std::vector<FrameStateGroup> frame_state_groups(const Frame& f);

bool frame_visible_at_aspect(const Frame& f, float aspect);

std::string selected_state(const Frame& owner, const FrameStateGroup& group, float aspect);

void apply_state_groups(Frame& root, float aspect);

enum class WhenAnswer { False, True, Unknown };
WhenAnswer evaluate_when(const Frame& owner, const core::XmlNode& when, float aspect,
                         const std::vector<const Frame*>& ancestors);
std::string selected_state(const Frame& owner, const FrameStateGroup& group, float aspect,
                           const std::vector<const Frame*>& ancestors);
std::vector<uint8_t> rasterize_layout(const std::vector<SolvedFrame>& frames,
                                      uint32_t w, uint32_t h);

using TextureFetcher =
    std::function<bool(const std::string& path, uint32_t& w, uint32_t& h,
                       std::vector<uint8_t>& rgba)>;

std::vector<uint8_t> rasterize_layout(const std::vector<SolvedFrame>& frames,
                                      uint32_t w, uint32_t h,
                                      const TextureFetcher& fetch);

}
