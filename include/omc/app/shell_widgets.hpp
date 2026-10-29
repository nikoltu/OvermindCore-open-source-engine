#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "omc/app/overlay_draw.hpp"
#include "omc/app/shell_layout.hpp"
#include "omc/app/shell_ui.hpp"
#include "omc/app/ui_icons.hpp"
#include "omc/app/ui_theme.hpp"
#include "omc/render/font_ttf.hpp"
#include "omc/render/input.hpp"

namespace omc::app::shell {

struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    bool hit(float mx, float my) const { return mx >= float(x0) && mx < float(x1) && my >= float(y0) && my < float(y1); }
    Rect inset(int d) const { return Rect{x0 + d, y0 + d, x1 - d, y1 - d}; }
};

struct FluidSpan {
    float a = 0.0f, b = 0.0f, share = 1.0f;
};
inline constexpr float kFluidMinW = 1920.0f;

enum class Pass { Input, Paint, Collect };
enum class ButtonKind { Primary, Normal, Ghost, Menu };
enum class State { Ok, Warn, Err };
enum class Align { Left, Centre, Right };

struct Opts {
    char hotkey = 0;
    bool enabled = true;
    std::string why_disabled;
    std::string hint;
    std::string trigger;
    std::string tag;
    bool danger = false;
    int force = 0;
};

struct Control {
    std::string id;
    std::string label;
    std::string trigger;
    Rect rect;
    bool enabled = true;
    bool region = false;
    bool takes_lr = false, takes_ud = false;
    std::string hint;
};

struct UiState {
    std::string focus;
    std::string pressed;
    std::string open_dropdown;
    std::string edit_field;
    std::string modal;
    std::string hot;
    std::string hint;
    bool keyboard_mode = false;
    std::map<std::string, int> scroll_px;
    std::size_t caret = 0;
    bool prev_down = false;
    bool prev_pan[4] = {false, false, false, false};
    float mouse_x = -1.0f, mouse_y = -1.0f;
    float down_x = -1.0f, down_y = -1.0f;
    bool down = false;
    float clock_ms = 0.0f;
    std::string last_click;
    float last_click_ms = -1.0e9f;
    float drag_grab = 0.0f;
    std::vector<Control> order;
    std::map<std::string, std::pair<int, int>> list_metrics;
    std::map<std::string, int> num;
    std::map<std::string, std::string> str;
    void reset_screen();
};

struct Faces {
    const render::OverlayFont* face[theme::kFaceCount] = {nullptr, nullptr, nullptr};
};

class Ui {
public:
    Ui(Pass pass, std::uint32_t view_w, std::uint32_t view_h, const render::InputState* in, std::vector<std::uint8_t>* out,
       ShellFx& fx, UiState& st, Faces faces = {}, float dt_ms = 0.0f, double double_click_ms = 500.0);
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    Pass pass() const { return pass_; }
    bool input() const { return pass_ == Pass::Input; }
    bool painting() const { return pass_ == Pass::Paint; }
    std::uint32_t view_w() const { return w_; }
    std::uint32_t view_h() const { return h_; }
    float scale() const { return scale_; }
    UiState& state() { return st_; }
    ShellFx& fx() { return fx_; }
    const ShellKeys& keys() const { return keys_; }

    int px(float pt) const;
    void fluid(std::vector<FluidSpan> spans);
    void fluid_centred(float x0, float x1);
    const std::vector<FluidSpan>& fluid_spans() const { return spans_; }
    float shrink() const { return shrink_; }
    int fluid_columns(int columns, float pitch_pt, float span_pt) const;
    Rect at(float x, float y, float w, float h) const;
    Rect at(const layout::R& r) const { return at(r.x, r.y, r.w, r.h); }
    Rect band(float y, float h) const;
    Rect view() const { return Rect{0, 0, int(w_), int(h_)}; }

    bool button(const std::string& id, Rect r, const std::string& label, ButtonKind kind = ButtonKind::Normal, const Opts& o = {});
    bool segmented(const std::string& id, Rect r, const std::vector<std::string>& cells, int& value, const Opts& o = {});
    bool stepper(const std::string& id, Rect r, int& value, int lo, int hi, const std::string& text, const Opts& o = {});
    bool dropdown(const std::string& id, Rect r, const std::vector<std::string>& items, int& value, const Opts& o = {});
    bool slider(const std::string& id, Rect r, float& value01, const std::string& text, const Opts& o = {});
    bool toggle(const std::string& id, Rect r, bool& value, const Opts& o = {});
    bool text_field(const std::string& id, Rect r, std::string& value, int max_len, const char* placeholder, const Opts& o = {});
    bool tabs(const std::string& id, Rect r, const std::vector<std::string>& names, int& value);
    struct Range {
        int first = 0, count = 0;
    };
    Range list(const std::string& id, Rect viewport, int item_count, int pitch_px, int item_h_px, int columns, int& selected);
    Rect list_item_rect(int index) const;
    void list_end();
    bool item(const std::string& id, Rect r, bool selected, const std::string& label = {}, const Opts& o = {});
    bool double_clicked() const { return double_clicked_; }

    void panel(Rect r, const std::string& title = {});
    void banner(Rect r, State s, const std::string& word, const std::string& sentence);
    void image(Rect r, const IconAtlas::Icon* img, draw::Fit fit, const char* missing_text, float max_scale = 0.0f);
    void picture(Rect r, const IconAtlas::Icon* img, draw::Fit fit = draw::Fit::Contain, float max_scale = 0.0f);
    Rect portrait(Rect frame, const IconAtlas::Icon* img, const theme::Rgb* team = nullptr);
    void progress(Rect r, float frac);
    void text(Rect r, const std::string& s, theme::Type role, theme::Rgb c, Align a = Align::Left);
    int text_width(const std::string& s, theme::Type role) const;
    void fill(Rect r, theme::Rgb c, std::uint8_t a = 255);
    void edge(Rect r, theme::Rgb c, float thick_pt = theme::v2::kLineW, std::uint8_t a = 255);
    void background(bool motion);
    void scrollbar(const std::string& list_id, Rect r);
    void publish_region(const std::string& id, Rect r, const std::string& label = {});
    bool show_badges = true;
    std::function<std::string()> paste;

    void open_modal(const std::string& id) { st_.modal = id; st_.focus.clear(); }
    void close_modal() { st_.modal.clear(); st_.focus.clear(); }
    bool modal_begin(const std::string& id, Rect r, const std::string& title);
    void modal_end();

    const std::string& hint() const { return st_.hint; }
    const std::vector<Control>& controls() const { return controls_; }
    bool enter_pending() const { return input() && keys_.enter && !enter_used_; }
    bool escape_pending() const { return input() && keys_.escape && !escape_used_; }
    void use_escape() { escape_used_ = true; }
    void finish();

private:
    struct Look {
        float hover = 0.0f;
        bool pressed = false, focused = false, enabled = true;
    };
    struct Hit {
        bool hovered = false, activated = false, pressed_now = false, held = false;
    };
    bool live() const;
    Hit interact(const std::string& id, Rect r, const Opts& o, const std::string& label, bool focusable, bool own_layer = false);
    void add_focus(const std::string& id, Rect r, const Opts& o, bool takes_lr, bool takes_ud);
    void label_in(Rect r, const std::string& s, theme::Type role, theme::Rgb c, Align a, int dy = 0);
    Look look(const std::string& id, Rect r, const Opts& o) const;
    void focus_ring(Rect r, const Look& k);
    void badge(Rect r, char hotkey, const Look& k);
    void paint_button(Rect r, const std::string& label, ButtonKind kind, const Look& k, const Opts& o, theme::Type role);
    void publish(const std::string& id, Rect r, const Opts& o, const std::string& label);
    bool arrow_edge(int dir) const { return arrow_[dir]; }

    Pass pass_;
    std::uint32_t w_, h_;
    const render::InputState* in_;
    std::vector<std::uint8_t>* out_;
    ShellFx& fx_;
    UiState& st_;
    Faces faces_;
    ShellKeys keys_;
    float map_x(float x) const;
    float scale_ = 1.0f;
    float shrink_ = 0.0f;
    std::vector<FluidSpan> spans_;
    int ox_ = 0, oy_ = 0;
    double double_click_ms_ = 500.0;
    bool press_edge_ = false, release_edge_ = false;
    bool arrow_[4] = {false, false, false, false};
    bool enter_used_ = false, escape_used_ = false, space_used_ = false, letter_used_ = false;
    bool double_clicked_ = false;
    bool swallow_ = false;
    bool commit_edit_ = false;
    std::string focus_hint_;
    bool in_modal_ = false;
    bool finished_ = false;
    std::string hot_now_, hint_now_;
    std::vector<Control> controls_;
    std::vector<Control> order_now_;
    std::vector<std::function<void()>> deferred_;
    struct ListCtx {
        std::string id;
        Rect viewport;
        int pitch = 1, item_h = 1, columns = 1, count = 0, scroll = 0;
        bool open = false;
    } list_;
};

}
