#pragma once

#include <cstdint>
#include <functional>
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

struct Layout {
    bool locked = false;
    std::vector<std::pair<std::string, std::string>> constants;
    std::vector<Frame> frames;

    const std::string* constant(const std::string& name) const;
    std::string resolve(const std::string& value) const;
    std::size_t frame_count() const;
    const Frame* find(const std::string& name) const;
};

Layout parse_layout(const std::string& xml);
Layout parse_layout(const std::vector<uint8_t>& xml);

std::string desc_name_of(const std::string& path);

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
    int depth = 0;
    std::string texture;
};

std::vector<SolvedFrame> solve_layout(const Frame& root, const Rect& canvas);

std::vector<uint8_t> rasterize_layout(const std::vector<SolvedFrame>& frames,
                                      uint32_t w, uint32_t h);

using TextureFetcher =
    std::function<bool(const std::string& path, uint32_t& w, uint32_t& h,
                       std::vector<uint8_t>& rgba)>;

std::vector<uint8_t> rasterize_layout(const std::vector<SolvedFrame>& frames,
                                      uint32_t w, uint32_t h,
                                      const TextureFetcher& fetch);

}
