#include "sc2/app/shell_widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sc2::app::shell {

namespace v2 = theme::v2;
using theme::Rgb;

namespace {

Rgb mix(Rgb a, Rgb c, float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    auto ch = [t](std::uint8_t x, std::uint8_t y) { return std::uint8_t(int(x) + int(std::lround(float(int(y) - int(x)) * t))); };
    return Rgb{ch(a.r, c.r), ch(a.g, c.g), ch(a.b, c.b)};
}

struct FaceScope {
    const render::OverlayFont* prev;
    FaceScope(const Faces& f, theme::Face face) : prev(draw::active_font()) {
        const render::OverlayFont* want = f.face[std::size_t(face)];
        if (!want || !want->valid()) want = f.face[0];
        if (want && want->valid()) draw::set_active_font(want);
    }
    ~FaceScope() { draw::set_active_font(prev); }
};

std::string upper(const std::string& s) {
    std::string o = s;
    for (char& c : o)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return o;
}

}

void UiState::reset_screen() {
    focus.clear();
    pressed.clear();
    open_dropdown.clear();
    edit_field.clear();
    modal.clear();
    hot.clear();
    hint.clear();
    order.clear();
    num.clear();
    str.clear();
    caret = 0;
}

Ui::Ui(Pass pass, std::uint32_t view_w, std::uint32_t view_h, const render::InputState* in, std::vector<std::uint8_t>* out,
       ShellFx& fx, UiState& st, Faces faces, float dt_ms, double double_click_ms)
    : pass_(pass), w_(view_w), h_(view_h), in_(in), out_(out), fx_(fx), st_(st), faces_(faces), double_click_ms_(double_click_ms) {
    static const bool scale_to_width = std::getenv("SC2_SHELL_SCALE_TO_WIDTH") != nullptr;
    const float min_w = scale_to_width ? layout::kCanvasW : kFluidMinW;
    const float s0 = h_ > 0 ? float(h_) / layout::kCanvasH : 1.0f;
    const float virt_w = s0 > 0.0f ? float(w_) / s0 : layout::kCanvasW;
    const float k = virt_w < min_w ? virt_w / min_w : 1.0f;
    scale_ = s0 * k;
    const float block_w = scale_ > 0.0f ? std::min(layout::kCanvasW, std::max(min_w, float(w_) / scale_)) : layout::kCanvasW;
    shrink_ = layout::kCanvasW - block_w;
    spans_ = {FluidSpan{v2::kMargin, layout::kCanvasW - v2::kMargin, 1.0f}};
    ox_ = int(std::lround((float(w_) - block_w * scale_) * 0.5f));
    oy_ = int(std::lround((float(h_) - layout::kCanvasH * scale_) * 0.5f));
    if (pass_ != Pass::Input || !in_) return;

    keys_ = ShellKeys::read(*in_);
    press_edge_ = in_->left_down && !st_.prev_down;
    release_edge_ = !in_->left_down && st_.prev_down;
    const bool moved = in_->mouse_dx != 0.0f || in_->mouse_dy != 0.0f ||
                       (st_.mouse_x >= 0.0f && (in_->mouse_x != st_.mouse_x || in_->mouse_y != st_.mouse_y));
    const bool pan[4] = {keys_.left, keys_.right, keys_.up, keys_.down};
    for (int i = 0; i < 4; ++i) arrow_[i] = pan[i] && !st_.prev_pan[i] && keys_.letter == 0 && keys_.text.empty();
    for (int i = 0; i < 4; ++i) st_.prev_pan[i] = pan[i];
    const bool any_arrow = arrow_[0] || arrow_[1] || arrow_[2] || arrow_[3];
    if (keys_.any() || any_arrow) st_.keyboard_mode = true;
    else if (moved || press_edge_) st_.keyboard_mode = false;
    st_.mouse_x = in_->mouse_x;
    st_.mouse_y = in_->mouse_y;
    st_.down = in_->left_down;
    if (in_->left_down) {
        st_.down_x = in_->mouse_x;
        st_.down_y = in_->mouse_y;
    }
    st_.clock_ms += dt_ms;

    if (keys_.escape) {
        if (!st_.open_dropdown.empty()) {
            st_.open_dropdown.clear();
            escape_used_ = true;
        } else if (!st_.edit_field.empty()) {
            st_.edit_field.clear();
            escape_used_ = true;
        }
    }
    if (keys_.enter && !st_.edit_field.empty()) {
        commit_edit_ = true;
        enter_used_ = true;
    }

    if (!st_.order.empty() && st_.open_dropdown.empty() && st_.edit_field.empty()) {
        int cur = -1;
        for (std::size_t i = 0; i < st_.order.size(); ++i)
            if (st_.order[i].id == st_.focus) cur = int(i);
        const int n = int(st_.order.size());
        if (keys_.tab || keys_.shift_tab) {
            const int dir = keys_.shift_tab ? -1 : 1;
            cur = cur < 0 ? (dir > 0 ? 0 : n - 1) : (cur + dir + n) % n;
            st_.focus = st_.order[std::size_t(cur)].id;
        } else if (any_arrow) {
            const int dx = arrow_[1] ? 1 : (arrow_[0] ? -1 : 0), dy = arrow_[3] ? 1 : (arrow_[2] ? -1 : 0);
            if (cur < 0) {
                st_.focus = st_.order[0].id;
                for (bool& a : arrow_) a = false;
            } else {
                const Control& from = st_.order[std::size_t(cur)];
                const bool kept = (dx != 0 && from.takes_lr) || (dy != 0 && from.takes_ud);
                if (!kept) {
                    const float fx0 = float(from.rect.x0 + from.rect.x1) * 0.5f, fy0 = float(from.rect.y0 + from.rect.y1) * 0.5f;
                    int best = -1;
                    float best_d = 0.0f;
                    for (int i = 0; i < n; ++i) {
                        if (i == cur) continue;
                        const Rect& r = st_.order[std::size_t(i)].rect;
                        const float ddx = float(r.x0 + r.x1) * 0.5f - fx0, ddy = float(r.y0 + r.y1) * 0.5f - fy0;
                        const float along = ddx * float(dx) + ddy * float(dy);
                        if (along <= 0.5f) continue;
                        const float across = std::fabs(ddx * float(dy)) + std::fabs(ddy * float(dx));
                        const float d = along + across * 2.0f;
                        if (best < 0 || d < best_d) {
                            best = i;
                            best_d = d;
                        }
                    }
                    if (best >= 0) st_.focus = st_.order[std::size_t(best)].id;
                    for (bool& a : arrow_) a = false;
                }
            }
        }
    }
}

Ui::~Ui() { finish(); }

void Ui::finish() {
    if (finished_) return;
    finished_ = true;
    if (list_.open) list_end();
    for (const std::function<void()>& f : deferred_) f();
    deferred_.clear();
    if (pass_ != Pass::Input || !in_) return;
    st_.hot = hot_now_;
    st_.hint = !hint_now_.empty() ? hint_now_ : (st_.keyboard_mode ? focus_hint_ : std::string());
    st_.order = order_now_;
    if (release_edge_) st_.pressed.clear();
    if (press_edge_ && hot_now_.empty() && st_.pressed.empty()) st_.focus.clear();
    st_.prev_down = in_->left_down;
    bool found = st_.focus.empty();
    for (const Control& c : order_now_) found = found || c.id == st_.focus;
    if (!found) st_.focus.clear();
}

int Ui::px(float pt) const {
    const int v = int(std::lround(pt * scale_));
    return pt > 0.0f && v < 1 ? 1 : v;
}

float Ui::map_x(float x) const {
    if (shrink_ <= 0.0f) return x;
    float out = x;
    for (const FluidSpan& s : spans_) {
        if (s.b <= s.a) continue;
        const float t = x <= s.a ? 0.0f : (x >= s.b ? 1.0f : (x - s.a) / (s.b - s.a));
        out -= shrink_ * s.share * t;
    }
    return out;
}

void Ui::fluid(std::vector<FluidSpan> spans) { spans_ = std::move(spans); }

void Ui::fluid_centred(float x0, float x1) {
    spans_ = {FluidSpan{0.0f, x0, 0.5f}, FluidSpan{x1, layout::kCanvasW, 0.5f}};
}

int Ui::fluid_columns(int columns, float pitch_pt, float span_pt) const {
    if (shrink_ <= 0.0f || pitch_pt <= 0.0f) return columns;
    const int fit = int((span_pt - shrink_) / pitch_pt);
    return fit < 1 ? 1 : (fit < columns ? fit : columns);
}

Rect Ui::at(float x, float y, float w, float h) const {
    return Rect{ox_ + int(std::lround(map_x(x) * scale_)), oy_ + int(std::lround(y * scale_)), ox_ + int(std::lround(map_x(x + w) * scale_)),
                oy_ + int(std::lround((y + h) * scale_))};
}

Rect Ui::band(float y, float h) const {
    Rect r{0, oy_ + int(std::lround(y * scale_)), int(w_), oy_ + int(std::lround((y + h) * scale_))};
    if (y <= 0.0f) r.y0 = 0;
    if (y + h >= layout::kCanvasH) r.y1 = int(h_);
    return r;
}

bool Ui::live() const {
    if (swallow_) return false;
    if (!st_.modal.empty() && !in_modal_) return false;
    return true;
}

void Ui::publish(const std::string& id, Rect r, const Opts& o, const std::string& label) {
    Control c;
    c.id = id;
    c.label = label;
    c.trigger = o.trigger;
    c.rect = r;
    c.enabled = o.enabled;
    controls_.push_back(std::move(c));
}

void Ui::publish_region(const std::string& id, Rect r, const std::string& label) {
    publish(id, r, Opts{}, label);
    controls_.back().region = true;
}

void Ui::picture(Rect r, const IconAtlas::Icon* img, draw::Fit fit, float max_scale) {
    if (!painting() || r.empty() || !img || !img->ok()) return;
    draw::blit_fit(*out_, w_, h_, r.x0, r.y0, r.x1, r.y1, img->rgba.data(), img->w, img->h, fit, max_scale);
}

void Ui::add_focus(const std::string& id, Rect r, const Opts& o, bool takes_lr, bool takes_ud) {
    if (pass_ != Pass::Input || !o.enabled || !live() || !st_.open_dropdown.empty()) return;
    Control c;
    c.id = id;
    c.rect = r;
    c.takes_lr = takes_lr;
    c.takes_ud = takes_ud;
    c.hint = o.hint;
    order_now_.push_back(std::move(c));
    if (st_.focus == id) focus_hint_ = o.hint;
}

Ui::Hit Ui::interact(const std::string& id, Rect r, const Opts& o, const std::string& label, bool focusable, bool own_layer) {
    Hit h;
    publish(id, r, o, label);
    if (pass_ != Pass::Input || !in_) return h;
    const bool can = live() && (st_.open_dropdown.empty() || own_layer);
    bool inside = r.hit(in_->mouse_x, in_->mouse_y);
    if (list_.open && !own_layer) inside = inside && list_.viewport.hit(in_->mouse_x, in_->mouse_y);
    h.hovered = can && inside;
    if (h.hovered) {
        hot_now_ = id;
        hint_now_ = o.enabled ? o.hint : o.why_disabled;
    }
    if (focusable) add_focus(id, r, o, false, false);
    if (!can || !o.enabled) return h;
    if (press_edge_ && h.hovered) {
        st_.pressed = id;
        if (focusable) st_.focus = id;
        h.pressed_now = true;
    }
    h.held = st_.pressed == id && in_->left_down;
    if (release_edge_ && st_.pressed == id && r.hit(st_.down_x, st_.down_y)) {
        h.activated = true;
        double_clicked_ = st_.last_click == id && double(st_.clock_ms - st_.last_click_ms) <= double_click_ms_;
        st_.last_click = double_clicked_ ? std::string() : id;
        st_.last_click_ms = st_.clock_ms;
    }
    if (!h.activated && st_.edit_field.empty()) {
        if (focusable && st_.focus == id && keys_.space && !space_used_) {
            space_used_ = true;
            h.activated = true;
            double_clicked_ = false;
        } else if (o.hotkey && keys_.letter == o.hotkey && !letter_used_ && !in_->key_ctrl && !in_->key_alt) {
            letter_used_ = true;
            h.activated = true;
            double_clicked_ = false;
        }
    }
    return h;
}

Ui::Look Ui::look(const std::string& id, Rect r, const Opts& o) const {
    Look k;
    k.enabled = o.enabled;
    if (!o.enabled) return k;
    k.hover = fx_.hover(id);
    k.pressed = st_.down && st_.pressed == id && r.hit(st_.mouse_x, st_.mouse_y);
    k.focused = st_.keyboard_mode && st_.focus == id;
    if (o.force & 1) k.hover = 1.0f;
    if (o.force & 2) k.pressed = true;
    if (o.force & 4) k.focused = true;
    return k;
}

void Ui::fill(Rect r, Rgb c, std::uint8_t a) {
    if (!painting() || r.empty()) return;
    draw::fill_a_c(*out_, w_, h_, r.x0, r.y0, r.x1, r.y1, c.r, c.g, c.b, a);
}

void Ui::edge(Rect r, Rgb c, float thick_pt, std::uint8_t a) {
    if (!painting() || r.empty()) return;
    draw::box_a_c(*out_, w_, h_, r.x0, r.y0, r.x1, r.y1, c.r, c.g, c.b, a, std::max(1, px(thick_pt)));
}

void Ui::focus_ring(Rect r, const Look& k) {
    if (!k.focused) return;
    const int d = px(4.0f) + std::max(1, px(v2::kLineW));
    edge(Rect{r.x0 - d, r.y0 - d, r.x1 + d, r.y1 + d}, v2::kFocus);
}

int Ui::text_width(const std::string& s, theme::Type role) const {
    FaceScope fs(faces_, role.face);
    return draw::text_w_em(s, std::max(1, int(std::lround(float(role.px) * scale_))), role.track);
}

void Ui::label_in(Rect r, const std::string& s, theme::Type role, Rgb c, Align a, int dy) {
    if (!painting() || s.empty() || r.empty()) return;
    FaceScope fs(faces_, role.face);
    const int em = std::max(1, int(std::lround(float(role.px) * scale_)));
    const std::string run = draw::text_ellipsis(s, em, role.track, r.w());
    if (run.empty()) return;
    const int tw = draw::text_w_em(run, em, role.track);
    int tx = r.x0;
    if (a == Align::Centre) tx = r.x0 + (r.w() - tw) / 2;
    else if (a == Align::Right) tx = r.x1 - tw;
    const int ty = r.y0 + (r.h() - draw::text_h_em(em)) / 2 + dy;
    const draw::ClipRect cl = draw::clip_rect();
    const bool shadow = draw::text_shadow_off();
    draw::text_shadow_off() = true;
    draw::text_em(*out_, w_, h_, tx, ty, run, em, role.track, c.r, c.g, c.b, std::max(cl.x0, r.x0), cl.y0, std::min(cl.x1, r.x1), cl.y1);
    draw::text_shadow_off() = shadow;
}

void Ui::text(Rect r, const std::string& s, theme::Type role, Rgb c, Align a) { label_in(r, s, role, c, a); }

void Ui::badge(Rect r, char hotkey, const Look& k) {
    if (!painting() || !hotkey || !show_badges || !k.enabled) return;
    const int side = px(32.0f), inset = px(24.0f);
    if (r.w() < side * 4) return;
    const Rect b{r.x1 - inset - side, r.y0 + (r.h() - side) / 2, r.x1 - inset, r.y0 + (r.h() - side) / 2 + side};
    fill(b, v2::kSurface1);
    edge(b, v2::kLine2);
    label_in(b, std::string(1, hotkey), v2::kKey, mix(v2::kTextMuted, v2::kTextHi, k.hover), Align::Centre);
}

void Ui::paint_button(Rect r, const std::string& label, ButtonKind kind, const Look& k, const Opts& o, theme::Type role) {
    if (!painting() || r.empty()) return;
    const int dy = k.pressed ? 1 : 0;
    Rgb ink = v2::kTextBody;
    if (kind == ButtonKind::Primary) {
        if (!k.enabled) {
            fill(r, v2::kSurface2);
            edge(r, v2::kLine2);
            ink = v2::kTextDisabled;
        } else {
            draw::vgrad_c(*out_, w_, h_, r.x0, r.y0, r.x1, r.y1, v2::kPrimaryTop.r, v2::kPrimaryTop.g, v2::kPrimaryTop.b, v2::kPrimaryBottom.r,
                          v2::kPrimaryBottom.g, v2::kPrimaryBottom.b);
            if (k.hover > 0.0f) fill(r, Rgb{255, 255, 255}, std::uint8_t(20.0f * k.hover));
            if (k.pressed) fill(r, Rgb{0, 0, 0}, 51);
            edge(r, mix(v2::kPrimaryRim, v2::kAccentHot, k.hover));
            const int t = std::max(1, px(v2::kLineW));
            fill(Rect{r.x0 + t, r.y0 + t, r.x1 - t, r.y0 + t + 1}, Rgb{255, 255, 255}, 40);
            ink = v2::kTextHi;
        }
    } else if (kind == ButtonKind::Ghost) {
        if (k.pressed) fill(r, v2::kSurface1);
        edge(r, k.enabled ? mix(v2::kLine2, v2::kLine3, k.hover) : v2::kLine1);
        ink = k.enabled ? mix(v2::kTextMuted, v2::kTextBody, k.hover) : v2::kTextDisabled;
    } else {
        if (!k.enabled) {
            fill(r, v2::kSurface1);
            edge(r, v2::kLine1);
            ink = v2::kTextDisabled;
        } else {
            fill(r, k.pressed ? v2::kSurface1 : mix(v2::kSurface2, v2::kSurface3, k.hover));
            const Rgb rest = o.danger ? v2::kErr : v2::kLine3;
            edge(r, k.pressed ? v2::kAccentDeep : mix(rest, v2::kAccentHot, k.hover));
            const int t = std::max(1, px(v2::kLineW));
            if (!k.pressed) fill(Rect{r.x0 + t, r.y0 + t, r.x1 - t, r.y0 + t + 1}, Rgb{255, 255, 255}, 14);
            ink = mix(v2::kTextBody, v2::kTextHi, k.hover);
        }
    }
    if (kind == ButtonKind::Menu) {
        if (k.enabled && k.hover > 0.0f) fill(Rect{r.x0, r.y0, r.x0 + px(4.0f), r.y1}, v2::kAccent, std::uint8_t(255.0f * k.hover));
        const int pad = px(24.0f);
        int right = r.x1 - pad;
        if (!o.tag.empty()) {
            const int tw = text_width(o.tag, v2::kLabel);
            label_in(Rect{right - tw, r.y0, right, r.y1}, o.tag, v2::kLabel, k.enabled ? v2::kTextMuted : v2::kTextDisabled, Align::Right, dy);
            right -= tw + pad;
        } else if (o.hotkey && show_badges && k.enabled) {
            right -= px(32.0f) + pad;
        }
        label_in(Rect{r.x0 + pad, r.y0, right, r.y1}, upper(label), role, ink, Align::Left, dy);
        if (o.tag.empty()) badge(r, o.hotkey, k);
    } else if (!label.empty()) {
        const int pad = px(6.0f);
        label_in(Rect{r.x0 + pad, r.y0, r.x1 - pad, r.y1}, upper(label), role, ink, Align::Centre, dy);
    }
    focus_ring(r, k);
}

bool Ui::button(const std::string& id, Rect r, const std::string& label, ButtonKind kind, const Opts& o) {
    Hit h = interact(id, r, o, label, true);
    if (pass_ == Pass::Input && kind == ButtonKind::Primary && o.enabled && live() && st_.open_dropdown.empty() && keys_.enter && !enter_used_) {
        enter_used_ = true;
        h.activated = true;
        double_clicked_ = false;
    }
    if (painting()) paint_button(r, label, kind, look(id, r, o), o, kind == ButtonKind::Primary ? v2::kButtonLg : v2::kButton);
    return h.activated;
}

bool Ui::segmented(const std::string& id, Rect r, const std::vector<std::string>& cells, int& value, const Opts& o) {
    const int n = int(cells.size());
    if (n <= 0) return false;
    bool changed = false;
    const int gap = px(4.0f);
    add_focus(id, r, o, true, false);
    theme::Type role = v2::kButton;
    if (painting()) {
        const int room = (r.w() + gap) / n - gap - px(12.0f);
        const theme::Type steps[4] = {v2::kButton, theme::Type{20, 0.0f, theme::Face::Standard}, theme::Type{17, 0.0f, theme::Face::Standard},
                                      theme::Type{15, 0.0f, theme::Face::Standard}};
        for (const theme::Type& t : steps) {
            role = t;
            bool fits = true;
            for (const std::string& c : cells) fits = fits && text_width(upper(c), t) <= room;
            if (fits) break;
        }
    }
    if (pass_ == Pass::Input && o.enabled && live() && st_.focus == id && st_.edit_field.empty()) {
        if (arrow_[0] && value > 0) { --value; changed = true; }
        if (arrow_[1] && value < n - 1) { ++value; changed = true; }
        if (arrow_[0] || arrow_[1]) arrow_[0] = arrow_[1] = false;
    }
    for (int i = 0; i < n; ++i) {
        const Rect c{r.x0 + int(std::int64_t(r.w() + gap) * i / n), r.y0, r.x0 + int(std::int64_t(r.w() + gap) * (i + 1) / n) - gap, r.y1};
        const std::string cid = id + "/" + cells[std::size_t(i)];
        Opts co = o;
        co.hotkey = 0;
        const Hit h = interact(cid, c, co, cells[std::size_t(i)], false);
        if (h.pressed_now) st_.focus = id;
        if (h.activated && value != i) {
            value = i;
            changed = true;
        }
        if (!painting()) continue;
        Look k = look(cid, c, co);
        k.focused = false;
        if (value == i && o.enabled) {
            fill(c, v2::kSurfaceSel);
            edge(c, v2::kAccent);
            label_in(c.inset(px(6.0f)), upper(cells[std::size_t(i)]), role, v2::kTextHi, Align::Centre);
        } else {
            paint_button(c, cells[std::size_t(i)], ButtonKind::Normal, k, co, role);
        }
    }
    if (painting()) {
        Look g;
        g.focused = st_.keyboard_mode && st_.focus == id;
        focus_ring(r, g);
    }
    return changed;
}

bool Ui::stepper(const std::string& id, Rect r, int& value, int lo, int hi, const std::string& text_s, const Opts& o) {
    bool changed = false;
    const int side = std::min(px(64.0f), r.w() / 3), gap = px(8.0f);
    const Rect minus{r.x0, r.y0, r.x0 + side, r.y1}, plus{r.x1 - side, r.y0, r.x1, r.y1};
    const Rect box{minus.x1 + gap, r.y0, plus.x0 - gap, r.y1};
    add_focus(id, r, o, true, false);
    if (pass_ == Pass::Input && o.enabled && live() && st_.focus == id && st_.edit_field.empty()) {
        if (arrow_[0] && value > lo) { --value; changed = true; }
        if (arrow_[1] && value < hi) { ++value; changed = true; }
        if (arrow_[0] || arrow_[1]) arrow_[0] = arrow_[1] = false;
    }
    Opts om = o, op = o;
    om.hotkey = op.hotkey = 0;
    om.enabled = o.enabled && value > lo;
    op.enabled = o.enabled && value < hi;
    const Hit hm = interact(id + "/-", minus, om, "-", false);
    const Hit hp = interact(id + "/+", plus, op, "+", false);
    if (hm.pressed_now || hp.pressed_now) st_.focus = id;
    if (hm.activated && value > lo) { --value; changed = true; }
    if (hp.activated && value < hi) { ++value; changed = true; }
    if (painting()) {
        Look km = look(id + "/-", minus, om), kp = look(id + "/+", plus, op);
        km.focused = kp.focused = false;
        paint_button(minus, "-", ButtonKind::Normal, km, om, v2::kButton);
        paint_button(plus, "+", ButtonKind::Normal, kp, op, v2::kButton);
        fill(box, v2::kSurface1);
        edge(box, v2::kLine2);
        const int pip_h = px(6.0f), pip_gap = px(4.0f), n = hi - lo + 1;
        const bool pips = n > 1 && n <= 12;
        Rect tr = box;
        if (pips) tr.y1 -= pip_h + px(8.0f);
        label_in(tr.inset(px(4.0f)), text_s, v2::kBody, o.enabled ? v2::kTextBody : v2::kTextDisabled, Align::Centre);
        if (pips) {
            const int pw = std::min(px(24.0f), (box.w() - px(16.0f) - pip_gap * (n - 1)) / n);
            const int total = pw * n + pip_gap * (n - 1);
            int x = box.x0 + (box.w() - total) / 2;
            const int y = box.y1 - px(8.0f) - pip_h;
            for (int i = 0; i < n; ++i, x += pw + pip_gap) fill(Rect{x, y, x + pw, y + pip_h}, (lo + i) <= value && o.enabled ? v2::kAccent : v2::kSurface3);
        }
        Look g;
        g.focused = st_.keyboard_mode && st_.focus == id;
        focus_ring(r, g);
    }
    return changed;
}

bool Ui::toggle(const std::string& id, Rect r, bool& value, const Opts& o) {
    const Hit h = interact(id, r, o, value ? "ON" : "OFF", true);
    if (h.activated) value = !value;
    if (painting()) {
        const Look k = look(id, r, o);
        const Rect off{r.x0, r.y0, r.x0 + r.w() / 2, r.y1}, on{r.x0 + r.w() / 2, r.y0, r.x1, r.y1};
        for (int i = 0; i < 2; ++i) {
            const Rect c = i ? on : off;
            const bool sel = (i == 1) == value;
            if (!o.enabled) {
                fill(c, v2::kSurface1);
                edge(c, v2::kLine1);
            } else if (sel) {
                fill(c, i ? v2::kSurfaceSel : v2::kSurface3);
                edge(c, i ? v2::kAccent : v2::kLine3);
            } else {
                fill(c, k.pressed ? v2::kSurface1 : mix(v2::kSurface2, v2::kSurface3, k.hover));
                edge(c, mix(v2::kLine2, v2::kAccentHot, k.hover));
            }
            label_in(c, i ? "ON" : "OFF", v2::kKey, !o.enabled ? v2::kTextDisabled : (sel ? v2::kTextHi : v2::kTextMuted), Align::Centre);
        }
        focus_ring(r, k);
    }
    return h.activated;
}

bool Ui::slider(const std::string& id, Rect r, float& value01, const std::string& text_s, const Opts& o) {
    bool changed = false;
    const int value_w = text_s.empty() ? 0 : px(112.0f), thumb_w = px(24.0f), thumb_h = std::min(px(40.0f), r.h());
    const Rect track_zone{r.x0, r.y0, r.x1 - value_w, r.y1};
    const int tx0 = track_zone.x0 + thumb_w / 2, tx1 = track_zone.x1 - thumb_w / 2;
    const Hit h = interact(id, track_zone, o, text_s, false);
    add_focus(id, r, o, true, false);
    if (h.pressed_now) st_.focus = id;
    auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    if (pass_ == Pass::Input && in_ && o.enabled && live()) {
        if (h.held && tx1 > tx0) {
            const float v = clamp01((in_->mouse_x - float(tx0)) / float(tx1 - tx0));
            if (v != value01) { value01 = v; changed = true; }
        }
        if (st_.focus == id && st_.edit_field.empty() && (arrow_[0] || arrow_[1])) {
            const float step = keys_.shift ? 0.01f : 0.05f;
            const float v = clamp01(value01 + (arrow_[1] ? step : -step));
            if (v != value01) { value01 = v; changed = true; }
            arrow_[0] = arrow_[1] = false;
        }
    }
    if (painting()) {
        Look k = look(id, track_zone, o);
        k.pressed = o.enabled && st_.down && st_.pressed == id;
        const int cy = (r.y0 + r.y1) / 2, th = std::max(2, px(8.0f));
        const Rect track{tx0, cy - th / 2, tx1, cy - th / 2 + th};
        fill(track, o.enabled ? v2::kSurface3 : v2::kLine1);
        const int tx = tx0 + int(std::lround(float(tx1 - tx0) * clamp01(value01)));
        if (o.enabled) fill(Rect{tx0, track.y0, tx, track.y1}, v2::kAccent);
        const Rgb thumb = !o.enabled ? v2::kTextDisabled : (k.pressed ? v2::kAccentHot : mix(v2::kTextBody, v2::kTextHi, k.hover));
        fill(Rect{tx - thumb_w / 2, cy - thumb_h / 2, tx - thumb_w / 2 + thumb_w, cy - thumb_h / 2 + thumb_h}, thumb);
        if (value_w) label_in(Rect{r.x1 - value_w + px(8.0f), r.y0, r.x1, r.y1}, text_s, v2::kBody, o.enabled ? v2::kTextBody : v2::kTextDisabled, Align::Right);
        Look g;
        g.focused = st_.keyboard_mode && st_.focus == id;
        focus_ring(r, g);
    }
    return changed;
}

bool Ui::text_field(const std::string& id, Rect r, std::string& value, int max_len, const char* placeholder, const Opts& o) {
    bool changed = false;
    const Hit h = interact(id, r, o, value, true);
    if (pass_ == Pass::Input && in_ && o.enabled && live()) {
        const bool editing = st_.edit_field == id;
        if (h.pressed_now || (h.activated && !editing)) {
            st_.edit_field = id;
            st_.focus = id;
        } else if (editing && press_edge_ && !h.hovered) {
            st_.edit_field.clear();
        } else if (editing && commit_edit_) {
            st_.edit_field.clear();
        } else if (editing) {
            std::string add = keys_.text;
            if (add.empty() && keys_.letter && !in_->key_ctrl) add = std::string(1, keys_.letter);
            if (keys_.ctrl_v) {
                add.clear();
                if (paste)
                    for (char ch : paste())
                        if (ch >= 0x20 && ch < 0x7F) add.push_back(ch);
            }
            for (char ch : add)
                if (int(value.size()) < max_len) {
                    value.push_back(ch);
                    changed = true;
                }
            if (keys_.backspace && !value.empty()) {
                value.pop_back();
                changed = true;
            }
            letter_used_ = true;
            space_used_ = true;
        }
    }
    if (painting()) {
        const Look k = look(id, r, o);
        const bool editing = st_.edit_field == id;
        fill(r, v2::kBgBase);
        if (editing) edge(r, v2::kAccent);
        else edge(r, !o.enabled ? v2::kLine1 : mix(v2::kLine3, v2::kAccentHot, k.hover));
        const int pad = px(16.0f);
        const Rect tr{r.x0 + pad, r.y0, r.x1 - pad, r.y1};
        if (value.empty() && !editing) label_in(tr, placeholder ? placeholder : "", v2::kBody, v2::kTextFaint, Align::Left);
        else label_in(tr, value, v2::kBody, v2::kTextHi, Align::Left);
        if (editing && std::fmod(st_.clock_ms, 1060.0f) < 530.0f) {
            const int cx = std::min(tr.x1 - px(2.0f), tr.x0 + text_width(value, v2::kBody) + px(2.0f));
            const int chh = px(32.0f);
            fill(Rect{cx, (r.y0 + r.y1 - chh) / 2, cx + std::max(1, px(2.0f)), (r.y0 + r.y1 - chh) / 2 + chh}, v2::kAccent);
        }
        if (!editing) focus_ring(r, k);
    }
    return changed;
}

bool Ui::tabs(const std::string& id, Rect r, const std::vector<std::string>& names, int& value) {
    bool changed = false;
    const int n = int(names.size());
    const int tw = px(240.0f), gap = px(8.0f);
    if (painting()) fill(Rect{r.x0, r.y1 - std::max(1, px(2.0f)), r.x1, r.y1}, v2::kLine1);
    for (int i = 0; i < n; ++i) {
        const Rect c{r.x0 + (tw + gap) * i, r.y0, r.x0 + (tw + gap) * i + tw, r.y1};
        const std::string cid = id + "/" + names[std::size_t(i)];
        const Hit h = interact(cid, c, Opts{}, names[std::size_t(i)], true);
        if (h.activated && value != i) {
            value = i;
            changed = true;
        }
        if (!painting()) continue;
        const Look k = look(cid, c, Opts{});
        const bool sel = value == i;
        label_in(c, upper(names[std::size_t(i)]), v2::kButton, sel ? v2::kTextHi : mix(v2::kTextMuted, v2::kTextBody, k.hover), Align::Centre);
        if (sel) fill(Rect{c.x0, c.y1 - px(4.0f), c.x1, c.y1}, v2::kAccent);
        focus_ring(c, k);
    }
    return changed;
}

bool Ui::dropdown(const std::string& id, Rect r, const std::vector<std::string>& items, int& value, const Opts& o) {
    bool changed = false;
    const int n = int(items.size());
    const bool open = st_.open_dropdown == id;
    const std::string cur = value >= 0 && value < n ? items[std::size_t(value)] : std::string();
    const Hit h = interact(id, r, o, cur, true, true);
    const int row_h = px(56.0f), shown = std::min(n, 8);
    int first = std::max(0, std::min(int(st_.scroll_px[id]), n - shown));
    Rect lr{r.x0, r.y1, r.x1, r.y1 + row_h * shown};
    if (lr.y1 > int(h_) && r.y0 - row_h * shown >= 0) lr = Rect{r.x0, r.y0 - row_h * shown, r.x1, r.y0};
    if (pass_ == Pass::Input && in_ && o.enabled && live()) {
        if (!open) {
            if (h.activated && st_.open_dropdown.empty()) {
                st_.open_dropdown = id;
                st_.caret = std::size_t(std::max(0, value));
                st_.scroll_px[id] = std::max(0, std::min(value - 3, n - shown));
            }
        } else {
            int hi = int(st_.caret);
            if (arrow_[2] && hi > 0) --hi;
            if (arrow_[3] && hi < n - 1) ++hi;
            arrow_[2] = arrow_[3] = false;
            if (in_->wheel != 0.0f) first = std::max(0, std::min(first + (in_->wheel > 0.0f ? -1 : 1), n - shown));
            if (hi < first) first = hi;
            if (hi >= first + shown) first = hi - shown + 1;
            st_.caret = std::size_t(hi);
            st_.scroll_px[id] = first;
            bool close = false;
            if (keys_.enter && !enter_used_) {
                enter_used_ = true;
                if (value != hi) { value = hi; changed = true; }
                close = true;
            }
            for (int i = 0; i < shown && !close; ++i) {
                const Rect row{lr.x0, lr.y0 + row_h * i, lr.x1, lr.y0 + row_h * (i + 1)};
                if (row.hit(in_->mouse_x, in_->mouse_y)) {
                    hot_now_ = id + "/" + items[std::size_t(first + i)];
                    if (in_->mouse_dx != 0.0f || in_->mouse_dy != 0.0f) st_.caret = std::size_t(first + i);
                    if (press_edge_) st_.pressed = hot_now_;
                    if (release_edge_ && st_.pressed == hot_now_ && row.hit(st_.down_x, st_.down_y)) {
                        if (value != first + i) { value = first + i; changed = true; }
                        close = true;
                    }
                }
            }
            if (!close && press_edge_ && !lr.hit(in_->mouse_x, in_->mouse_y)) {
                close = true;
                swallow_ = !r.hit(in_->mouse_x, in_->mouse_y);
                if (!swallow_) st_.pressed.clear();
            } else if (!close && h.activated) {
                close = true;
            }
            if (close) st_.open_dropdown.clear();
        }
    }
    if (pass_ != Pass::Input && open)
        for (int i = 0; i < shown; ++i)
            publish(id + "/" + items[std::size_t(first + i)], Rect{lr.x0, lr.y0 + row_h * i, lr.x1, lr.y0 + row_h * (i + 1)}, o, items[std::size_t(first + i)]);
    if (painting()) {
        const Look k = look(id, r, o);
        Opts po = o;
        po.hotkey = 0;
        paint_button(r, "", ButtonKind::Normal, k, po, v2::kButton);
        const int pad = px(16.0f), caret_w = px(16.0f);
        label_in(Rect{r.x0 + pad, r.y0, r.x1 - pad * 2 - caret_w, r.y1}, cur, v2::kBody, o.enabled ? mix(v2::kTextBody, v2::kTextHi, k.hover) : v2::kTextDisabled,
                 Align::Left);
        const int cx = r.x1 - pad - caret_w / 2, cy = (r.y0 + r.y1) / 2, step = std::max(1, px(2.0f));
        const Rgb cc = o.enabled ? v2::kTextMuted : v2::kTextDisabled;
        for (int i = 0; i < 4; ++i) fill(Rect{cx - (4 - i) * step, cy - 2 * step + i * step, cx + (4 - i) * step, cy - step + i * step}, cc);
        if (open) {
            const int hi = int(st_.caret);
            std::vector<std::string> rows(items.begin() + first, items.begin() + first + shown);
            const int sel = value - first, hot = hi - first;
            deferred_.push_back([this, lr, rows, row_h, sel, hot, pad]() {
                fill(lr, v2::kSurface2);
                for (int i = 0; i < int(rows.size()); ++i) {
                    const Rect row{lr.x0, lr.y0 + row_h * i, lr.x1, lr.y0 + row_h * (i + 1)};
                    if (i == sel) fill(row, v2::kSurfaceSel);
                    else if (i == hot) fill(row, v2::kSurface3);
                    label_in(Rect{row.x0 + pad, row.y0, row.x1 - pad, row.y1}, rows[std::size_t(i)], v2::kBody, i == sel || i == hot ? v2::kTextHi : v2::kTextBody,
                             Align::Left);
                }
                edge(lr, v2::kLine3);
            });
        }
    }
    return changed;
}

Ui::Range Ui::list(const std::string& id, Rect viewport, int item_count, int pitch_px, int item_h_px, int columns, int& selected) {
    if (list_.open) list_end();
    columns = std::max(1, columns);
    pitch_px = std::max(1, pitch_px);
    const int rows = (item_count + columns - 1) / columns;
    const int content = rows > 0 ? (rows - 1) * pitch_px + item_h_px : 0;
    const int max_scroll = std::max(0, content - viewport.h());
    int scroll = st_.scroll_px[id];
    Opts o;
    add_focus(id, viewport, o, false, true);
    if (pass_ == Pass::Input && in_ && live() && st_.open_dropdown.empty()) {
        const bool over = viewport.hit(in_->mouse_x, in_->mouse_y);
        if (over && in_->wheel != 0.0f) scroll -= (in_->wheel > 0.0f ? 1 : -1) * 3 * pitch_px;
        if (over && press_edge_) st_.focus = id;
        if (st_.focus == id && st_.edit_field.empty() && item_count > 0) {
            const int page = std::max(1, viewport.h() / pitch_px) * columns;
            int s = selected;
            if (arrow_[2]) s -= columns;
            if (arrow_[3]) s += columns;
            if (columns > 1 && arrow_[0]) s -= 1;
            if (columns > 1 && arrow_[1]) s += 1;
            if (keys_.page_up) s -= page;
            if (keys_.page_down) s += page;
            if (keys_.home) s = 0;
            if (keys_.end) s = item_count - 1;
            const bool nav = arrow_[2] || arrow_[3] || (columns > 1 && (arrow_[0] || arrow_[1])) || keys_.page_up || keys_.page_down || keys_.home || keys_.end;
            arrow_[2] = arrow_[3] = false;
            if (columns > 1) arrow_[0] = arrow_[1] = false;
            if (nav) {
                s = std::max(0, std::min(s, item_count - 1));
                selected = s;
                const int top = (s / columns) * pitch_px;
                if (top < scroll) scroll = top;
                if (top + item_h_px > scroll + viewport.h()) scroll = top + item_h_px - viewport.h();
            }
        }
    }
    scroll = std::max(0, std::min(scroll, max_scroll));
    st_.scroll_px[id] = scroll;
    st_.list_metrics[id] = {content, viewport.h()};
    list_.id = id;
    list_.viewport = viewport;
    list_.pitch = pitch_px;
    list_.item_h = item_h_px;
    list_.columns = columns;
    list_.count = item_count;
    list_.scroll = scroll;
    list_.open = true;
    draw::clip_push(viewport.x0, viewport.y0, viewport.x1, viewport.y1);
    if (painting()) {
        Look g;
        g.focused = st_.keyboard_mode && st_.focus == id;
        if (g.focused) deferred_.push_back([this, viewport, g]() { focus_ring(viewport, g); });
    }
    Range rg;
    const int first_row = scroll / pitch_px;
    const int last_row = std::min(rows, (scroll + viewport.h() + pitch_px - 1) / pitch_px);
    rg.first = first_row * columns;
    rg.count = std::max(0, std::min(item_count, last_row * columns) - rg.first);
    return rg;
}

Rect Ui::list_item_rect(int index) const {
    const int col = index % list_.columns, row = index / list_.columns;
    const int cw = list_.viewport.w() / list_.columns;
    const int x0 = list_.viewport.x0 + col * cw, y0 = list_.viewport.y0 + row * list_.pitch - list_.scroll;
    const int gap = list_.columns > 1 ? list_.pitch - list_.item_h : 0;
    return Rect{x0, y0, x0 + cw - gap, y0 + list_.item_h};
}

void Ui::list_end() {
    if (!list_.open) return;
    list_.open = false;
    draw::clip_pop();
}

void Ui::scrollbar(const std::string& list_id, Rect r) {
    if (list_.open) list_end();
    const auto it = st_.list_metrics.find(list_id);
    if (it == st_.list_metrics.end()) return;
    const int content = it->second.first, viewh = it->second.second;
    if (content <= viewh || r.empty()) return;
    const int max_scroll = content - viewh;
    const int thumb_h = std::max(px(48.0f), int(std::int64_t(r.h()) * viewh / content));
    int& scroll = st_.scroll_px[list_id];
    const int travel = std::max(1, r.h() - thumb_h);
    const int ty = r.y0 + int(std::int64_t(travel) * scroll / max_scroll);
    const std::string id = list_id + "/scroll";
    const Rect grab{r.x0 - px(8.0f), r.y0, r.x1 + px(8.0f), r.y1};
    const Hit h = interact(id, grab, Opts{}, "", false);
    if (pass_ == Pass::Input && in_) {
        if (h.pressed_now) st_.drag_grab = (in_->mouse_y >= float(ty) && in_->mouse_y < float(ty + thumb_h)) ? in_->mouse_y - float(ty) : float(thumb_h) * 0.5f;
        if (h.held) {
            const float top = in_->mouse_y - st_.drag_grab - float(r.y0);
            scroll = std::max(0, std::min(max_scroll, int(std::lround(top * float(max_scroll) / float(travel)))));
        }
    }
    if (painting()) {
        const Look k = look(id, grab, Opts{});
        fill(r, v2::kSurface2);
        fill(Rect{r.x0, ty, r.x1, ty + thumb_h}, mix(v2::kLine3, v2::kTextMuted, std::max(k.hover, st_.pressed == id ? 1.0f : 0.0f)));
    }
}

bool Ui::item(const std::string& id, Rect r, bool selected, const std::string& label, const Opts& o) {
    const Hit h = interact(id, r, o, label, false);
    if (painting()) {
        const Look k = look(id, r, o);
        if (selected) {
            fill(r, v2::kSurfaceSel);
            fill(Rect{r.x0, r.y0, r.x0 + px(4.0f), r.y1}, v2::kAccent);
        } else {
            fill(r, k.pressed ? v2::kSurface1 : mix(v2::kSurface1, v2::kSurface2, k.hover));
        }
        const int pad = px(16.0f);
        int right = r.x1 - pad;
        if (!o.tag.empty()) {
            const int tw = text_width(o.tag, v2::kLabel);
            label_in(Rect{right - tw, r.y0, right, r.y1}, o.tag, v2::kLabel, o.danger ? v2::kErr : v2::kTextMuted, Align::Right);
            right -= tw + pad;
        }
        if (!label.empty())
            label_in(Rect{r.x0 + pad, r.y0, right, r.y1}, label, v2::kBody, !o.enabled ? v2::kTextDisabled : (selected ? v2::kTextHi : v2::kTextBody), Align::Left);
    }
    return h.activated;
}

void Ui::panel(Rect r, const std::string& title) {
    if (!painting()) return;
    fill(r, v2::kSurface1);
    edge(r, v2::kLine2);
    if (!title.empty()) label_in(Rect{r.x0 + px(24.0f), r.y0 + px(20.0f), r.x1 - px(24.0f), r.y0 + px(44.0f)}, upper(title), v2::kLabel, v2::kTextMuted, Align::Left);
}

void Ui::banner(Rect r, State s, const std::string& word, const std::string& sentence) {
    if (!painting()) return;
    const Rgb c = s == State::Ok ? v2::kOk : (s == State::Warn ? v2::kWarn : v2::kErr);
    fill(r, v2::kSurface1);
    edge(r, v2::kLine2);
    fill(Rect{r.x0, r.y0, r.x0 + px(6.0f), r.y1}, c);
    int x = r.x0 + px(24.0f);
    if (!word.empty()) {
        const int ww = text_width(upper(word), v2::kLabel);
        label_in(Rect{x, r.y0, x + ww, r.y1}, upper(word), v2::kLabel, c, Align::Left);
        x += ww + px(16.0f);
    }
    label_in(Rect{x, r.y0, r.x1 - px(16.0f), r.y1}, sentence, v2::kBody, v2::kTextBody, Align::Left);
}

void Ui::image(Rect r, const IconAtlas::Icon* img, draw::Fit fit, const char* missing_text, float max_scale) {
    if (!painting() || r.empty()) return;
    fill(r, v2::kBgBase);
    const int t = std::max(1, px(v2::kLineW));
    if (img && img->ok()) {
        draw::blit_fit(*out_, w_, h_, r.x0 + t, r.y0 + t, r.x1 - t, r.y1 - t, img->rgba.data(), img->w, img->h, fit, max_scale);
    } else if (missing_text && missing_text[0]) {
        label_in(r.inset(px(8.0f)), missing_text, v2::kSmall, v2::kTextFaint, Align::Centre);
    }
    edge(r, v2::kLine3);
}

Rect Ui::portrait(Rect frame, const IconAtlas::Icon* img, const Rgb* team) {
    Rect drawn{0, 0, 0, 0};
    if (!painting() || frame.empty()) return drawn;
    fill(frame, v2::kBgBase);
    const int t = std::max(1, px(v2::kLineW));
    if (img && img->ok()) {
        const draw::BlitRect b = draw::blit_fit(*out_, w_, h_, frame.x0, frame.y0, frame.x1, frame.y1, img->rgba.data(), img->w, img->h,
                                                draw::Fit::Contain, 1.0f);
        drawn = Rect{b.x0, b.y0, b.x1, b.y1};
    }
    edge(Rect{frame.x0 - t, frame.y0 - t, frame.x1 + t, frame.y1 + t}, v2::kLine3);
    if (team) fill(Rect{frame.x0, frame.y1, frame.x1, frame.y1 + px(6.0f)}, *team);
    return drawn;
}

void Ui::progress(Rect r, float frac) {
    if (!painting() || r.empty()) return;
    fill(r, v2::kSurface3);
    if (frac < 0.0f) {
        const float t = std::fmod(fx_.clock_ms, 1400.0f) / 1400.0f;
        const int seg = r.w() / 5;
        const int x = r.x0 + int(std::lround(float(r.w() - seg) * t));
        fill(Rect{x, r.y0, x + seg, r.y1}, v2::kAccentHot);
        return;
    }
    if (frac > 1.0f) frac = 1.0f;
    fill(Rect{r.x0, r.y0, r.x0 + int(std::lround(float(r.w()) * frac)), r.y1}, v2::kAccent);
}

void Ui::background(bool motion) {
    if (!painting()) return;
    draw::fill_a_c(*out_, w_, h_, 0, 0, int(w_), int(h_), v2::kBgBase.r, v2::kBgBase.g, v2::kBgBase.b, 255);
    const int gh = px(v2::kGlowH);
    draw::vgrad_c(*out_, w_, h_, 0, 0, int(w_), gh, v2::kBgGlow.r, v2::kBgGlow.g, v2::kBgGlow.b, v2::kBgBase.r, v2::kBgBase.g, v2::kBgBase.b);
    const float pitch = v2::kBandPitch * scale_, bh = v2::kBandH * scale_;
    if (pitch < 2.0f || bh < 2.0f) return;
    const float drift = motion ? std::fmod(fx_.clock_ms * 0.001f * v2::kBandDrift * scale_, pitch) : 0.0f;
    for (float y = drift - pitch; y < float(h_); y += pitch) {
        const int y0 = int(std::lround(y)), y1 = int(std::lround(y + bh)), mid = (y0 + y1) / 2;
        draw::vgrad_c(*out_, w_, h_, 0, y0, int(w_), mid, v2::kAccent.r, v2::kAccent.g, v2::kAccent.b, v2::kAccent.r, v2::kAccent.g, v2::kAccent.b, 0,
                      v2::kBandAlpha);
        draw::vgrad_c(*out_, w_, h_, 0, mid, int(w_), y1, v2::kAccent.r, v2::kAccent.g, v2::kAccent.b, v2::kAccent.r, v2::kAccent.g, v2::kAccent.b,
                      v2::kBandAlpha, 0);
    }
}

bool Ui::modal_begin(const std::string& id, Rect r, const std::string& title) {
    if (st_.modal != id) return false;
    if (pass_ == Pass::Input && keys_.escape && !escape_used_) {
        escape_used_ = true;
        close_modal();
        return false;
    }
    in_modal_ = true;
    if (painting()) {
        draw::fill_a_c(*out_, w_, h_, 0, 0, int(w_), int(h_), 0, 0, 0, v2::kAlphaScrim);
        fill(r, v2::kSurface1);
        edge(r, v2::kLine3);
        fill(Rect{r.x0, r.y0, r.x1, r.y0 + px(4.0f)}, v2::kAccent);
        label_in(Rect{r.x0 + px(24.0f), r.y0 + px(40.0f), r.x1 - px(24.0f), r.y0 + px(72.0f)}, title, v2::kH3, v2::kTextHi, Align::Centre);
    }
    publish_region(id, r, title);
    return true;
}

void Ui::modal_end() { in_modal_ = false; }

}
