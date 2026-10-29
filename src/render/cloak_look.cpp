#include "omc/render/cloak_look.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "omc/render/anim.hpp"

namespace omc::render {

const char* cloak_view_name(CloakView v) {
    switch (v) {
        case CloakView::Ally: return "Ally";
        case CloakView::Detected: return "Detected";
        case CloakView::Enemy: return "Enemy";
        default: return "None";
    }
}

bool CloakModelLook::bind(const formats::Model* model, const MaskLoader& load_mask) {
    model_ = nullptr;
    composite_ = -1;
    children_.clear();
    masks_.clear();
    if (!model || model->composites.empty()) return false;
    std::vector<int> displacement;
    for (std::size_t i = 0; i < model->materials.size(); ++i)
        if (model->materials[i].displace > 0.0f) displacement.push_back(int(i));
    std::map<std::string, int> mask_of;
    const formats::M3Composite& cmp = model->composites.front();
    for (const formats::M3CompositeChild& ch : cmp.children) {
        if (!ch.resolved) continue;
        Child c;
        c.alpha_interp = ch.alpha_interp;
        c.alpha_flags = ch.alpha_flags;
        c.alpha_id = ch.alpha_id;
        c.alpha = ch.alpha;
        if (ch.type == 2) {
            if (ch.index >= displacement.size()) continue;
            c.material = displacement[ch.index];
            c.displacement = true;
            children_.push_back(c);
            continue;
        }
        if (ch.type != 1 || ch.index >= model->materials.size()) continue;
        const formats::M3Material& mt = model->materials[ch.index];
        if (mt.displace > 0.0f) continue;
        bool body = false;
        for (const formats::M3Layer& L : mt.layers)
            if (L.usage == "Diffuse" && !L.image_path.empty()) body = true;
        if (!body) continue;
        if (mt.blend_mode == formats::kBlendOpaque) {
            c.cutout = mt.cutout_thresh > 0;
            c.cutout_level = float(mt.cutout_thresh) / 255.0f;
        } else if (mt.blend_mode != formats::kBlendAlphaBlend) {
            continue;
        }
        c.material = int(ch.index);
        if (const formats::M3Layer* mask = formats::material_alpha_mask(mt); mask && !mask->image_path.empty() && load_mask) {
            auto it = mask_of.find(mask->image_path);
            if (it == mask_of.end()) {
                std::vector<float> h = load_mask(mask->image_path);
                int slot = -1;
                if (h.size() == 256) {
                    slot = int(masks_.size());
                    masks_.push_back(std::move(h));
                }
                it = mask_of.emplace(mask->image_path, slot).first;
            }
            c.mask = it->second;
        }
        children_.push_back(c);
    }
    bool any_body = false, any_disp = false;
    for (const Child& c : children_) (c.displacement ? any_disp : any_body) = true;
    if (!any_body || !any_disp) {
        children_.clear();
        masks_.clear();
        return false;
    }
    model_ = model;
    composite_ = 0;
    return true;
}

int CloakModelLook::sequence(const std::string& comma_tokens) const {
    if (!model_ || comma_tokens.empty()) return -1;
    std::vector<std::string> toks;
    std::string cur;
    for (char ch : comma_tokens) {
        if (ch == ',') {
            if (!cur.empty()) toks.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    if (toks.empty()) return -1;
    return select_sequence(*model_, toks, 0);
}

float CloakModelLook::length_ms(int seq) const {
    if (!model_ || seq < 0 || std::size_t(seq) >= model_->sequences.size()) return 0.0f;
    return float(model_->sequences[std::size_t(seq)].length_ms());
}

float CloakModelLook::coverage(const Child& c, int seq, float time_ms) const {
    const formats::M3Material& mt = model_->materials[std::size_t(c.material)];
    const formats::M3Layer* mask = formats::material_alpha_mask(mt);
    if (!mask || c.mask < 0) return 1.0f;
    const formats::M3Layer::AnimRefs& a = mask->anim;
    float mul = a.bright_mult, add = a.midtone_offset;
    animated_layer_field(*model_, std::uint32_t(seq), a.bright_mult_interp, a.bright_mult_flags, a.bright_mult_id, time_ms, 1,
                         &a.bright_mult, &mul);
    animated_layer_field(*model_, std::uint32_t(seq), a.midtone_interp, a.midtone_flags, a.midtone_id, time_ms, 1, &a.midtone_offset,
                         &add);
    if (mask->usage == "AlphaMask" && (mask->word36 & 0x10u) != 0) {
        add = mul + add;
        mul = -mul;
    }
    const std::vector<float>& h = masks_[std::size_t(c.mask)];
    float acc = 0.0f;
    for (int b = 0; b < 256; ++b) {
        if (h[std::size_t(b)] <= 0.0f) continue;
        const float v = float(b) / 255.0f * mul + add;
        if (c.cutout)
            acc += v > c.cutout_level ? h[std::size_t(b)] : 0.0f;
        else
            acc += h[std::size_t(b)] * std::min(1.0f, std::max(0.0f, v));
    }
    return std::min(1.0f, std::max(0.0f, acc));
}

CloakLook CloakModelLook::sample(int seq, float time_ms) const {
    CloakLook out;
    if (!valid() || seq < 0 || std::size_t(seq) >= model_->sequences.size()) return out;
    float clear = 1.0f, strength = 0.0f;
    for (const Child& c : children_) {
        float w = c.alpha;
        animated_layer_field(*model_, std::uint32_t(seq), c.alpha_interp, c.alpha_flags, c.alpha_id, time_ms, 1, &c.alpha, &w);
        w = std::min(1.0f, std::max(0.0f, w));
        if (w <= 0.0f) continue;
        if (c.displacement) {
            const formats::M3Material& mt = model_->materials[std::size_t(c.material)];
            float s = mt.displace;
            animated_layer_field(*model_, std::uint32_t(seq), mt.displace_interp, mt.displace_flags, mt.displace_id, time_ms, 1,
                                 &mt.displace, &s);
            strength += w * std::max(0.0f, s);
        } else {
            clear *= 1.0f - w * coverage(c, seq, time_ms);
        }
    }
    out.opacity = std::min(1.0f, std::max(0.0f, 1.0f - clear));
    out.strength = strength;
    return out;
}

CloakTransitions::Out CloakTransitions::update(std::uint32_t id, CloakView view, double now_ms) {
    if (id >= entries_.size()) entries_.resize(std::size_t(id) + 1);
    Entry& e = entries_[id];
    const bool valid = look_ && look_->valid();
    auto resolve = [&](Entry& x) {
        x.enter_seq = valid ? look_->sequence(x.enter) : -1;
        x.loop_seq = valid ? look_->sequence(x.loop) : -1;
        x.enter_ms = valid ? look_->length_ms(x.enter_seq) : 0.0f;
        x.loop_ms = valid ? look_->length_ms(x.loop_seq) : 0.0f;
        if (x.enter_ms <= 0.0f) x.enter_seq = -1;
    };
    const bool continuing = e.seen && (e.frame == frame_ || e.frame + 1 == frame_);
    if (!continuing) {
        e = Entry{};
        e.view = view;
        std::string enter;
        if (view != CloakView::None && lookup_) lookup_(CloakView::None, view, enter, e.loop);
        e.start_ms = now_ms;
        resolve(e);
    } else if (e.view != view) {
        std::string enter, loop;
        bool found = lookup_ && lookup_(e.view, view, enter, loop);
        if (!found && view != CloakView::None && lookup_) {
            std::string skip;
            lookup_(CloakView::None, view, skip, loop);
        }
        if (!play_enter_) enter.clear();
        const int keep_loop = e.loop_seq;
        const double keep_start = e.start_ms;
        const float keep_enter = e.enter_seq >= 0 ? e.enter_ms : 0.0f;
        e.view = view;
        e.enter = enter;
        e.loop = loop;
        e.start_ms = now_ms;
        resolve(e);
        if (e.enter_seq < 0 && e.loop_seq >= 0 && e.loop_seq == keep_loop && now_ms - keep_start >= keep_enter)
            e.start_ms = keep_start + keep_enter;
    }
    e.seen = true;
    e.frame = frame_;
    Out o;
    const double t = now_ms - e.start_ms;
    if (e.enter_seq >= 0 && t < double(e.enter_ms)) {
        o.cloak_fx = true;
        o.entering = true;
        o.seq = e.enter_seq;
        o.time_ms = float(std::max(0.0, t));
    } else if (view != CloakView::None) {
        o.cloak_fx = true;
        if (e.loop_seq >= 0) {
            const double lt = t - (e.enter_seq >= 0 ? double(e.enter_ms) : 0.0);
            o.seq = e.loop_seq;
            o.time_ms = e.loop_ms > 0.0f ? float(std::fmod(std::max(0.0, lt), double(e.loop_ms))) : 0.0f;
        }
    }
    e.entering = o.entering;
    if (o.cloak_fx && valid && o.seq >= 0) o.look = look_->sample(o.seq, o.time_ms);
    return o;
}

}
