#include "sc2/app/melee_game.hpp"
#include "sc2/app/ui_hotkeys.hpp"
#include "sc2/data/environment.hpp"

#include "sc2/formats/placed_objects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

#include "sc2/app/caster_composition.hpp"
#include "sc2/app/command_bind.hpp"
#include "sc2/app/control.hpp"
#include "sc2/app/doodad_place.hpp"
#include "sc2/app/hud.hpp"
#include "sc2/app/hud_render.hpp"
#include "sc2/app/melee_commands.hpp"
#include "sc2/app/overlay_draw.hpp"
#include "sc2/app/ui_theme.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/app/ability.hpp"
#include "sc2/app/spawn.hpp"
#include "sc2/app/tooltip.hpp"
#include "sc2/data/economy.hpp"
#include "sc2/audio/mixer.hpp"
#include "sc2/audio/vorbis.hpp"
#include "sc2/audio/wasapi.hpp"
#include "sc2/data/actor_catalog.hpp"
#include "sc2/render/anim.hpp"
#include "sc2/data/audio.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/formats/m3.hpp"
#include "sc2/render/mat4.hpp"
#include "sc2/sim/weapon.hpp"
#include "sc2/render/creep_layer.hpp"
#include "sc2/sim/transport.hpp"
#include "sc2/render/prim.hpp"
#include "sc2/ui/font.hpp"

namespace sc2::app {

using sim::Unit;

namespace {

bool ends_with_ci(const std::string& s, const char* suf) {
    const std::size_t n = std::string(suf).size();
    if (s.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        char a = s[s.size() - n + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = char(a + 32);
        if (b >= 'A' && b <= 'Z') b = char(b + 32);
        if (a != b) return false;
    }
    return true;
}

}

MeleeAudio::MeleeAudio() : mixer_(new audio::Mixer(44100, 2)) {}

MeleeAudio::~MeleeAudio() {
    if (sink_) sink_->stop();
    sink_.reset();
}

void MeleeAudio::bind_catalog(const data::Catalog& cat, const data::ActorCatalog& actors,
                              casc::Storage* store) {
    actors_ = &actors;
    audio_cat_.reset(new data::AudioCatalog());
    audio_cat_->build(cat);
    store_ = store;
}

bool MeleeAudio::bound() const { return actors_ && audio_cat_ && store_; }

bool MeleeAudio::start_device() {
    if (!sink_) sink_.reset(new audio::WasapiSink());
    const bool live = sink_->start([this](std::int16_t* out, std::size_t frames) {
        this->render(out, frames);
    });
    if (live) {
        std::lock_guard<std::mutex> lk(mtx_);
        const std::uint32_t r = sink_->rate() ? sink_->rate() : 44100;
        if (!mixer_ || mixer_->out_rate() != r) mixer_.reset(new audio::Mixer(r, 2));
    }
    return live;
}

bool MeleeAudio::device_active() const { return sink_ && sink_->active(); }

const MeleeAudio::PcmClip* MeleeAudio::decode_cached(const std::string& path) {
    auto it = cache_.find(path);
    if (it != cache_.end()) return &it->second;

    PcmClip clip;
    if (store_) {
        try {
            std::vector<std::uint8_t> bytes = store_->read_by_name(path);
            if (ends_with_ci(path, ".ogg")) {
                audio::VorbisPcm p = audio::decode_ogg_vorbis(bytes);
                clip.samples = std::move(p.interleaved);
                clip.rate = p.rate;
                clip.channels = p.channels;
            } else if (ends_with_ci(path, ".wav")) {
                audio::WavData wav;
                if (audio::decode_wav(bytes, wav)) {
                    clip.samples = std::move(wav.samples);
                    clip.rate = wav.rate;
                    clip.channels = wav.channels;
                }
            }
        } catch (...) {
            clip.samples.clear();
            clip.channels = 0;
        }
    }
    auto res = cache_.emplace(path, std::move(clip));
    return &res.first->second;
}

static const float kCombatCueTrim = 0.85f;
static const float kUiCueTrim = 0.90f;

int MeleeAudio::play_clip(const std::string& key, const PcmClip& clip, float gain, double rate_factor) {
    last_voice_id_ = -1;
    if (clip.empty()) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = cache_.find(key);
    if (it == cache_.end()) it = cache_.emplace(key, clip).first;
    const PcmClip* stored = &it->second;
    if (stored->empty()) return 0;
    std::vector<int> live;
    live.reserve(recent_ids_.size());
    for (int id : recent_ids_)
        if (mixer_->is_active(id)) live.push_back(id);
    recent_ids_.swap(live);
    if (int(recent_ids_.size()) >= max_active_voices_) return 0;

    audio::Voice v;
    v.samples = stored->samples.data();
    v.frame_count = stored->samples.size() / std::size_t(stored->channels);
    v.src_rate = stored->rate ? stored->rate : 44100;
    if (rate_factor > 0.0 && rate_factor != 1.0) v.src_rate = std::uint32_t(double(v.src_rate) * rate_factor + 0.5);
    v.channels = stored->channels;
    v.gain = gain;
    const int id = mixer_->add_voice(v);
    recent_ids_.push_back(id);
    last_voice_id_ = id;
    return 1;
}

bool MeleeAudio::voice_active(int id) const {
    if (id < 0 || !mixer_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return mixer_->is_active(id);
}

namespace {
static const char* const kMixCategories[] = {
    "Alert", "Ambient", "Build", "Combat",
    "Death", "Dialogue", "Doodad", "Flames",
    "Foley", "Gather", "ME", "Mission",
    "Movement", "Movie", "Music", "Other",
    "Ready", "SAmbient", "SEmitters", "SPieces",
    "Spell", "TV", "UI", "Voice",
};
static const float kMixGame[] = {
    100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f,
    100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f,
    100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f,
};
static const float kMixCinematic[] = {
    0.0f, 100.0f, 40.0f, 40.0f, 40.0f, 100.0f, 40.0f, 60.0f,
    100.0f, 40.0f, 100.0f, 100.0f, 40.0f, 100.0f, 70.0f, 100.0f,
    40.0f, 100.0f, 100.0f, 100.0f, 40.0f, 100.0f, 100.0f, 0.0f,
};
static const float kMixSpeech[] = {
    0.0f, 100.0f, 25.0f, 25.0f, 25.0f, 100.0f, 40.0f, 60.0f,
    100.0f, 25.0f, 100.0f, 100.0f, 25.0f, 100.0f, 70.0f, 100.0f,
    25.0f, 100.0f, 100.0f, 100.0f, 25.0f, 100.0f, 25.0f, 0.0f,
};
static const float kMixVictory[] = {
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 100.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 100.0f, 0.0f,
};

static_assert(sizeof(kMixGame) / sizeof(kMixGame[0]) == sizeof(kMixCategories) / sizeof(kMixCategories[0]),
              "the Game row must name every category");
static_assert(sizeof(kMixCinematic) / sizeof(kMixCinematic[0]) == sizeof(kMixCategories) / sizeof(kMixCategories[0]),
              "the Cinematic row must name every category");
static_assert(sizeof(kMixSpeech) / sizeof(kMixSpeech[0]) == sizeof(kMixCategories) / sizeof(kMixCategories[0]),
              "the Speech row must name every category");
static_assert(sizeof(kMixVictory) / sizeof(kMixVictory[0]) == sizeof(kMixCategories) / sizeof(kMixCategories[0]),
              "the Victory row must name every category");
}

void MeleeAudio::apply_mix_state(MixState st) {
    const float* row = nullptr;
    switch (st) {
        case MixState::Game: row = kMixGame; break;
        case MixState::Cinematic: row = kMixCinematic; break;
        case MixState::Speech: row = kMixSpeech; break;
        case MixState::Victory: row = kMixVictory; break;
    }
    if (!row) return;
    for (std::size_t i = 0; i < sizeof(kMixCategories) / sizeof(kMixCategories[0]); ++i)
        set_category_gain(kMixCategories[i], row[i] / 100.0f);
    std::lock_guard<std::mutex> lk(mtx_);
    mix_state_ = st;
}

MeleeAudio::MixState MeleeAudio::mix_state() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return mix_state_;
}

void MeleeAudio::set_category_gain(const std::string& category, float g) {
    std::lock_guard<std::mutex> lk(mtx_);
    category_gain_[category] = g;
}

float MeleeAudio::category_gain(const std::string& category) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = category_gain_.find(category);
    return it == category_gain_.end() ? 1.0f : it->second;
}

double MeleeAudio::pitch_rate_factor(const std::string& pitch_pair, const std::string& sound_id,
                                     std::uint64_t play_index) {
    if (pitch_pair.empty()) return 1.0;
    const std::size_t comma = pitch_pair.find(',');
    double lo = 0.0, hi = 0.0;
    try {
        lo = std::stod(comma == std::string::npos ? pitch_pair : pitch_pair.substr(0, comma));
        hi = comma == std::string::npos ? lo : std::stod(pitch_pair.substr(comma + 1));
    } catch (const std::exception&) {
        return 1.0;
    }
    if (hi < lo) std::swap(lo, hi);
    if (hi == lo) return std::pow(2.0, lo / 12.0);
    std::uint64_t h = 1099511628211ull;
    for (char ch : sound_id) {
        h ^= std::uint64_t(static_cast<unsigned char>(ch));
        h *= 1469598103934665603ull;
    }
    h ^= play_index * 0x9E3779B97F4A7C15ull + 0x5851F42D4C957F2Dull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    const double u = double(h >> 11) / double(1ull << 53);
    return std::pow(2.0, (lo + (hi - lo) * u) / 12.0);
}

float MeleeAudio::volume_db_pair_to_gain(const std::string& db_pair) {
    if (db_pair.empty()) return 1.0f;
    const std::size_t comma = db_pair.find(',');
    const std::string lo = comma == std::string::npos ? db_pair : db_pair.substr(0, comma);
    try {
        const double db = std::stod(lo);
        if (db <= -96.0) return 0.0f;
        if (db >= 0.0) return 1.0f;
        return float(std::pow(10.0, db / 20.0));
    } catch (const std::exception&) {
        return 1.0f;
    }
}

std::uint64_t MeleeAudio::play_hash(const std::string& sound_id, std::uint64_t play_index) {
    std::uint64_t h = 1469598103934665603ull;
    for (char ch : sound_id) {
        h ^= std::uint64_t(static_cast<unsigned char>(ch));
        h *= 1099511628211ull;
    }
    h ^= play_index + 0x9E3779B97F4A7C15ull;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

std::size_t MeleeAudio::weighted_pick(const std::vector<float>& weights, std::uint64_t hash) {
    if (weights.empty()) return 0;
    double total = 0.0;
    for (float w : weights) total += w > 0.0f ? double(w) : 0.0;
    if (total <= 0.0) return std::size_t(hash % weights.size());
    const double u = double(std::uint32_t(hash >> 16)) / 4294967296.0 * total;
    double acc = 0.0;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        acc += weights[i] > 0.0f ? double(weights[i]) : 0.0;
        if (u < acc) return i;
    }
    return weights.size() - 1;
}

bool MeleeAudio::chance_allows(const std::optional<std::string>& chance, std::uint64_t hash) {
    if (!chance || chance->empty()) return true;
    const double c = std::atof(chance->c_str());
    if (c >= 100.0) return true;
    if (c <= 0.0) return false;
    return double(hash % 10000ull) / 100.0 < c;
}

std::size_t MeleeAudio::select_variation(const std::string& sound_id, const std::string& select,
                                         std::size_t asset_count, std::uint64_t play_index) {
    if (asset_count == 0) return 0;
    if (select == "Sequential") return std::size_t(play_index % asset_count);
    return std::size_t(play_hash(sound_id, play_index) % asset_count);
}
int MeleeAudio::play_sound(const std::string& sound_id, float gain) {
    last_sound_id_ = sound_id;
    if (!bound() || sound_id.empty()) return 0;
    const data::Sound* def = audio_cat_->sound(sound_id);
    if (!def || def->assets.empty()) return 0;
    std::uint64_t play_index = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        play_index = play_count_[sound_id]++;
    }
    const std::uint64_t hash = play_hash(sound_id, play_index);
    if (!chance_allows(def->chance, hash)) return 0;
    const std::string select = def->select ? *def->select : std::string();
    bool any_weight = false;
    std::vector<float> weights;
    for (const data::SoundAsset& a : def->assets) {
        float w = kDefaultAssetWeight;
        if (a.weight && !a.weight->empty()) { w = float(std::atof(a.weight->c_str())); any_weight = true; }
        weights.push_back(w);
    }
    std::size_t pick = select_variation(sound_id, select, def->assets.size(), play_index);
    if (any_weight && select != "Sequential") pick = weighted_pick(weights, hash);
    for (std::size_t tries = 0; tries < def->assets.size(); ++tries) {
        const data::SoundAsset& a = def->assets[(pick + tries) % def->assets.size()];
        const PcmClip* clip = decode_cached(a.file);
        if (!clip || clip->empty()) continue;
        float g = gain;
        if (def->volume) g *= volume_db_pair_to_gain(*def->volume);
        if (def->category) g *= category_gain(*def->category);
        if (a.volume) g *= volume_db_pair_to_gain(*a.volume);
        const double rate = def->pitch ? pitch_rate_factor(*def->pitch, sound_id, play_index) : 1.0;
        return play_clip(a.file, *clip, g, rate);
    }
    return 0;
}

int MeleeAudio::play_event(const std::string& unit_catalog_id, data::UnitEvent ev, float gain, float listener_distance) {
    if (!bound()) return 0;
    last_rolloff_gain_ = 1.0f;
    std::vector<data::SoundCue> cues =
        data::resolve_sound_cues(*actors_, *audio_cat_, unit_catalog_id, ev);
    std::vector<std::string> order;
    std::unordered_map<std::string, std::vector<const data::SoundCue*>> by_sound;
    for (const data::SoundCue& c : cues) {
        auto& v = by_sound[c.sound_id];
        if (v.empty()) order.push_back(c.sound_id);
        v.push_back(&c);
    }
    int voices = 0;
    bool any_asset = false;
    for (const std::string& sid : order) {
        const std::vector<const data::SoundCue*>& variants = by_sound[sid];
        const data::Sound* def = audio_cat_->sound(sid);
        std::uint64_t play_index = 0;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            play_index = play_count_[sid]++;
        }
        const std::string select = def && def->select ? *def->select : std::string();
        const std::uint64_t hash = play_hash(sid, play_index);
        if (def && !chance_allows(def->chance, hash)) continue;
        auto asset_of = [&](const data::SoundCue* c) -> const data::SoundAsset* {
            if (!def) return nullptr;
            for (const data::SoundAsset& a : def->assets)
                if (a.file == c->asset_path) return &a;
            return nullptr;
        };
        bool any_weight = false;
        std::vector<float> weights;
        for (const data::SoundCue* c : variants) {
            const data::SoundAsset* a = asset_of(c);
            float w = kDefaultAssetWeight;
            if (a && a->weight && !a->weight->empty()) { w = float(std::atof(a->weight->c_str())); any_weight = true; }
            weights.push_back(w);
        }
        std::size_t pick = select_variation(sid, select, variants.size(), play_index);
        if (any_weight && select != "Sequential") pick = weighted_pick(weights, hash);
        const PcmClip* clip = nullptr;
        const data::SoundCue* chosen = nullptr;
        for (std::size_t tries = 0; tries < variants.size(); ++tries) {
            const data::SoundCue* cand = variants[(pick + tries) % variants.size()];
            const PcmClip* c = decode_cached(cand->asset_path);
            if (c && !c->empty()) {
                clip = c;
                chosen = cand;
                break;
            }
        }
        if (!clip || !chosen) continue;
        any_asset = true;
        float g = gain;
        if (def) {
            if (def->volume) g *= volume_db_pair_to_gain(*def->volume);
            if (def->category) g *= category_gain(*def->category);
            if (const data::SoundAsset* a = asset_of(chosen); a && a->volume) g *= volume_db_pair_to_gain(*a->volume);
            const bool two_d = def->mode && *def->mode == "2D";
            if (!two_d && listener_distance >= 0.0f && !def->rolloff.empty()) {
                const float r = data::rolloff_gain(*def, listener_distance);
                last_rolloff_gain_ = r;
                g *= r;
                if (r <= 0.0f) continue;
            }
        }
        const double rate = def && def->pitch ? pitch_rate_factor(*def->pitch, sid, play_index) : 1.0;
        voices += play_clip(chosen->asset_path, *clip, g, rate);
    }
    if (any_asset)
        resolved_[unit_catalog_id + "|" + std::string(data::to_string(ev))] = 1;
    return voices;
}

void MeleeAudio::render(std::int16_t* out, std::size_t frames) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (mixer_)
        mixer_->mix(out, frames);
    else
        std::fill(out, out + frames * 2, std::int16_t(0));
}

std::size_t MeleeAudio::cached_assets() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return cache_.size();
}
std::size_t MeleeAudio::resolved_cue_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return resolved_.size();
}
std::uint32_t MeleeAudio::voice_src_rate(int id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const audio::Voice* v = mixer_ ? mixer_->voice(id) : nullptr;
    return v ? v->src_rate : 0u;
}

std::size_t MeleeAudio::voice_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return mixer_ ? mixer_->voice_count() : 0;
}

namespace {

void alpha_over(std::vector<uint8_t>& base, const std::vector<uint8_t>& top) {
    size_t n = std::min(base.size(), top.size());
    for (size_t i = 0; i + 3 < n; i += 4) {
        float a = top[i + 3] / 255.0f;
        for (int c = 0; c < 3; ++c)
            base[i + c] = uint8_t(float(top[i + c]) * a + float(base[i + c]) * (1.0f - a) + 0.5f);
        base[i + 3] = uint8_t(std::min(255.0f, float(top[i + 3]) + float(base[i + 3]) * (1.0f - a) + 0.5f));
    }
}

std::array<float, 4> team_colour(int player, bool selected) {
    std::array<float, 4> c;
    if (player == 0)
        c = {0.85f, 0.15f, 0.12f, 1.0f};
    else if (player == 1)
        c = {0.15f, 0.35f, 0.90f, 1.0f};
    else
        return {0, 0, 0, -1.0f};
    if (selected)
        for (int i = 0; i < 3; ++i) c[i] = std::min(1.0f, c[i] + 0.35f);
    return c;
}

std::optional<data::UnitEvent> classify_actor_cue(const std::string& name) {
    std::string s;
    s.reserve(name.size());
    for (char c : name) s += (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
    auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
    if (has("death") || has("die")) return data::UnitEvent::Death;
    if (has("attack") || has("fire") || has("launch") || has("weapon") || has("shoot"))
        return data::UnitEvent::Attack;
    if (has("birth") || has("spawn")) return data::UnitEvent::Birth;
    return std::nullopt;
}

int race_of(const std::string& t) {
    static const char* kProtoss[] = {"Probe", "Zealot", "Stalker", "Sentry", "Adept",
        "Immortal", "Colossus", "Nexus", "Pylon", "Gateway", "WarpGate", "Assimilator",
        "Cybernetics", "Forge", "Stargate", "Robotics", "Twilight", "Dark", "Templar",
        "Archon", "Phoenix", "Oracle", "Void", "Carrier", "Tempest", "Mothership",
        "Observer", "WarpPrism", "Disruptor"};
    static const char* kZerg[] = {"Drone", "Zergling", "Baneling", "Roach", "Ravager",
        "Hydralisk", "Lurker", "Infestor", "SwarmHost", "Ultralisk", "Overlord", "Overseer",
        "Mutalisk", "Corruptor", "Brood", "Viper", "Queen", "Hatchery", "Lair", "Hive",
        "Spawning", "Extractor", "Spire", "Nydus", "Creep", "Roach", "Baneling"};
    for (const char* s : kProtoss) if (t.find(s) != std::string::npos) return 1;
    for (const char* s : kZerg) if (t.find(s) != std::string::npos) return 2;
    return 0;
}

int race_index(const data::Catalog* cat, const std::string& catalog_id, const std::string& token) {
    if (cat && !catalog_id.empty())
        if (const int r = cat->unit_race_index(catalog_id); r >= 0) return r;
    return race_of(token);
}

struct WireMask { int w = 0, h = 0; std::vector<std::uint8_t> cov; };

const WireMask& wire_mask(const formats::Model* m, int W, int H) {
    static std::unordered_map<const formats::Model*, WireMask> cache;
    auto it = cache.find(m);
    if (it != cache.end() && it->second.w == W && it->second.h == H) return it->second;
    WireMask wm; wm.w = W; wm.h = H; wm.cov.assign(std::size_t(W) * H, 0);
    if (m && m->vertex_count > 2 && !m->positions.empty()) {
        float mnx = 1e30f, mxx = -1e30f, mnz = 1e30f, mxz = -1e30f;
        for (std::uint32_t i = 0; i < m->vertex_count; ++i) {
            const float x = m->positions[3 * i], z = m->positions[3 * i + 2];
            mnx = x < mnx ? x : mnx; mxx = x > mxx ? x : mxx;
            mnz = z < mnz ? z : mnz; mxz = z > mxz ? z : mxz;
        }
        const float spanx = std::max(mxx - mnx, 1e-3f), spanz = std::max(mxz - mnz, 1e-3f);
        const float availW = W * 0.84f, availH = H * 0.84f;
        const float s = std::min(availW / spanx, availH / spanz);
        const float ox = (W - spanx * s) * 0.5f, oy = (H - spanz * s) * 0.5f;
        auto proj = [&](std::uint32_t i, float& sx, float& sy) {
            sx = ox + (m->positions[3 * i] - mnx) * s;
            sy = oy + (mxz - m->positions[3 * i + 2]) * s;
        };
        auto raster = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            if (a >= m->vertex_count || b >= m->vertex_count || c >= m->vertex_count) return;
            float ax, ay, bx, by, cx, cy;
            proj(a, ax, ay); proj(b, bx, by); proj(c, cx, cy);
            int x0 = std::max(0, int(std::floor(std::min(ax, std::min(bx, cx)))));
            int x1 = std::min(W - 1, int(std::ceil(std::max(ax, std::max(bx, cx)))));
            int y0 = std::max(0, int(std::floor(std::min(ay, std::min(by, cy)))));
            int y1 = std::min(H - 1, int(std::ceil(std::max(ay, std::max(by, cy)))));
            const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            if (std::fabs(area) < 1e-6f) return;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area;
                    const float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 >= -0.001f && w1 >= -0.001f && w2 >= -0.001f)
                        wm.cov[std::size_t(y) * W + x] = 255;
                }
        };
        for (const formats::M3Region& r : m->regions) {
            const std::uint32_t end = r.first_index + r.num_indices;
            for (std::uint32_t k = r.first_index; k + 2 < end && k + 2 < m->indices.size(); k += 3)
                raster(m->indices[k] + r.first_vertex, m->indices[k + 1] + r.first_vertex,
                       m->indices[k + 2] + r.first_vertex);
        }
    }
    return cache.emplace(m, std::move(wm)).first->second;
}

float model_diameter(const formats::Model& m) {
    if (!m.vertex_count || m.positions.size() < size_t(m.vertex_count) * 3) return 0.0f;
    float mn = 1e30f, mx = -1e30f;
    for (uint32_t i = 0; i < m.vertex_count; ++i)
        for (int a = 0; a < 2; ++a) {
            float v = m.positions[i * 3 + a];
            mn = v < mn ? v : mn;
            mx = v > mx ? v : mx;
        }
    return mx - mn;
}

std::unordered_map<const formats::Model*, float>& diam_cache() {
    static std::unordered_map<const formats::Model*, float> c;
    return c;
}
float cached_diameter(const formats::Model* m) {
    if (!m) return 0.0f;
    auto& c = diam_cache();
    auto it = c.find(m);
    if (it != c.end()) return it->second;
    float d = model_diameter(*m);
    c[m] = d;
    return d;
}

void draw_text_rgba(std::vector<std::uint8_t>& buf, std::uint32_t w, std::uint32_t h, int x,
                    int y, const std::string& text, int s, std::uint8_t r, std::uint8_t g,
                    std::uint8_t b) {
    auto put = [&](int px, int py, std::uint8_t cr, std::uint8_t cg, std::uint8_t cb) {
        if (px < 0 || py < 0 || px >= int(w) || py >= int(h)) return;
        std::size_t i = (std::size_t(py) * w + px) * 4;
        buf[i] = cr; buf[i + 1] = cg; buf[i + 2] = cb; buf[i + 3] = 255;
    };
    int pen = x;
    for (char ch : text) {
        for (int row = 0; row < ui::kFontH; ++row)
            for (int col = 0; col < ui::kFontW; ++col)
                if (ui::font_pixel(ch, col, row))
                    for (int dy = 0; dy < s; ++dy)
                        for (int dx = 0; dx < s; ++dx) {
                            int px = pen + col * s + dx, py = y + row * s + dy;
                            put(px + s, py + s, 0, 0, 0);
                            put(px, py, r, g, b);
                        }
        pen += (ui::kFontW + 1) * s;
    }
}

void draw_box_rgba(std::vector<std::uint8_t>& buf, std::uint32_t w, std::uint32_t h, int x0,
                   int y0, int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    auto put = [&](int px, int py) {
        if (px < 0 || py < 0 || px >= int(w) || py >= int(h)) return;
        std::size_t i = (std::size_t(py) * w + px) * 4;
        buf[i] = r; buf[i + 1] = g; buf[i + 2] = b; buf[i + 3] = 255;
    };
    for (int x = x0; x <= x1; ++x) { put(x, y0); put(x, y1); }
    for (int y = y0; y <= y1; ++y) { put(x0, y); put(x1, y); }
}

}

std::array<float, 4> team_colour_for(const data::Catalog* cat, int player, bool selected) {
    if (player < 0) return {0, 0, 0, -1.0f};
    if (cat) {
        static const data::Catalog* cached_for = nullptr;
        static std::vector<data::TeamColor> palette;
        if (cached_for != cat) {
            palette = data::team_colors(*cat);
            cached_for = cat;
        }
        for (const data::TeamColor& tc : palette)
            if (tc.slot == player + 1) {
                std::array<float, 4> c = {tc.diffuse[0], tc.diffuse[1], tc.diffuse[2], 1.0f};
                if (selected)
                    for (int k = 0; k < 3; ++k) c[k] = std::min(1.0f, c[k] + 0.35f);
                return c;
            }
    }
    return team_colour(player, selected);
}

MeleeGame::MeleeGame(MeleeGameConfig cfg) : cfg_(cfg), world_(cfg.seed) {
    alerts_.set_player(cfg_.human_player >= 0 ? cfg_.human_player : 0);
}

int MeleeGame::hall_race() const {
    if (cfg_.human_player < 0 || size_t(cfg_.human_player) >= layout_.players.size()) return -1;
    const std::uint32_t depot = layout_.players[size_t(cfg_.human_player)].base.depot;
    if (depot >= world_.units().size()) return -1;
    const std::string& id = world_.units()[depot].catalog_id;
    return race_index(cat_, id, id);
}

std::string MeleeGame::human_gas_structure() const {
    const int r = hall_race();
    return r == 2 ? "Extractor" : r == 1 ? "Assimilator" : "Refinery";
}

std::string MeleeGame::human_town_hall() const {
    const int r = hall_race();
    return r == 2 ? "Hatchery" : r == 1 ? "Nexus" : "CommandCenter";
}

bool MeleeGame::human_is_protoss() const { return hall_race() == 1; }

int MeleeGame::human_race() const {
    const int r = hall_race();
    return r < 0 ? 0 : r;
}

MeleeCommandContext MeleeGame::command_context() {
    MeleeCommandContext ctx;
    if (cat_)
        if (auto ic = cat_->button_icon("Cancel")) ctx.cancel_icon = std::move(*ic);
    if (cfg_.human_player < 0) return ctx;
    for (std::uint32_t id : selection_)
        if (id < world_.units().size() && world_.unit(id).alive &&
            world_.unit(id).player == cfg_.human_player) {
            const sim::Unit& u = world_.unit(id);
            if (u.commandable) ctx.commandable_selected = true;
            if (u.speed.raw() != 0) {
                ctx.mobile_selected = true;
                if (id < layout_.unit_type.size() && layout_.unit_type[id] == "SCV")
                    ctx.worker_selected = true;
                else
                    ctx.combat_selected = true;
            } else if (u.supply_provides.raw() > 0) {
                ctx.town_hall_selected = true;
            }
        }
    if (cat_ && !selection_.empty()) {
        const std::uint32_t lead = selection_.front();
        if (lead < world_.units().size() && world_.unit(lead).alive &&
            !world_.unit(lead).catalog_id.empty()) {
            auto take = [&](const data::Catalog::CommandCardSlot& sl,
                            MeleeCommandContext::UniversalCell& cell) {
                if (sl.row < 0 || sl.col < 0) return;
                const int slot = sl.row * CommandCard::kCols + sl.col;
                if (slot < 0 || slot >= CommandCard::kSlots) return;
                cell.slot = slot;
                if (auto ic = cat_->button_icon(sl.face)) cell.icon = std::move(*ic);
                if (auto nm = cat_->button_display_name(sl.face)) cell.name = std::move(*nm);
            };
            const sim::Unit& lead_u = world_.unit(lead);
            std::string lead_id = lead_u.catalog_id;
            if (lead_u.burrowed) {
                const std::string var = cat_->burrowed_variant(lead_id);
                if (!var.empty()) lead_id = var;
            }
            const std::string burrow_ab = cat_->burrow_ability(lead_id);
            const std::string unburrow_ab = cat_->unburrow_ability(lead_id);
            const std::string liftoff_ab = cat_->liftoff_ability(lead_id);
            const std::string unload_ab = cat_->transport_ability(lead_id);
            const std::string merge_ab = cat_->merge_ability(lead_id);
            for (const data::Catalog::CommandCardLayout& c : cat_->unit_command_cards(lead_id)) {
                if (c.index != 0) continue;
                for (const data::Catalog::CommandCardSlot& sl : c.slots) {
                    if (sl.type != "AbilCmd") continue;
                    if (sl.face == "Move") take(sl, ctx.univ_move);
                    else if (sl.face == "Stop") take(sl, ctx.univ_stop);
                    else if (sl.face == "MoveHoldPosition") take(sl, ctx.univ_hold);
                    else if (sl.face == "Attack") take(sl, ctx.univ_attack);
                    else if (sl.face == "MovePatrol") take(sl, ctx.univ_patrol);
                    else if (!burrow_ab.empty() && sl.ability == burrow_ab) take(sl, ctx.univ_burrow);
                    else if (!unburrow_ab.empty() && sl.ability == unburrow_ab)
                        take(sl, ctx.univ_unburrow);
                    else if (!liftoff_ab.empty() && sl.ability == liftoff_ab)
                        take(sl, ctx.univ_liftoff);
                    else if (!unload_ab.empty() && sl.ability == unload_ab) take(sl, ctx.univ_unload);
                    else if (!merge_ab.empty() && sl.ability == merge_ab) take(sl, ctx.univ_merge);
                }
            }
        }
    }
    ctx.cancellable_selected = selected_constructing_building() != sim::kNoTarget ||
                               selected_training_producer() != sim::kNoTarget;
    ctx.producer_built = human_producer_ != sim::kNoTarget;
    const int bank = world_.player(cfg_.human_player).minerals;
    ctx.can_afford_producer = bank >= human_build_.producer_cost &&
                              world_.player(cfg_.human_player).gas >= human_build_.producer_gas;
    ctx.can_afford_supply = bank >= human_build_.supply_cost;
    if (ctx.worker_selected && !ctx.town_hall_selected && cat_ &&
        size_t(cfg_.human_player) < layout_.players.size()) {
        ctx.worker_gas_selected = true;
        ctx.refinery_name = human_gas_structure();
        ctx.can_afford_refinery = bank >= data::unit_cost(*cat_, ctx.refinery_name).minerals;
        ctx.worker_expand_selected = true;
        ctx.town_hall_name = human_town_hall();
        ctx.can_afford_town_hall = bank >= data::unit_cost(*cat_, ctx.town_hall_name).minerals;
    }
    if (ctx.worker_selected && !ctx.town_hall_selected && ctx.producer_built && cat_) {
        ctx.worker_starport_selected = true;
        const data::UnitCost sc = data::unit_cost(*cat_, "Starport");
        ctx.can_afford_starport = bank >= sc.minerals && world_.player(cfg_.human_player).gas >= sc.gas;
    }
    if (ctx.worker_selected && !ctx.town_hall_selected && cat_ && human_is_protoss()) {
        ctx.worker_pylon_selected = true;
        ctx.can_afford_pylon = bank >= data::unit_cost(*cat_, "Pylon").minerals;
        ctx.worker_gateway_selected = true;
        ctx.can_afford_gateway = bank >= data::unit_cost(*cat_, "Gateway").minerals;
    }
    if (cat_) ctx.can_afford_factory = bank >= data::unit_cost(*cat_, "Factory").minerals;
    if (cat_) ctx.can_afford_engbay = bank >= data::unit_cost(*cat_, "EngineeringBay").minerals;
    if (cat_) {
        if (selected_liberator() != sim::kNoTarget) ctx.liberator_selected = true;
        if (selected_liberator_ag() != sim::kNoTarget) ctx.liberator_ag_selected = true;
    }
    if (cat_ && selected_merge_templar().size() >= 2) ctx.templar_merge_selected = true;
    if (cat_) {
        const std::vector<std::uint32_t> surfaced = selected_burrowers(false);
        if (!surfaced.empty()) ctx.burrow_selected = true;
        if (!selected_burrowers(true).empty()) ctx.unburrow_selected = true;
        ctx.burrow_requirement_met = surfaced.empty();
        for (std::uint32_t id : surfaced)
            if (burrow_command_allowed(id)) { ctx.burrow_requirement_met = true; break; }
    }
    if (cat_) {
        if (selected_liftoff_structure(false) != sim::kNoTarget) ctx.liftoff_selected = true;
        if (selected_liftoff_structure(true) != sim::kNoTarget) ctx.land_selected = true;
    }
    if (cat_) {
        if (selected_depot(false) != sim::kNoTarget) ctx.depot_lower_selected = true;
        if (selected_depot(true) != sim::kNoTarget) ctx.depot_raise_selected = true;
    }
    if (selected_loaded_transport() != sim::kNoTarget) ctx.transport_unload_selected = true;
    return ctx;
}

std::uint32_t MeleeGame::selected_training_producer() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (world_.queue_count(id) > 0) return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_constructing_building() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || !u.constructing) continue;
        if (u.speed.raw() != 0) continue;
        return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_liberator() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.catalog_id == "Liberator") return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_liberator_ag() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.catalog_id == "LiberatorAG") return id;
    }
    return sim::kNoTarget;
}

std::vector<std::uint32_t> MeleeGame::selected_merge_templar() const {
    std::vector<std::uint32_t> out;
    if (cfg_.human_player < 0 || !cat_) return out;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (!cat_->merge_ability(u.catalog_id).empty()) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::uint32_t MeleeGame::selected_liftoff_structure(bool flying) const {
    if (cfg_.human_player < 0 || !cat_) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.flying != flying) continue;
        if (!cat_->liftoff_ability(u.catalog_id).empty()) return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_depot(bool lowered) const {
    if (cfg_.human_player < 0 || !cat_) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.lowered != lowered) continue;
        if (!cat_->depot_lower_ability(u.catalog_id).empty()) return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_loaded_transport() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.cargo_space > 0 && world_.cargo_used(id) > 0) return id;
    }
    return sim::kNoTarget;
}

bool MeleeGame::burrow_command_allowed(std::uint32_t id) const {
    if (!cat_ || id >= world_.units().size()) return true;
    const sim::Unit& u = world_.units()[id];
    if (u.catalog_id.empty()) return true;
    const std::string ab = cat_->burrow_ability(u.catalog_id);
    if (ab.empty()) return true;
    for (const data::Catalog::CommandCardLayout& c : cat_->unit_command_cards(u.catalog_id))
        for (const data::Catalog::CommandCardSlot& sl : c.slots) {
            if (sl.type != "AbilCmd" || sl.ability != ab) continue;
            const std::string req = cat_->button_requirement(sl.ability, sl.command);
            if (req.empty()) return true;
            return data::eval_requirement(*cat_, req, "Use", human_tech_state(id));
        }
    return true;
}

std::vector<std::uint32_t> MeleeGame::selected_burrowers(bool burrowed) const {
    std::vector<std::uint32_t> out;
    if (cfg_.human_player < 0 || !cat_) return out;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.burrowed != burrowed) continue;
        if (!cat_->burrow_ability(u.catalog_id).empty()) out.push_back(id);
    }
    return out;
}

void MeleeGame::setup(const MeleeSetup& setup, MeleeModels* models,
                      const formats::Model* ground, float map_w, float map_h,
                      const sim::NavGrid* nav, const sim::TerrainMap* placement,
                      float playable_x0, float playable_y0, float playable_x1,
                      float playable_y1) {
    ground_ = ground;
    map_w_ = map_w;
    map_h_ = map_h;
    playable_x0_ = playable_x0;
    playable_y0_ = playable_y0;
    playable_x1_ = playable_x1;
    playable_y1_ = playable_y1;
    if (map_w > 0.0f && map_h > 0.0f)
        world_.creep().configure(int(map_w) + 1, int(map_h) + 1, sim::Fixed::from_int(1),
                                 sim::Fixed(), sim::Fixed());
    layout_ = build_melee(world_, setup);
    for (std::size_t p = 0; p < setup.players.size(); ++p)
        world_.set_supply_ceiling(int32_t(p), sim::Fixed::from_int(setup.players[p].food_ceiling));
    for (const sim::Unit& u : world_.units())
        if (u.alive && u.catalog_id == "XelNagaTower")
            world_.register_watchtower(u.id, sim::Fixed::from_double(2.5));
    if (nav) world_.nav() = *nav;
    if (placement) world_.placement() = *placement;
    if (world_.nav().configured())
        for (const sim::Unit& u : world_.units()) {
            if (!u.alive || u.catalog_id.find("Destructible") == std::string::npos) continue;
            world_.register_footprint_block(u.id);
        }
    if (world_.nav().configured())
        for (const sim::Unit& u : world_.units()) {
            if (!u.alive || u.player >= 0 || u.resource_left <= 0) continue;
            world_.register_footprint_block(u.id);
        }
    if (world_.nav().configured())
        for (const sim::Unit& u : world_.units()) {
            if (!u.alive || u.player < 0) continue;
            if (u.speed.raw() != 0 || (u.attributes & sim::kAttrStructure) == 0) continue;
            if (u.resource_left > 0 || u.is_gas) continue;
            world_.register_footprint_block(u.id);
        }
    {
        std::vector<std::uint32_t> towers;
        for (const sim::Unit& u : world_.units()) {
            const std::string& c = u.catalog_id;
            if (u.alive && c.rfind("Collapsible", 0) == 0 && c.find("Tower") != std::string::npos &&
                c.find("Debris") == std::string::npos && c.find("PushUnit") == std::string::npos)
                towers.push_back(u.id);
        }
        const int n = int(towers.size());
        std::vector<int> parent(n);
        for (int i = 0; i < n; ++i) parent[i] = i;
        auto find = [&](int i) {
            while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
            return i;
        };
        const sim::Fixed r2 = sim::Fixed::from_int(15) * sim::Fixed::from_int(15);
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                const sim::Unit& a = world_.unit(towers[i]);
                const sim::Unit& b = world_.unit(towers[j]);
                const sim::Fixed dx = a.x - b.x, dy = a.y - b.y;
                if (dx * dx + dy * dy <= r2) parent[find(i)] = find(j);
            }
        for (int root = 0; root < n; ++root) {
            if (find(root) != root) continue;
            std::vector<std::uint32_t> members;
            for (int i = 0; i < n; ++i)
                if (find(i) == root) members.push_back(towers[i]);
            world_.register_conjoined_group(std::move(members));
        }
    }
    world_.set_separation(true);
    world_.set_acceleration(true);
    world_.set_turn_limit(true);
    world_.enable_building_burndown();
    world_.enable_flee_response();
    world_.enable_multi_hit_armor();
    world_.set_projectile_travel(true);
    world_.set_attack_windup(true);
    world_.set_backswing(true);
    world_.set_fire_while_moving(true);
    world_.enable_point_defense();
    world_.set_fog_acquire(true);

    models_ = models;
    driver_.default_model = models ? models->model_for("Marine") : nullptr;
    for (uint32_t id = 0; id < layout_.unit_type.size(); ++id) {
        const formats::Model* m = nullptr;
        if (models) {
            const std::string& cid =
                id < world_.units().size() ? world_.unit(id).catalog_id : std::string();
            if (!cid.empty()) m = models->model_for(cid);
            if (!m) m = models->model_for(layout_.unit_type[id]);
        }
        driver_.set_model(id, m);
        if (id < layout_.unit_facing_set.size() && layout_.unit_facing_set[id])
            driver_.set_spawn_facing(id, layout_.unit_facing[id]);
    }
    producer_model_ = models ? models->model_for("Barracks") : nullptr;
    bound_count_ = layout_.unit_type.size();
    spawn_worker_model_.assign(layout_.players.size(), models ? models->model_for("SCV") : nullptr);
    spawn_army_model_.assign(layout_.players.size(), driver_.default_model);
    spawn_producer_model_.assign(layout_.players.size(), producer_model_);

    ais_.clear();
    for (size_t p = 0; p < layout_.players.size(); ++p) {
        ai::MeleeConfig cfg = layout_.players[p].config;
        if (int(p) != cfg_.human_player) {
            cfg.smart_combat = true;
            cfg.num_scouts = 1;
            if (cfg_.ai_difficulty > 0)
                ai::apply_difficulty(cfg, ai::Difficulty(cfg_.ai_difficulty), cfg_.ai_period_ticks);
        }
        auto ai = std::make_unique<ai::MeleeAI>(int(p), cfg);
        ai->set_base(layout_.players[p].base);
        if (map_w_ > 0.0f && map_h_ > 0.0f)
            ai->set_map_bounds(sim::Bounds{sim::Fixed(), sim::Fixed(), sim::Fixed::from_double(map_w_),
                                           sim::Fixed::from_double(map_h_)});
        ais_.push_back(std::move(ai));
    }

    camctl_.yaw = 1.5707963f;
    camctl_.mirror_x = true;
    render::apply_melee_rest_frame(camctl_);

    if (cfg_.human_player >= 0 && size_t(cfg_.human_player) < setup.players.size()) {
        const MeleePlayerSetup& hp = setup.players[size_t(cfg_.human_player)];
        human_build_.producer_cost = hp.producer_mineral_cost;
        human_build_.producer_gas = hp.producer_gas_cost;
        human_build_.army_cost = hp.army_mineral_cost;
        human_build_.worker_proto = hp.worker;
        human_build_.worker_cost = hp.worker_mineral_cost;
        human_build_.worker_build_time = hp.worker_build_time;
        human_build_.supply_proto = hp.supply_unit;
        human_build_.supply_cost = hp.supply_mineral_cost;
        human_build_.supply_build_time = hp.supply_build_time;
        human_build_.producer_proto = hp.producer;
        human_build_.producer_proto.speed = sim::Fixed();
        human_build_.producer_build_time = hp.producer_build_time;
    }

    if (cfg_.human_player >= 0 && size_t(cfg_.human_player) < setup.players.size()) {
        const MeleePos& st = setup.players[size_t(cfg_.human_player)].start;
        {
            const float fx = float(st.x.to_double()), fy = float(st.y.to_double());
            camctl_.focus = {fx, fy, ground_z_at(fx, fy)};
        }
        framed_ = true;
    } else {
        render::Vec3 c{0, 0, 0};
        int n = 0;
        for (const Unit& u : world_.units()) {
            c.x += float(u.x.to_double());
            c.y += float(u.y.to_double());
            ++n;
        }
        if (n) {
            const float fx = c.x / float(n), fy = c.y / float(n);
            camctl_.focus = {fx, fy, ground_z_at(fx, fy)};
            camctl_.distance = 40.0f;
        }
    }

    replay_log_ = OrderLog{};
    replay_log_.seed = world_.seed();
    replay_log_.initial_units.assign(world_.units().begin(), world_.units().end());
    replay_recorder_ = std::make_unique<LogRecorder>(replay_log_);
    world_.set_order_sink(replay_recorder_.get());

    if (cfg_.human_player >= 0) start_music(race_music_id(human_race()));
}

void MeleeGame::begin_replay(const OrderLog& log) {
    if (log.seed != world_.seed()) return;
    replay_source_ = log;
    replay_cursor_ = 0;
    replay_total_ = log.total_ticks;
    replay_drive_ = true;
    world_.set_order_sink(nullptr);
}

void MeleeGame::inject_replay_orders(int tick) {
    while (replay_cursor_ < replay_source_.orders.size() &&
           replay_source_.orders[replay_cursor_].tick <= tick)
        apply_timed_order(world_, replay_source_.orders[replay_cursor_++]);
}

ReplayTrace MeleeGame::run_replay_to_end() {
    ReplayTrace trace;
    if (!replay_drive_) return trace;
    trace.hashes.reserve(std::size_t(replay_total_ > 0 ? replay_total_ : 0));
    while (world_.tick() < replay_total_) {
        inject_replay_orders(world_.tick());
        world_.step();
        if (research_) research_->apply_completed();
        trace.hashes.push_back(world_.state_hash());
    }
    return trace;
}

void MeleeGame::enable_research(const data::Catalog& cat, const data::UpgradeCatalog& up,
                                std::vector<MeleeResearchDriver::ProtoNames> proto_names) {
    research_ = std::make_unique<MeleeResearchDriver>(cat, up);
    research_->set_world(&world_);
    cat_ = &cat;
    if (auto v = cat.tag_default_scalar("CGame", "MixedFormationDiameter"); v && !v->empty())
        formation_diameter_[0] = float(std::strtod(v->c_str(), nullptr));
    if (auto v = cat.tag_default_scalar("CGame", "AirFormationDiameter"); v && !v->empty())
        formation_diameter_[1] = float(std::strtod(v->c_str(), nullptr));
    for (std::size_t p = 0; p < ais_.size(); ++p) {
        MeleeResearchDriver::ProtoNames names =
            p < proto_names.size() ? proto_names[p] : MeleeResearchDriver::ProtoNames{};
        research_->bind(*ais_[p], std::move(names));
    }
}

void MeleeGame::bind_actor_rules(const data::Catalog& cat) {
    auto convert = [](const std::vector<data::Catalog::ActorRule>& rows) {
        std::vector<render::ActorEventRule> out;
        out.reserve(rows.size());
        for (const data::Catalog::ActorRule& r : rows) {
            render::ActorEventRule er;
            er.head = r.head;
            er.subject = r.subject;
            er.sub = r.sub;
            er.filters = r.filters;
            er.verb = r.verb;
            er.args = r.args;
            out.push_back(std::move(er));
        }
        return out;
    };
    driver_.unit_rules = [&cat, convert](const std::string& id) {
        static std::unordered_map<std::string, std::vector<render::ActorEventRule>> cache;
        auto it = cache.find(id);
        if (it == cache.end()) it = cache.emplace(id, convert(cat.actor_rules(id))).first;
        return it->second;
    };
    driver_.sub_actor_rules = [&cat, convert](const std::string& id) {
        static std::unordered_map<std::string, std::vector<render::ActorEventRule>> cache;
        auto it = cache.find(id);
        if (it == cache.end()) {
            std::vector<render::ActorEventRule> rules = convert(cat.actor_rules(id, "CActorSound"));
            if (rules.empty()) rules = convert(cat.actor_rules(id, "CActorModel"));
            it = cache.emplace(id, std::move(rules)).first;
        }
        return it->second;
    };
    driver_.harvest_ability = [&cat](const std::string& id) {
        static std::unordered_map<std::string, std::string> cache;
        auto it = cache.find(id);
        if (it != cache.end()) return it->second;
        std::string found;
        for (const std::string& ab : cat.array_links("CUnit", id, "AbilArray")) {
            if (ab.empty() || !cat.is_harvest_ability(ab)) continue;
            found = ab;
            break;
        }
        return cache.emplace(id, found).first->second;
    };
    driver_.burrow_pair = [&cat](const std::string& id) {
        static std::unordered_map<std::string, render::ActorDriver::BurrowPair> cache;
        auto it = cache.find(id);
        if (it != cache.end()) return it->second;
        render::ActorDriver::BurrowPair bp;
        bp.down_ability = cat.burrow_ability(id);
        if (!bp.down_ability.empty()) {
            bp.variant = cat.burrowed_variant(id);
            if (auto k = cat.scalar("CAbil", bp.down_ability, "ActorKey")) bp.down_key = *k;
            if (auto ms = cat.morph_spec(bp.down_ability)) bp.down_ms = ms->morph_time * 1000.0;
            if (!bp.variant.empty()) {
                bp.up_ability = cat.unburrow_ability(bp.variant);
                if (!bp.up_ability.empty()) {
                    if (auto k = cat.scalar("CAbil", bp.up_ability, "ActorKey")) bp.up_key = *k;
                    if (auto ms = cat.morph_spec(bp.up_ability)) bp.up_ms = ms->morph_time * 1000.0;
                }
            }
        }
        return cache.emplace(id, bp).first->second;
    };
    driver_.depot_pair = [&cat](const std::string& id) {
        static std::unordered_map<std::string, render::ActorDriver::BurrowPair> cache;
        auto it = cache.find(id);
        if (it == cache.end()) it = cache.emplace(id, depot_morph_pair(cat, id)).first;
        return it->second;
    };
}

render::ActorDriver::BurrowPair depot_morph_pair(const data::Catalog& cat, const std::string& unit_id) {
    render::ActorDriver::BurrowPair dp;
    dp.down_ability = cat.depot_lower_ability(unit_id);
    if (dp.down_ability.empty()) return dp;
    if (auto ms = cat.morph_spec(dp.down_ability)) {
        dp.variant = ms->target;
        dp.down_ms = ms->morph_time * 1000.0;
    }
    if (dp.variant.empty()) return dp;
    dp.up_ability = cat.morph_button_ability(dp.variant, "Raise");
    if (!dp.up_ability.empty())
        if (auto ms = cat.morph_spec(dp.up_ability)) dp.up_ms = ms->morph_time * 1000.0;
    return dp;
}

void MeleeGame::register_creep_masks(const data::Catalog& cat) {
    for (const std::string& id : cat.ids("CUnit")) {
        auto mask = cat.creep_source_footprint_offsets(id);
        if (mask.empty()) mask = cat.toggled_creep_footprint_offsets(id);
        if (!mask.empty()) world_.register_creep_mask(id, std::move(mask));
    }
}

void MeleeGame::enable_larva(const data::Catalog& cat) {
    std::unordered_map<std::string, int32_t> proto_idx;
    for (const MeleePlayerLayout& pl : layout_.players) {
        const std::uint32_t hall = pl.town_hall;
        if (hall >= world_.units().size() || !world_.unit(hall).alive) continue;
        const std::string hid = world_.unit(hall).catalog_id;
        if (hid.empty()) continue;
        const auto sb = cat.unit_spawn_behavior(hid);
        if (!sb) continue;
        auto it = proto_idx.find(sb->unit);
        int32_t idx;
        if (it == proto_idx.end()) {
            idx = world_.add_spawn_proto(make_unit(cat, sb->unit, 0));
            if (!sb->offsets.empty()) world_.set_spawn_offsets(idx, spawn_slots_of(*sb));
            proto_idx.emplace(sb->unit, idx);
        } else {
            idx = it->second;
        }
        world_.begin_spawner(hall, idx, sb->max_count, sim::Fixed::from_double(sb->period),
                             sb->hard_cap);
    }
    if (!proto_idx.empty())
        for (const std::string& hid : cat.ids("CUnit"))
            if (const auto sb = cat.unit_spawn_behavior(hid))
                if (auto it = proto_idx.find(sb->unit); it != proto_idx.end())
                    world_.register_spawner_type(hid, it->second, sb->max_count, sim::Fixed::from_double(sb->period), sb->hard_cap);
    if (!proto_idx.empty())
        world_.set_larva_egg_proto(world_.add_spawn_proto(make_unit(cat, "Egg", 0)));
}

void MeleeGame::begin_research(int player, const std::string& id, sim::Fixed delay) {
    if (research_) research_->begin(player, id, delay);
}

void MeleeGame::maybe_ai_research(int player) {
    if (!research_ || !cat_ || player < 0 || std::size_t(player) >= ai_research_.size()) return;
    const AiResearchPlan& plan = ai_research_[player];
    if (plan.tech_id.empty() || plan.upgrades.empty()) return;
    std::string next;
    for (const std::string& tier : plan.upgrades) {
        if (research_->has(player, tier)) continue;
        if (research_->pending(player, tier)) return;
        next = tier;
        break;
    }
    if (next.empty()) return;
    bool tech_stands = false;
    for (const sim::Unit& u : world_.units())
        if (u.alive && u.player == player && !u.constructing && u.catalog_id == plan.tech_id) {
            tech_stands = true;
            break;
        }
    if (!tech_stands) return;
    const data::Catalog::ResearchInfo info = cat_->research_info(next);
    if (!info.found || info.time <= 0) return;
    sim::Player& pp = world_.player(player);
    if (pp.minerals < info.minerals || pp.gas < info.gas) return;
    pp.minerals -= info.minerals;
    pp.gas -= info.gas;
    research_->begin(player, next, sim::Fixed::from_double(info.time));
}

const std::vector<std::string>& MeleeGame::researched(int player) const {
    static const std::vector<std::string> kNone{};
    return research_ ? research_->researched(player) : kNone;
}

int MeleeGame::research_in_progress() const {
    return research_ ? research_->in_progress() : 0;
}

void MeleeGame::enable_collapsible_debris(const data::Catalog& cat) {
    if (world_.conjoined_count() == 0) return;
    sim::Unit debris = make_unit(cat, "CollapsibleRockTowerDebris", 0);
    debris.player = -1;
    debris.alive = true;
    world_.set_conjoined_debris_proto(world_.add_spawn_proto(std::move(debris)));
}

void MeleeGame::enable_broodlings(const data::Catalog& cat) {
    std::unordered_map<std::string, int32_t> proto_idx;
    for (const std::string& id : cat.ids("CUnit")) {
        const auto spec = cat.attack_spawn_of(id);
        if (!spec) continue;
        auto it = proto_idx.find(spec->unit);
        int32_t idx;
        if (it == proto_idx.end()) {
            sim::Unit brood = make_unit(cat, spec->unit, 0);
            brood.alive = true;
            brood.time_life = sim::Fixed::from_double(spec->life);
            idx = world_.add_spawn_proto(std::move(brood));
            proto_idx.emplace(spec->unit, idx);
        } else {
            idx = it->second;
        }
        world_.register_attack_spawn(id, idx, spec->count);
    }
}

void MeleeGame::enable_interceptors(const data::Catalog& cat) {
    std::unordered_map<std::string, int32_t> proto_idx;
    for (const std::string& id : cat.ids("CUnit")) {
        const auto spec = cat.interceptor_magazine_of(id);
        if (!spec || spec->count <= 0 || spec->count_max <= 0) continue;
        if (cat.attack_spawn_of(id)) continue;
        auto it = proto_idx.find(spec->unit);
        int32_t idx;
        if (it == proto_idx.end()) {
            sim::Unit ic = make_unit(cat, spec->unit, 0);
            ic.alive = true;
            idx = world_.add_spawn_proto(std::move(ic));
            proto_idx.emplace(spec->unit, idx);
        } else {
            idx = it->second;
        }
        world_.register_interceptor_arm(id, idx, spec->count, spec->count_max,
                                        sim::Fixed::from_double(spec->leash),
                                        sim::Fixed::from_double(spec->build_time));
    }
}

void MeleeGame::enable_novas(const data::Catalog& cat) {
    for (const std::string& id : cat.ids("CUnit")) {
        const auto spec = cat.nova_launch_of(id);
        if (!spec || spec->speed <= 0 || spec->damage <= 0 || spec->radius <= 0 || spec->duration <= 0)
            continue;
        const sim::Fixed cd = ability_cooldown_sec(cat, spec->ability);
        world_.register_nova_launch(id, sim::Fixed::from_double(spec->speed),
                                    sim::Fixed::from_double(spec->radius),
                                    sim::Fixed::from_double(spec->damage),
                                    sim::Fixed::from_double(spec->shield_bonus),
                                    sim::Fixed::from_double(spec->duration), cd);
        nova_ability_by_unit_[id] = spec->ability;
    }
}

void MeleeGame::enable_ai_casting(const data::Catalog& cat,
                                  const std::vector<std::pair<std::string, std::string>>& regs) {
    cast_policy_ = std::make_unique<CastPolicy>(cat);
    cat_ = &cat;
    for (const auto& [unit_id, abil_id] : regs)
        cast_policy_->register_offensive(world_, unit_id, abil_id);
}

int MeleeGame::enable_ai_casting_from_roster(const data::Catalog& cat) {
    cast_policy_ = std::make_unique<CastPolicy>(cat);
    cat_ = &cat;
    int registered = 0;
    for (std::size_t i = 0; i < kNumCasterCompositions; ++i) {
        const CasterComposition& c = kCasterCompositions[i];
        const int before = registered;
        std::set<std::string> seen;
        for (const auto& [abil_id, cmd_index] : cat.unit_ability_commands(c.caster)) {
            (void)cmd_index;
            if (abil_id.empty() || !seen.insert(abil_id).second) continue;
            const auto* ce = cat.find("CAbil", abil_id);
            if (!ce || ce->tag != "CAbilEffectTarget") continue;
            if (cast_policy_->register_offensive(world_, c.caster, abil_id)) ++registered;
        }
        if (registered == before && cast_policy_->register_offensive(world_, c.caster, c.ability))
            ++registered;
    }
    struct SelfBuff {
        const char* unit;
        const char* abil;
    };
    static const SelfBuff kSelfBuffs[] = {{"Marine", "Stimpack"}, {"Marauder", "StimpackMarauder"}};
    for (const SelfBuff& sb : kSelfBuffs)
        if (cast_policy_->register_selfcast(world_, sb.unit, sb.abil, sim::Fixed::from_int(7)))
            ++registered;
    auto authored_trigger_range = [&cat](const char* abil) {
        if (const auto ac = cat.scalar("CAbil", abil, "AutoCastRange"); ac && !ac->empty())
            return sim::Fixed::parse(*ac);
        const auto r = cat.scalar("CAbil", abil, "Range");
        return (r && !r->empty()) ? sim::Fixed::parse(*r) : sim::Fixed();
    };
    if (cast_policy_->register_support(world_, "Medivac", "MedivacHeal",
                                       authored_trigger_range("MedivacHeal")))
        ++registered;
    if (cast_policy_->register_support(world_, "Queen", "Transfusion",
                                       authored_trigger_range("Transfusion")))
        ++registered;
    if (cast_policy_->register_support(world_, "ShieldBattery", "ShieldBatteryRechargeEx5",
                                       authored_trigger_range("ShieldBatteryRechargeEx5")))
        ++registered;
    return registered;
}

std::uint32_t MeleeGame::human_lead_caster() const {
    {
        const std::uint32_t lead = sel_groups_.active_lead();
        if (lead < world_.units().size() && world_.units()[lead].alive && world_.units()[lead].player == cfg_.human_player &&
            std::find(selection_.begin(), selection_.end(), lead) != selection_.end())
            return lead;
    }
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (u.alive && u.player == cfg_.human_player) return id;
    }
    return sim::kNoTarget;
}

bool MeleeGame::toggle_autocast(std::uint32_t unit, const std::string& abil_id) {
    if (!cast_policy_ || !cat_) return false;
    if (!cat_->ability_autocast(abil_id).offered) return false;
    return cast_policy_->toggle_autocast(world_, unit, abil_id);
}

bool MeleeGame::autocast_state(std::uint32_t unit, const std::string& abil_id) {
    if (!cast_policy_) return false;
    return cast_policy_->autocast_state(world_, unit, abil_id);
}

bool MeleeGame::human_cast(const std::string& abil_id, std::uint32_t target_id) {
    if (!cast_policy_) return false;
    const std::uint32_t caster = human_lead_caster();
    if (caster == sim::kNoTarget) return false;
    return cast_policy_->cast_now(world_, caster, abil_id, target_id);
}

bool MeleeGame::human_cast_point(const std::string& abil_id, sim::Fixed px, sim::Fixed py) {
    if (!cast_policy_) return false;
    const std::uint32_t caster = human_lead_caster();
    if (caster == sim::kNoTarget) return false;
    return cast_policy_->cast_now_point(world_, caster, abil_id, px, py);
}

std::vector<AbilityCardEntry> MeleeGame::selection_ability_entries() const {
    if (!cat_) return {};
    const std::uint32_t lead = human_lead_caster();
    if (lead == sim::kNoTarget) return {};
    const std::string& type = world_.units()[lead].catalog_id;
    if (type.empty()) return {};
    std::vector<AbilityCardEntry> out =
        build_ability_card_entries(*cat_, type, human_tech_state(lead));
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const AbilityCardEntry& e) {
                                 return unpowered_offer_blocked(lead, e.ability);
                             }),
              out.end());
    return out;
}

std::vector<AbilityCardEntry> MeleeGame::merge_selection_abilities(CommandCard& card) const {
    std::vector<AbilityCardEntry> out;
    if (!cat_) return out;
    std::vector<AbilityCardEntry> entries = selection_ability_entries();
    if (entries.empty()) return out;
    auto occupied = [&](int s) {
        if (s < 0 || s >= CommandCard::kSlots) return true;
        if (s == CommandCard::kCancelSlot)
            return card.slots[s].state != ButtonState::Empty && !card.slots[s].is_cancel;
        return card.slots[s].state != ButtonState::Empty;
    };
    std::string burrow_ab, unburrow_ab;
    if (!selection_.empty()) {
        const std::uint32_t lead = selection_.front();
        if (lead < world_.units().size() && !world_.units()[lead].catalog_id.empty()) {
            const std::string& lid = world_.units()[lead].catalog_id;
            burrow_ab = cat_->burrow_ability(lid);
            const std::string var = cat_->burrowed_variant(lid);
            unburrow_ab = var.empty() ? cat_->unburrow_ability(lid) : cat_->unburrow_ability(var);
            if (unburrow_ab.empty()) unburrow_ab = cat_->unburrow_ability(lid);
        }
    }
    std::vector<int> claimed_cells;
    for (AbilityCardEntry& e : entries) {
        if (e.card != card_page_) continue;
        if (e.slot >= 0) {
            bool taken = false;
            for (int c : claimed_cells)
                if (c == e.slot) { taken = true; break; }
            if (taken) continue;
            if (!e.ability.empty()) {
                auto akey = cat_->scalar("CAbil", e.ability, "ActorKey");
                if (akey && (*akey == "BurrowDown" || *akey == "BurrowUp" || *akey == "LiftOffLand"))
                    continue;
            }
            if (e.slot >= 0 && e.slot < CommandCard::kSlots && !e.icon.empty() &&
                card.slots[e.slot].icon == e.icon)
                continue;
            claimed_cells.push_back(e.slot);
        }
        if (e.slot < 0 || occupied(e.slot)) continue;
        const int slot = e.slot;
        CommandButton btn;
        btn.icon = e.icon;
        btn.name = e.name;
        btn.hotkey = e.hotkey;
        btn.hotkey = e.hotkey;
        btn.ability = 0;
        btn.state = resolve_button_state(e.requirement_met, 0.0f);
        card.slots[slot] = btn;
        out.push_back(e);
    }
    return out;
}

std::vector<AbilityCardEntry> MeleeGame::selection_card_abilities() {
    return command_card_layout().abilities;
}

MinimapExtent MeleeGame::minimap_extent() const {
    MinimapExtent e;
    if (playable_x1_ > playable_x0_ && playable_y1_ > playable_y0_) {
        e.x0 = playable_x0_; e.y0 = playable_y0_; e.x1 = playable_x1_; e.y1 = playable_y1_;
    } else {
        e.x1 = map_w_; e.y1 = map_h_;
    }
    return e;
}

void MeleeGame::surrender(int player) {
    if (player < 0 || surrendered_player_ >= 0 || outcome_ != MatchResult::Outcome::Ongoing) return;
    surrendered_player_ = player;
    const int viewer = cfg_.human_player >= 0 ? cfg_.human_player : 0;
    outcome_ = player == viewer ? MatchResult::Outcome::Defeat : MatchResult::Outcome::Victory;
}

int card_slot_for_key(const CommandCard& card, char key) {
    if (!key) return -1;
    const char up = (key >= 'a' && key <= 'z') ? char(key - 'a' + 'A') : key;
    for (int i = 0; i < CommandCard::kSlots; ++i) {
        const CommandButton& b = card.slots[i];
        if (b.state == ButtonState::Empty || b.hotkey.empty()) continue;
        const char h = b.hotkey[0];
        const char hu = (h >= 'a' && h <= 'z') ? char(h - 'a' + 'A') : h;
        if (hu == up) return i;
    }
    return -1;
}

MeleeGame::CardLayout MeleeGame::command_card_layout() {
    CardLayout l;
    l.card = compose_command_card(l.builds, l.abilities, l.trains, l.submenus);
    return l;
}

std::vector<MeleeGame::SubmenuCardEntry> MeleeGame::selection_submenu_card() {
    return command_card_layout().submenus;
}

std::vector<MeleeGame::SubmenuCardEntry> MeleeGame::merge_selection_submenus(CommandCard& card) {
    std::vector<SubmenuCardEntry> out;
    if (!cat_ || cfg_.human_player < 0) return out;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.catalog_id.empty()) continue;
        const auto cards = cat_->unit_command_cards(u.catalog_id);
        for (const auto& c : cards) {
            if (int(c.index) != card_page_) continue;
            for (const auto& sl : c.slots) {
                if (sl.type == "Passive") {
                    if (sl.row < 0 || sl.col < 0) continue;
                    const int slot = sl.row * CommandCard::kCols + sl.col;
                    if (slot < 0 || slot >= CommandCard::kSlots) continue;
                    if (!sl.requirements.empty()) {
                        const data::TechState st = human_tech_state(id);
                        if (!data::eval_requirement(*cat_, sl.requirements, "Use", st)) continue;
                    }
                    CommandButton btn;
                    if (auto ic = cat_->button_icon(sl.face)) btn.icon = std::move(*ic);
                    if (auto hk = cat_->button_hotkey(sl.face)) btn.hotkey = std::move(*hk);
                    if (auto nm = cat_->button_display_name(sl.face)) btn.name = std::move(*nm);
                    if (btn.name.empty()) btn.name = sl.face;
                    btn.ability = 0;
                    btn.state = ButtonState::Enabled;
                    btn.is_passive = true;
                    card.slots[slot] = btn;
                    continue;
                }
                if (sl.type == "SelectLarva") {
                    if (sl.row < 0 || sl.col < 0) continue;
                    const int slot = sl.row * CommandCard::kCols + sl.col;
                    if (slot < 0 || slot >= CommandCard::kSlots) continue;
                    bool any = false;
                    for (const sim::Unit& l : world_.units())
                        if (l.alive && l.spawn_parent == id) { any = true; break; }
                    if (!any) continue;
                    CommandButton btn;
                    if (auto ic = cat_->button_icon(sl.face)) btn.icon = std::move(*ic);
                    if (auto hk = cat_->button_hotkey(sl.face)) btn.hotkey = std::move(*hk);
                    if (auto nm = cat_->button_display_name(sl.face)) btn.name = std::move(*nm);
                    if (btn.name.empty()) btn.name = sl.face;
                    btn.ability = 0;
                    btn.state = ButtonState::Enabled;
                    btn.select_larva = id;
                    card.slots[slot] = btn;
                    continue;
                }
                if (sl.type == "SelectBuilder") {
                    if (sl.row < 0 || sl.col < 0) continue;
                    const int slot = sl.row * CommandCard::kCols + sl.col;
                    if (slot < 0 || slot >= CommandCard::kSlots) continue;
                    std::uint32_t builder = sim::kNoTarget;
                    for (const sim::Unit& w : world_.units())
                        if (w.alive && w.construct_target == id) {
                            builder = w.id;
                            break;
                        }
                    if (builder == sim::kNoTarget) continue;
                    CommandButton btn;
                    if (auto ic = cat_->button_icon(sl.face)) btn.icon = std::move(*ic);
                    if (auto hk = cat_->button_hotkey(sl.face)) btn.hotkey = std::move(*hk);
                    if (auto nm = cat_->button_display_name(sl.face)) btn.name = std::move(*nm);
                    if (btn.name.empty()) btn.name = sl.face;
                    btn.ability = 0;
                    btn.state = ButtonState::Enabled;
                    btn.select_builder = builder;
                    card.slots[slot] = btn;
                    continue;
                }
                if (sl.type != "Submenu" && sl.type != "CancelSubmenu") continue;
                if (sl.row < 0 || sl.col < 0) continue;
                SubmenuCardEntry e;
                e.slot = sl.row * CommandCard::kCols + sl.col;
                e.target_page =
                    sl.type == "CancelSubmenu" ? 0 : card_page_of_submenu(sl.submenu_id);
                if (auto nm = cat_->button_display_name(sl.face)) e.name = std::move(*nm);
                if (e.name.empty()) e.name = sl.face;
                if (auto ic = cat_->button_icon(sl.face)) e.icon = std::move(*ic);
                if (auto hk = cat_->button_hotkey(sl.face)) e.hotkey = std::move(*hk);
                if (e.slot < 0 || e.slot >= CommandCard::kSlots) continue;
                CommandButton btn;
                btn.icon = e.icon;
                btn.name = e.name;
                btn.ability = 0;
                btn.state = ButtonState::Enabled;
                card.slots[e.slot] = btn;
                out.push_back(std::move(e));
            }
            break;
        }
        break;
    }
    return out;
}

CommandCard MeleeGame::compose_command_card(std::vector<BuildCardEntry>& builds,
                                            std::vector<AbilityCardEntry>& abils,
                                            std::vector<TrainCardEntry>& trains,
                                            std::vector<SubmenuCardEntry>& submenus) {
    if (card_page_ != 0 && !lead_authors_card_page(card_page_)) card_page_ = 0;
    CommandCard card =
        card_page_ == 0 ? build_melee_command_card(command_context()) : CommandCard{};
    submenus = merge_selection_submenus(card);
    builds = merge_selection_build(card);
    abils = merge_selection_abilities(card);
    trains = merge_selection_train(card);
    int authored_cancel = -1;
    for (int i = 0; i < CommandCard::kSlots; ++i) {
        if (card.slots[i].is_cancel || card.slots[i].state == ButtonState::Empty) continue;
        std::string ic = card.slots[i].icon;
        for (char& ch : ic) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (ic.find("btn-command-cancel") != std::string::npos) { authored_cancel = i; break; }
    }
    if (authored_cancel >= 0) {
        card.slots[authored_cancel].is_cancel = true;
        if (authored_cancel != CommandCard::kCancelSlot &&
            card.slots[CommandCard::kCancelSlot].is_cancel)
            card.slots[CommandCard::kCancelSlot] = CommandButton{};
    }

    if (selected_constructing_building() != sim::kNoTarget ||
        selected_training_producer() != sim::kNoTarget) {
        if (authored_cancel >= 0) card.slots[authored_cancel].is_cancel = true;
        else card.set_cancel(command_context().cancel_icon);
    }
    return card;
}

std::optional<TooltipInfo> MeleeGame::hovered_card_tooltip(std::uint32_t view_w, std::uint32_t view_h,
                                                           float mx, float my,
                                                           const ConsolePanel* console) {
    if (cfg_.human_player < 0 || !view_w || !view_h) return std::nullopt;
    std::vector<BuildCardEntry> builds;
    std::vector<AbilityCardEntry> abils;
    std::vector<TrainCardEntry> trains;
    std::vector<SubmenuCardEntry> submenus;
    const CommandCard card = compose_command_card(builds, abils, trains, submenus);
    for (int i = 0; i < CommandCard::kSlots; ++i) {
        const CommandButton& btn = card.slots[i];
        if (btn.state == ButtonState::Empty) continue;
        const HudRect c = hud_command_cell(view_w, view_h, i, console);
        if (c.w() <= 0) continue;
        if (mx < float(c.x0) || mx >= float(c.x1) || my < float(c.y0) || my >= float(c.y1)) continue;
        TooltipInfo t;
        t.name = btn.name;
        t.hotkey = btn.hotkey;
        for (const TrainCardEntry& e : trains)
            if (e.slot == i) {
                t.minerals = e.minerals;
                t.gas = e.gas;
                t.build_time = float(e.time.to_double());
                break;
            }
        for (const BuildCardEntry& e : builds)
            if (e.slot == i) {
                t.minerals = e.minerals;
                t.gas = e.gas;
                t.build_time = float(e.time.to_double());
                break;
            }
        if (btn.state == ButtonState::Disabled) {
            bool tech_met = true, found = false;
            for (const TrainCardEntry& e : trains)
                if (e.slot == i) { tech_met = e.tech_met; found = true; break; }
            if (!found)
                for (const BuildCardEntry& e : builds)
                    if (e.slot == i) { tech_met = e.tech_met; found = true; break; }
            if (!tech_met) {
                t.requirement = "requirements not met";
            } else if (cfg_.human_player >= 0) {
                const sim::Player& hp = world_.player(cfg_.human_player);
                if (hp.minerals < t.minerals && hp.gas < t.gas)
                    t.requirement = "not enough resources";
                else if (hp.minerals < t.minerals)
                    t.requirement = "not enough minerals";
                else if (hp.gas < t.gas)
                    t.requirement = "not enough vespene gas";
            }
        }
        return t;
    }
    return std::nullopt;
}

static std::int32_t addon_kind_of(const data::Catalog& cat, const std::string& unit_id) {
    if (unit_id.empty()) return sim::World::kAddonNone;
    for (const std::string& a : cat.array_links("CUnit", unit_id, "TechAliasArray")) {
        if (a == "Alias_TechLab") return sim::World::kAddonTechLab;
        if (a == "Alias_Reactor") return sim::World::kAddonReactor;
    }
    return sim::World::kAddonNone;
}

sim::World::AddonBodyFactory addon_body_factory(const data::Catalog& cat) {
    return [&cat](const sim::Unit& host, std::int32_t kind, sim::Unit& body) {
        if (host.catalog_id.empty()) return false;
        for (const data::Catalog::BuildProduct& bp : cat.unit_build_products(host.catalog_id))
            if (addon_kind_of(cat, bp.unit) == kind) {
                body = make_unit(cat, bp.unit, host.player);
                return true;
            }
        return false;
    };
}

data::TechState MeleeGame::tech_state_of(int player, std::uint32_t producer) const {
    data::TechState st;
    for (const std::string& up : world_.researched(player))
        if (!up.empty()) st.upgrades[up] = 1;
    if (player >= 0) world_.pending_research_by_type(player, st.upgrades_queued);
    if (player >= 0)
        for (const sim::Unit& u : world_.units()) {
            if (!u.alive || u.player != player || u.catalog_id.empty()) continue;
            if (u.constructing) ++st.units_queued[u.catalog_id];
            else ++st.units_complete[u.catalog_id];
        }
    if (player >= 0) world_.pending_by_type(player, st.units_queued);
    if (cat_ && producer < world_.units().size()) {
        const sim::Unit& p = world_.units()[producer];
        const std::int32_t kind = world_.addon_of(producer);
        if (kind != sim::World::kAddonNone && !p.catalog_id.empty())
            for (const data::Catalog::BuildProduct& bp : cat_->unit_build_products(p.catalog_id))
                if (addon_kind_of(*cat_, bp.unit) == kind) { ++st.at_unit[bp.unit]; break; }
        if (p.addon_building != 0 && !p.catalog_id.empty())
            for (const data::Catalog::BuildProduct& bp : cat_->unit_build_products(p.catalog_id))
                if (addon_kind_of(*cat_, bp.unit) == p.addon_building) {
                    ++st.at_unit_queued[bp.unit];
                    break;
                }
        for (const sim::BehaviorInstance& b : p.behaviors) {
            if (b.src == 0) continue;
            const std::string id = behavior_id_of_tag(b.src);
            if (id.empty()) continue;
            ++st.at_unit[id];
            ++st.behaviors[id];
        }
    }
    return st;
}

void MeleeGame::wire_addon_bodies() {
    if (cat_) world_.set_addon_body_factory(addon_body_factory(*cat_));
}

void MeleeGame::enable_ai_tech_gate(const data::Catalog& cat) {
    cat_ = &cat;
    world_.set_train_gate([this](std::uint32_t producer, const std::string& unit_id) {
        if (!cat_ || producer >= world_.units().size()) return true;
        const sim::Unit& p = world_.units()[producer];
        if (p.player == cfg_.human_player || p.catalog_id.empty()) return true;
        for (const data::Catalog::TrainProduct& tp : cat_->unit_train_products(p.catalog_id))
            if (tp.unit == unit_id)
                return production_.can_produce(*cat_, tp.ability, unit_id, tech_state_of(p.player, producer));
        return true;
    });
}

const std::string& MeleeGame::behavior_id_of_tag(std::uint32_t tag) const {
    static const std::string kNone;
    if (!cat_) return kNone;
    if (behavior_tags_.empty())
        for (const std::string& id : cat_->ids("CBehavior")) behavior_tags_[behavior_id_hash(id)] = id;
    auto it = behavior_tags_.find(tag);
    return it == behavior_tags_.end() ? kNone : it->second;
}

static int construction_sequence(const formats::Model& m, double progress) {
    static const char* kStageLetters[] = {"A", "B", "C", "D", "E", "F"};
    std::vector<const char*> stages;
    for (const char* L : kStageLetters)
        if (render::select_sequence(m, {"Build", L}) >= 0) stages.push_back(L);
    if (stages.empty()) {
        return render::select_sequence(m, {"Morph"});
    }
    double p = progress;
    if (!(p >= 0.0)) p = 0.0;
    if (p > 1.0) p = 1.0;
    std::size_t idx = std::size_t(p * double(stages.size()));
    if (idx >= stages.size()) idx = stages.size() - 1;
    const char* L = stages[idx];
    const int hold = render::select_sequence(m, {"Build", L, "Stand"});
    if (hold >= 0) return hold;
    return render::select_sequence(m, {"Build", L, "Start"});
}

sim::BuildVerb MeleeGame::build_verb_for(std::uint32_t builder) const {
    if (!cat_ || builder >= world_.units().size()) return human_build_verb_;
    const std::string& id = world_.units()[builder].catalog_id;
    if (id.empty()) return human_build_verb_;
    return worker_build_verb(*cat_, id);
}

bool MeleeGame::unpowered_offer_blocked(std::uint32_t id, const std::string& ability) const {
    if (!cat_ || id >= world_.units().size()) return false;
    const sim::Unit& u = world_.units()[id];
    if (!sim::power_disabled(u) || u.catalog_id.empty()) return false;
    if (!cat_->disables_abils_unpowered(u.catalog_id)) return false;
    const std::vector<std::string> allow = cat_->unpowered_enabled_abilities(u.catalog_id);
    return std::find(allow.begin(), allow.end(), ability) == allow.end();
}

double MeleeGame::folded_train_time(const std::string& ability, const std::string& command,
                                    double base) const {
    if (!research_ || cfg_.human_player < 0) return base;
    const data::UpgradeCatalog* up = research_->upgrades();
    if (!up) return base;
    return data::fold_upgrade_field(*up, world_.researched(cfg_.human_player), "Abil", ability,
                                    "InfoArray[" + command + "].Time", base);
}

std::vector<MeleeGame::TrainCardEntry> MeleeGame::selection_train_entries() const {
    std::vector<TrainCardEntry> out;
    if (!cat_ || cfg_.human_player < 0) return out;
    std::uint32_t producer = sim::kNoTarget;
    std::vector<data::Catalog::TrainProduct> products;
    std::vector<data::Catalog::TrainProduct> warp_products;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing || u.catalog_id.empty()) continue;
        std::vector<data::Catalog::TrainProduct> ps = cat_->unit_train_products(u.catalog_id);
        const std::vector<data::Catalog::TrainProduct> ws = cat_->unit_warp_products(u.catalog_id);
        if (!ps.empty() || !ws.empty()) {
            producer = id;
            products = std::move(ps);
            warp_products = ws;
            break;
        }
    }
    if (producer == sim::kNoTarget) return out;
    const data::TechState st = human_tech_state(producer);
    const sim::Player& hp = world_.player(cfg_.human_player);
    for (const data::Catalog::TrainProduct& tp : products) {
        if (data::requirement_hidden(*cat_, tp.requirement, st)) continue;
        if (unpowered_offer_blocked(producer, tp.ability)) continue;
        TrainCardEntry e;
        e.producer = producer;
        e.unit = tp.unit;
        e.name = "Train " + tp.unit;
        if (auto nm = cat_->button_display_name(tp.face)) e.name = std::move(*nm);
        if (auto ic = cat_->button_icon(tp.face)) e.icon = std::move(*ic);
        if (auto hk = cat_->button_hotkey(tp.face)) e.hotkey = std::move(*hk);
        if (tp.row >= 0 && tp.col >= 0) e.slot = tp.row * 5 + tp.col;
        e.card = tp.card;
        const data::UnitCost c = data::unit_cost(*cat_, tp.unit);
        e.minerals = c.minerals;
        e.gas = c.gas;
        const double t = folded_train_time(tp.ability, tp.command, tp.time);
        e.time = t > 0 ? sim::Fixed::from_double(t) : sim::Fixed();
        const bool affordable = hp.minerals >= c.minerals && hp.gas >= c.gas;
        const bool req_ok = production_.can_produce(*cat_, tp.ability, tp.unit, st);
        e.tech_met = req_ok;
        e.requirement_met = affordable && e.tech_met;
        out.push_back(std::move(e));
    }
    for (const data::Catalog::TrainProduct& wp : warp_products) {
        if (data::requirement_hidden(*cat_, wp.requirement, st)) continue;
        if (unpowered_offer_blocked(producer, wp.ability)) continue;
        TrainCardEntry e;
        e.producer = producer;
        e.unit = wp.unit;
        e.warp = true;
        e.warp_charge = wp.warp_charge;
        e.name = "Warp in " + wp.unit;
        if (auto nm = cat_->button_display_name(wp.face)) e.name = std::move(*nm);
        if (auto ic = cat_->button_icon(wp.face)) e.icon = std::move(*ic);
        if (auto hk = cat_->button_hotkey(wp.face)) e.hotkey = std::move(*hk);
        if (wp.row >= 0 && wp.col >= 0) e.slot = wp.row * 5 + wp.col;
        e.card = wp.card;
        const data::UnitCost c = data::unit_cost(*cat_, wp.unit);
        e.minerals = c.minerals;
        e.gas = c.gas;
        e.time = wp.time > 0 ? sim::Fixed::from_double(wp.time) : sim::Fixed();
        const bool affordable = hp.minerals >= c.minerals && hp.gas >= c.gas;
        const bool req_ok = production_.can_produce(*cat_, wp.ability, wp.unit, st);
        e.tech_met = req_ok;
        const bool ready = world_.warp_ready(producer);
        e.requirement_met = affordable && req_ok && ready;
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<MeleeGame::TrainCardEntry> MeleeGame::merge_selection_train(CommandCard& card) const {
    std::vector<TrainCardEntry> out;
    std::vector<TrainCardEntry> entries = selection_train_entries();
    if (entries.empty()) return out;
    auto occupied = [&](int s) {
        if (s < 0 || s >= CommandCard::kSlots) return true;
        if (s == CommandCard::kCancelSlot)
            return card.slots[s].state != ButtonState::Empty && !card.slots[s].is_cancel;
        return card.slots[s].state != ButtonState::Empty;
    };
    for (TrainCardEntry& e : entries) {
        if (e.card != card_page_) continue;
        int slot = e.slot;
        if (slot < 0 || occupied(slot)) continue;
        e.slot = slot;
        CommandButton btn;
        btn.icon = e.icon;
        btn.name = e.name;
        btn.hotkey = e.hotkey;
        btn.ability = 0;
        btn.state = resolve_button_state(e.requirement_met, 0.0f);
        card.slots[slot] = btn;
        out.push_back(e);
    }
    return out;
}

std::vector<MeleeGame::TrainCardEntry> MeleeGame::selection_train_card() {
    return command_card_layout().trains;
}

bool MeleeGame::human_train(std::uint32_t producer, const std::string& unit_id) {
    if (!cat_ || unit_id.empty() || producer >= world_.units().size()) return false;
    const sim::Unit& p = world_.units()[producer];
    if (!p.alive || p.player != cfg_.human_player) return false;
    sim::Fixed time;
    bool trains_it = false;
    std::string train_ability;
    for (const data::Catalog::TrainProduct& tp : cat_->unit_train_products(p.catalog_id))
        if (tp.unit == unit_id) {
            const double ft = folded_train_time(tp.ability, tp.command, tp.time);
            time = ft > 0 ? sim::Fixed::from_double(ft) : sim::Fixed();
            train_ability = tp.ability;
            trains_it = true;
            break;
        }
    if (!trains_it) return false;
    if (unpowered_offer_blocked(producer, train_ability)) return false;
    if (!production_.can_produce(*cat_, train_ability, unit_id, human_tech_state(producer)))
        return false;
    const data::UnitCost c = data::unit_cost(*cat_, unit_id);
    sim::Unit proto = make_unit(*cat_, unit_id, cfg_.human_player);
    return MeleeCommands::train(world_, producer, proto, c.minerals, c.gas, time);
}

std::vector<MeleeGame::BuildCardEntry> MeleeGame::selection_build_entries() const {
    std::vector<BuildCardEntry> out;
    if (!cat_ || cfg_.human_player < 0) return out;
    std::uint32_t worker = sim::kNoTarget;
    std::vector<data::Catalog::BuildProduct> products;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing || u.catalog_id.empty())
            continue;
        std::vector<data::Catalog::BuildProduct> ps;
        for (data::Catalog::BuildProduct& bp : cat_->unit_build_products(u.catalog_id)) {
            const bool summon = cat_->build_spec(bp.ability).has_value();
            if (u.is_worker || bp.instant_placement || !summon) ps.push_back(std::move(bp));
        }
        if (!ps.empty()) { worker = id; products = std::move(ps); break; }
    }
    if (worker == sim::kNoTarget) return out;
    const data::TechState st = human_tech_state(worker);
    const sim::Player& hp = world_.player(cfg_.human_player);
    for (const data::Catalog::BuildProduct& bp : products) {
        if (data::requirement_hidden(*cat_, bp.requirement, st)) continue;
        if (unpowered_offer_blocked(worker, bp.ability)) continue;
        BuildCardEntry e;
        e.worker = worker;
        e.card = bp.card;
        e.unit = bp.unit;
        e.name = "Build " + bp.unit;
        if (auto nm = cat_->button_display_name(bp.face)) e.name = std::move(*nm);
        if (auto ic = cat_->button_icon(bp.face)) e.icon = std::move(*ic);
        if (bp.row >= 0 && bp.col >= 0) e.slot = bp.row * 5 + bp.col;
        const data::UnitCost c = data::unit_cost(*cat_, bp.unit);
        e.minerals = c.minerals;
        e.gas = c.gas;
        e.time = bp.time > 0 ? sim::Fixed::from_double(bp.time) : sim::Fixed();
        e.is_gas = bp.is_gas;
        const bool affordable = hp.minerals >= c.minerals && hp.gas >= c.gas;
        const bool req_ok = production_.can_produce(*cat_, bp.ability, bp.unit, st);
        e.tech_met = req_ok;
        e.requirement_met = affordable && req_ok;
        if (world_.units()[worker].build_charges >= 0 && !world_.build_charge_ready(worker)) e.requirement_met = false;
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<MeleeGame::BuildCardEntry> MeleeGame::merge_selection_build(CommandCard& card) const {
    std::vector<BuildCardEntry> out;
    std::vector<BuildCardEntry> entries = selection_build_entries();
    if (entries.empty()) return out;
    auto occupied = [&](int s) {
        if (s < 0 || s >= CommandCard::kSlots) return true;
        if (s == CommandCard::kCancelSlot)
            return card.slots[s].state != ButtonState::Empty && !card.slots[s].is_cancel;
        return card.slots[s].state != ButtonState::Empty;
    };
    for (BuildCardEntry& e : entries) {
        if (e.card != card_page_) continue;
        int slot = e.slot;
        if (slot < 0 || occupied(slot)) continue;
        e.slot = slot;
        CommandButton btn;
        btn.icon = e.icon;
        btn.name = e.name;
        btn.hotkey = e.hotkey;
        btn.ability = 0;
        btn.state = resolve_button_state(e.requirement_met, 0.0f);
        card.slots[slot] = btn;
        out.push_back(e);
    }
    return out;
}

bool MeleeGame::lead_authors_card_page(int page) const {
    if (!cat_) return false;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.catalog_id.empty()) continue;
        for (const auto& c : cat_->unit_command_cards(u.catalog_id))
            if (int(c.index) == page) return true;
        return false;
    }
    return false;
}

const std::vector<std::vector<std::string>>& MeleeGame::construction_ladder(const std::string& catalog_id) {
    auto it = construction_ladders_.find(catalog_id);
    if (it != construction_ladders_.end()) return it->second;
    std::vector<std::vector<std::string>> ladder;
    if (cat_ && !catalog_id.empty())
        ladder = cat_->actor_progress_stages(cat_->progress_actor_for_build(cat_->build_ability_of(catalog_id)));
    return construction_ladders_.emplace(catalog_id, std::move(ladder)).first->second;
}

int MeleeGame::card_page_of_submenu(const std::string& card_id) const {
    if (!cat_ || card_id.empty()) return 0;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.catalog_id.empty()) continue;
        const auto cards = cat_->unit_command_cards(u.catalog_id);
        for (std::size_t i = 0; i < cards.size(); ++i)
            if (cards[i].card_id == card_id) return int(cards[i].index);
        break;
    }
    return 0;
}

std::vector<MeleeGame::BuildCardEntry> MeleeGame::selection_build_card() {
    return command_card_layout().builds;
}

bool MeleeGame::human_warp_train(std::uint32_t producer, const std::string& unit_id, sim::Fixed tx,
                                 sim::Fixed ty) {
    if (!cat_ || unit_id.empty() || producer >= world_.units().size()) return false;
    const sim::Unit& p = world_.units()[producer];
    if (!p.alive || p.player != cfg_.human_player || p.constructing) return false;
    const data::Catalog::TrainProduct* found = nullptr;
    const std::vector<data::Catalog::TrainProduct> ws = cat_->unit_warp_products(p.catalog_id);
    for (const data::Catalog::TrainProduct& wp : ws)
        if (wp.unit == unit_id) { found = &wp; break; }
    if (!found) return false;
    if (unpowered_offer_blocked(producer, found->ability)) return false;
    if (!production_.can_produce(*cat_, found->ability, found->unit, human_tech_state(producer)))
        return false;
    const sim::Unit proto = app::make_unit(*cat_, unit_id, cfg_.human_player);
    const data::UnitCost c = data::unit_cost(*cat_, unit_id);
    const sim::Fixed forming =
        found->time > 0 ? sim::Fixed::from_double(found->time) : sim::Fixed();
    const sim::Fixed charge =
        found->warp_charge > 0 ? sim::Fixed::from_double(found->warp_charge) : sim::Fixed();
    return world_.warp_train(producer, proto, tx, ty, forming, charge, c.minerals, c.gas);
}

bool MeleeGame::human_build(std::uint32_t worker, const std::string& structure_unit_id) {
    if (!cat_ || structure_unit_id.empty() || worker >= world_.units().size()) return false;
    const sim::Unit& w = world_.units()[worker];
    if (!w.alive || w.player != cfg_.human_player) return false;
    sim::Fixed time;
    bool is_gas = false, builds_it = false, instant = false;
    std::string build_ability;
    for (const data::Catalog::BuildProduct& bp : cat_->unit_build_products(w.catalog_id))
        if (bp.unit == structure_unit_id) {
            time = bp.time > 0 ? sim::Fixed::from_double(bp.time) : sim::Fixed();
            is_gas = bp.is_gas;
            instant = bp.instant_placement;
            build_ability = bp.ability;
            builds_it = true;
            break;
        }
    if (!builds_it) return false;
    const bool charged = !instant && !w.is_worker && w.build_charges >= 0;
    if (!instant && !w.is_worker && !charged) return false;
    if (charged && !world_.build_charge_ready(worker)) return false;
    if (!production_.can_produce(*cat_, build_ability, structure_unit_id, human_tech_state(worker)))
        return false;
    pending_attack_ = pending_patrol_ = pending_move_ = false;
    cancel_ability_cast();
    if (instant) {
        const std::int32_t kind = addon_kind_of(*cat_, structure_unit_id);
        if (kind == sim::World::kAddonNone) return false;
        const data::UnitCost c = data::unit_cost(*cat_, structure_unit_id);
        sim::Player& hp = world_.player(cfg_.human_player);
        if (hp.minerals < c.minerals || hp.gas < c.gas) return false;
        if (!world_.build_addon(worker, kind, time)) return false;
        hp.minerals -= c.minerals;
        hp.gas -= c.gas;
        return true;
    }
    if (is_gas) {
        pending_refinery_ = true;
        pending_build_ = false;
        build_proto_ = make_unit(*cat_, structure_unit_id, cfg_.human_player);
        build_proto_.speed = sim::Fixed();
        build_cost_ = data::unit_cost(*cat_, structure_unit_id).minerals;
        build_gas_cost_ = data::unit_cost(*cat_, structure_unit_id).gas;
        build_time_ = time;
        return true;
    }
    pending_build_ = true;
    build_caster_ = charged ? worker : sim::kNoTarget;
    build_proto_ = make_unit(*cat_, structure_unit_id, cfg_.human_player);
    build_proto_.speed = sim::Fixed();
    build_cost_ = data::unit_cost(*cat_, structure_unit_id).minerals;
    build_gas_cost_ = data::unit_cost(*cat_, structure_unit_id).gas;
    build_time_ = time;
    return true;
}

bool MeleeGame::human_morph(std::uint32_t caster, const std::string& morph_abil) {
    if (!cat_ || morph_abil.empty() || caster >= world_.units().size()) return false;
    const sim::Unit& u = world_.units()[caster];
    if (!u.alive || u.player != cfg_.human_player) return false;
    const auto ms = cat_->morph_spec(morph_abil);
    if (!ms || ms->target.empty()) return false;
    auto delta = morph_cost_delta(*cat_, u.catalog_id, ms->target);
    if (const auto mc = cat_->morph_ability_cost(morph_abil); mc.found)
        delta = {mc.minerals, mc.gas};
    sim::Player& hp = world_.player(cfg_.human_player);
    if (hp.minerals < delta.first || hp.gas < delta.second) return false;
    const std::int32_t proto = world_.add_spawn_proto(make_unit(*cat_, ms->target, cfg_.human_player));
    world_.clear_zone(caster);
    const sim::Fixed t = ms->morph_time > 0 ? sim::Fixed::from_double(ms->morph_time) : sim::Fixed();
    const std::string fin = cat_->morph_finish_behavior(morph_abil);
    const std::uint32_t fin_tag =
        (!fin.empty() && cat_->behavior_is_permanent(fin)) ? behavior_id_hash(fin) : 0u;
    world_.begin_morph(caster, proto, t, fin_tag);
    hp.minerals -= delta.first;
    hp.gas -= delta.second;
    return true;
}

bool MeleeGame::morph_command_allowed(std::uint32_t body, const std::string& morph_abil) const {
    if (!cat_ || morph_abil.empty() || body >= world_.units().size()) return false;
    const sim::Unit& u = world_.units()[body];
    if (!u.alive || u.player != cfg_.human_player || u.constructing || u.catalog_id.empty())
        return false;
    bool carries = false;
    for (const std::string& ab : cat_->array_links("CUnit", u.catalog_id, "AbilArray"))
        if (ab == morph_abil) { carries = true; break; }
    if (!carries) return false;
    std::string req;
    bool found_execute = false;
    for (const data::Catalog::CommandCardLayout& c : cat_->unit_command_cards(u.catalog_id))
        for (const data::Catalog::CommandCardSlot& sl : c.slots) {
            if (sl.type != "AbilCmd" || sl.ability != morph_abil || sl.command.empty()) continue;
            const std::string r = cat_->button_requirement(sl.ability, sl.command);
            if (sl.command == "Execute") {
                req = r;
                found_execute = true;
            } else if (!found_execute && req.empty()) {
                req = r;
            }
        }
    if (req.empty()) return true;
    const data::TechState st = human_tech_state(body);
    return data::eval_requirement(*cat_, req, "Use", st) &&
           !data::requirement_hidden(*cat_, req, st);
}

int MeleeGame::human_morph_selection(const std::string& morph_abil) {
    if (!cat_ || morph_abil.empty() || cfg_.human_player < 0) return 0;
    int committed = 0;
    for (std::uint32_t id : selection_) {
        if (!morph_command_allowed(id, morph_abil)) continue;
        if (human_morph(id, morph_abil)) ++committed;
    }
    return committed;
}

bool MeleeGame::human_toggle_cloak(const std::string& abil_id, const std::string& command) {
    if (!cat_) return false;
    bool is_cloak = false;
    for (const std::string& beh : cat_->array_links("CAbil", abil_id, "BehaviorArray"))
        if (cat_->cloak_drain(beh)) {
            is_cloak = true;
            break;
        }
    if (!is_cloak) return false;

    const bool engage = command != "Off";
    const sim::Fixed cost = ability_energy_cost(*cat_, abil_id);

    bool any = false;
    if (cfg_.human_player < 0) return false;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        sim::Unit& u = world_.unit(id);
        if (!u.alive || u.player != cfg_.human_player || u.cloak_drain.raw() <= 0) continue;
        if (engage) {
            if (u.cloaked) continue;
            if (u.energy < cost) continue;
            if (!world_.set_cloak(id, true)) continue;
            u.energy = u.energy - cost;
        } else {
            if (!u.cloaked) continue;
            world_.set_cloak(id, false);
        }
        any = true;
    }
    return any;
}

bool MeleeGame::human_toggle_weapon(const std::string& abil_id, const std::string& command) {
    if (!cat_ || cfg_.human_player < 0) return false;
    bool enables_weapon = false;
    for (const std::string& beh : cat_->array_links("CAbil", abil_id, "BehaviorArray"))
        if (!cat_->behavior_weapon_enable(beh).empty()) {
            enables_weapon = true;
            break;
        }
    if (!enables_weapon) return false;

    const bool engage = command != "Off";
    const sim::Fixed cost = ability_energy_cost(*cat_, abil_id);

    bool any = false;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        sim::Unit& u = world_.unit(id);
        if (!u.alive || u.player != cfg_.human_player || !u.has_toggle_wpn) continue;
        if (engage) {
            if (u.toggle_wpn_on) continue;
            if (u.energy < cost) continue;
            if (!world_.set_weapon_toggle(id, true)) continue;
            u.energy = u.energy - cost;
        } else {
            if (!u.toggle_wpn_on) continue;
            world_.set_weapon_toggle(id, false);
        }
        any = true;
    }
    return any;
}

bool MeleeGame::human_toggle_creep(const std::string& abil_id, const std::string& command) {
    if (!cat_ || cfg_.human_player < 0) return false;
    bool is_creep_toggle = false;
    for (const std::string& beh : cat_->array_links("CAbil", abil_id, "BehaviorArray")) {
        if (beh.empty()) continue;
        if (cat_->scalar("CBehavior", beh, "Grown")) {
            is_creep_toggle = true;
            break;
        }
    }
    if (!is_creep_toggle) return false;

    const bool engage = command != "Off";
    bool any = false;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || !u.has_toggle_creep) continue;
        if (engage == u.creep_gen_on) continue;
        if (world_.set_creep_generation(id, engage)) any = true;
        else if (!engage) any = true;
    }
    return any;
}

bool MeleeGame::human_salvage(const std::string& abil_id) {
    if (!cat_ || cfg_.human_player < 0) return false;
    const data::Catalog::SalvageSpec sp = cat_->salvage_spec(abil_id);
    if (!sp.found || (sp.mineral_frac <= 0 && sp.gas_frac <= 0)) return false;

    bool any = false;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        sim::Unit& u = world_.unit(id);
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.salvage_timer.raw() > 0) continue;
        if (world_.cargo_used(id) > 0) continue;
        const data::UnitCost c = data::unit_cost(*cat_, u.catalog_id);
        const int32_t rm = int32_t(double(c.minerals) * sp.mineral_frac);
        const int32_t rg = int32_t(double(c.gas) * sp.gas_frac);
        if (world_.begin_salvage(id, sim::Fixed::from_double(sp.delay), rm, rg)) any = true;
    }
    return any;
}

bool MeleeGame::human_merge() {
    if (!cat_) return false;
    const std::vector<std::uint32_t> tem = selected_merge_templar();
    if (tem.size() < 2) return false;
    const std::uint32_t hi = tem[tem.size() - 1];
    const std::uint32_t lo = tem[tem.size() - 2];
    const std::string abil = cat_->merge_ability(world_.unit(lo).catalog_id);
    const auto ms = cat_->merge_spec(abil);
    if (!ms || ms->target.empty()) return false;
    const std::int32_t proto = world_.add_spawn_proto(make_unit(*cat_, ms->target, cfg_.human_player));
    world_.merge_units(lo, hi, proto, sim::Fixed::from_double(ms->time));
    return true;
}

bool MeleeGame::human_research(const std::string& abil_id, const std::string& index,
                               std::uint32_t producer) {
    if (!cat_ || !research_ || cfg_.human_player < 0) return false;
    if (producer == sim::kNoTarget || producer >= world_.units().size()) return false;
    const std::string upgrade = cat_->research_slot_upgrade(abil_id, index);
    if (upgrade.empty()) return false;
    if (research_->has(cfg_.human_player, upgrade) || research_->pending(cfg_.human_player, upgrade))
        return false;
    const std::string req = cat_->button_requirement(abil_id, index);
    if (!req.empty() &&
        !data::eval_requirement(*cat_, req, "Use", human_tech_state(producer)))
        return false;
    const data::Catalog::ResearchInfo info = cat_->research_slot_info(abil_id, index);
    sim::Player& hp = world_.player(cfg_.human_player);
    if (!info.found || info.time <= 0 || hp.minerals < info.minerals || hp.gas < info.gas) return false;
    hp.minerals -= info.minerals;
    hp.gas -= info.gas;
    begin_research(cfg_.human_player, upgrade, sim::Fixed::from_double(info.time));
    return true;
}

bool MeleeGame::human_siege_defender(std::uint32_t caster, sim::Fixed cx, sim::Fixed cy) {
    if (!human_morph(caster, "LiberatorMorphtoAG")) return false;
    world_.set_zone(caster, cx, cy, sim::Fixed::from_int(5));
    return true;
}

float MeleeGame::GroundZ::max_z() const {
    float top = 0.0f;
    for (float z : game->tier_heights_) top = z > top ? z : top;
    return top;
}

void MeleeGame::rebuild_air_layer() {
    air_layer_.clear();
    if (ground_heights_.empty() || ground_w_ == 0 || ground_h_ == 0) return;
    const int W = int(ground_w_), Hh = int(ground_h_);
    std::vector<float> a(ground_heights_.size()), b(ground_heights_.size());
    for (std::size_t k = 0; k < a.size(); ++k) a[k] = std::max(ground_heights_[k], air_floor_);
    auto pass = [&](const std::vector<float>& in, std::vector<float>& out, int r, bool horizontal, bool take_max) {
        for (int y = 0; y < Hh; ++y)
            for (int x = 0; x < W; ++x) {
                float acc = take_max ? -1e30f : 0.0f;
                for (int k = -r; k <= r; ++k) {
                    int sx = horizontal ? x + k : x, sy = horizontal ? y : y + k;
                    sx = sx < 0 ? 0 : (sx >= W ? W - 1 : sx);
                    sy = sy < 0 ? 0 : (sy >= Hh ? Hh - 1 : sy);
                    const float v = in[std::size_t(sy) * ground_w_ + std::size_t(sx)];
                    acc = take_max ? std::max(acc, v) : acc + v;
                }
                out[std::size_t(y) * ground_w_ + std::size_t(x)] = take_max ? acc : acc / float(2 * r + 1);
            }
    };
    for (int d = 0; d < kAirDilateCells; ++d) {
        pass(a, b, 1, true, true);
        pass(b, a, 1, false, true);
    }
    for (int p = 0; p < kAirBlurPasses; ++p) {
        pass(a, b, kAirBlurRadius, true, false);
        pass(b, a, kAirBlurRadius, false, false);
    }
    air_layer_ = std::move(a);
}

float MeleeGame::air_z_at(float wx, float wy) const {
    if (!air_layer_.empty() && ground_w_ > 0 && ground_h_ > 0) {
        const float fx = wx - 0.5f, fy = wy - 0.5f;
        const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
        const float tx = fx - float(x0), ty = fy - float(y0);
        auto at = [&](int gx, int gy) -> float {
            gx = gx < 0 ? 0 : (gx >= int(ground_w_) ? int(ground_w_) - 1 : gx);
            gy = gy < 0 ? 0 : (gy >= int(ground_h_) ? int(ground_h_) - 1 : gy);
            return air_layer_[std::size_t(gy) * ground_w_ + std::size_t(gx)];
        };
        const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
        const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
        return a + (b - a) * ty;
    }
    float z = air_floor_;
    for (int dy = -kAirSmoothCells; dy <= kAirSmoothCells; ++dy)
        for (int dx = -kAirSmoothCells; dx <= kAirSmoothCells; ++dx)
            z = std::max(z, ground_z_at(wx + float(dx), wy + float(dy)));
    return z;
}

float MeleeGame::ground_z_at(float wx, float wy) const {
    if (tier_heights_.empty()) return 0.0f;
    const sim::NavGrid& nav = world_.nav();
    if (!nav.configured()) return 0.0f;
    int cx = 0, cy = 0;
    nav.to_cell(sim::Fixed::from_double(double(wx)), sim::Fixed::from_double(double(wy)), cx, cy);
    if (!ground_heights_.empty() && ground_w_ > 0 && ground_h_ > 0) {
        const float fx = wx - 0.5f, fy = wy - 0.5f;
        const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
        const float tx = fx - float(x0), ty = fy - float(y0);
        auto at = [&](int gx, int gy) -> float {
            gx = gx < 0 ? 0 : (gx >= int(ground_w_) ? int(ground_w_) - 1 : gx);
            gy = gy < 0 ? 0 : (gy >= int(ground_h_) ? int(ground_h_) - 1 : gy);
            const std::size_t k = std::size_t(gy) * ground_w_ + std::size_t(gx);
            return k < ground_heights_.size() ? ground_heights_[k] : 0.0f;
        };
        const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
        const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
        return a + (b - a) * ty;
    }
    const std::size_t L = std::size_t(nav.level_cell(cx, cy));
    return tier_heights_[L < tier_heights_.size() ? L : tier_heights_.size() - 1];
}

void MeleeGame::update_camera(const render::InputState& in, float dt_s) {
    if (in.wheel != 0.0f) {
        const int dir = in.wheel > 0.0f ? 1 : -1;
        zoom_level_ = render::apply_melee_zoom(camctl_, zoom_level_ + dir);
    }
    float px = 0, py = 0;
    if (in.key_pan_left) px -= 1;
    if (in.key_pan_right) px += 1;
    if (in.key_pan_down) py -= 1;
    if (in.key_pan_up) py += 1;
    if (in.view_w && in.view_h) {
        refresh_hud_layout(in.view_w, in.view_h);
        const ConsolePanel console = console_view();
        const bool over_bar =
            hud_covers_point(in.view_w, in.view_h, &console, in.mouse_x, in.mouse_y);
        if (in.mouse_x <= cfg_.edge_margin) px -= 1;
        else if (in.mouse_x >= float(in.view_w) - cfg_.edge_margin) px += 1;
        if (in.mouse_y <= cfg_.edge_margin) py += 1;
        else if (in.mouse_y >= float(in.view_h) - cfg_.edge_margin && !over_bar) py -= 1;
    }
    if (px != 0.0f || py != 0.0f) {
        user_scrolled_ = true;
        camctl_.pan(px * cfg_.pan_speed * dt_s, py * cfg_.pan_speed * dt_s);
        if (playable_x1_ > playable_x0_ && playable_y1_ > playable_y0_) {
            camctl_.focus.x = std::clamp(camctl_.focus.x, playable_x0_, playable_x1_);
            camctl_.focus.y = std::clamp(camctl_.focus.y, playable_y0_, playable_y1_);
        }
    }
    if (follow_.active) {
        float cx = 0.0f, cy = 0.0f;
        if (!selection_centre(cx, cy)) {
            follow_.stop();
        } else if (follow_.update(cx, cy, camctl_.focus.x, camctl_.focus.y, dt_s, user_scrolled_)) {
            camctl_.focus.z = ground_z_at(camctl_.focus.x, camctl_.focus.y);
        }
    }
    float o = 0;
    if (in.key_orbit_left) o += 1;
    if (in.key_orbit_right) o -= 1;
    if (o != 0.0f) camctl_.orbit(o * cfg_.orbit_speed * dt_s);
    if (in.middle_down && (in.mouse_dx != 0.0f || in.mouse_dy != 0.0f))
        camctl_.pan(-in.mouse_dx * cfg_.pan_mouse, in.mouse_dy * cfg_.pan_mouse);
}

void MeleeGame::build_actor_catalog(const data::Catalog& cat) {
    actor_cat_.reset(new data::ActorCatalog());
    actor_cat_->build(cat);
    actor_rules_.clear();
}

static formats::Model flat_ground_quad(float extent, std::uint32_t divs, const std::vector<float>& heights) {
    const std::uint32_t n = divs + 1;
    const float half = extent * 0.5f, step = extent / float(divs);
    formats::Model m;
    m.vertex_count = n * n;
    m.stride = 0;
    float zmin = 1e30f, zmax = -1e30f;
    for (std::uint32_t y = 0; y < n; ++y)
        for (std::uint32_t x = 0; x < n; ++x) {
            const float wz = heights.size() == std::size_t(n) * n ? heights[std::size_t(y) * n + x] : 0.0f;
            m.positions.push_back(-half + float(x) * step);
            m.positions.push_back(-half + float(y) * step);
            m.positions.push_back(wz);
            m.uvs.push_back(float(x) / float(divs));
            m.uvs.push_back(float(y) / float(divs));
            zmin = std::min(zmin, wz);
            zmax = std::max(zmax, wz);
        }
    for (std::uint32_t y = 0; y < divs; ++y)
        for (std::uint32_t x = 0; x < divs; ++x) {
            const std::uint16_t i0 = std::uint16_t(y * n + x), i1 = std::uint16_t(y * n + x + 1);
            const std::uint16_t i2 = std::uint16_t((y + 1) * n + x + 1), i3 = std::uint16_t((y + 1) * n + x);
            for (std::uint16_t i : {i0, i1, i2, i0, i2, i3}) m.indices.push_back(i);
        }
    formats::M3Region r;
    r.num_vertices = m.vertex_count;
    r.num_indices = std::uint32_t(m.indices.size());
    m.regions.push_back(r);
    m.bounds_min[0] = -half; m.bounds_min[1] = -half; m.bounds_min[2] = zmin;
    m.bounds_max[0] = half; m.bounds_max[1] = half; m.bounds_max[2] = zmax;
    m.bounds_radius = std::sqrt(half * half * 2.0f + std::max(zmax * zmax, zmin * zmin));
    return m;
}

const formats::Model* MeleeGame::splat_quad_for(const sim::Unit& u) {
    if (auto it = splat_quads_.find(u.id); it != splat_quads_.end()) return &it->second;
    if (splat_none_.count(u.id)) return nullptr;
    const formats::Model* splat = (models_ && ui_store_) ? models_->unit_splat(*ui_store_, u.catalog_id) : nullptr;
    if (!splat || splat->materials.empty()) {
        splat_none_.insert(u.id);
        return nullptr;
    }
    float extent = 1.0f;
    if (cat_) {
        int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        bool any = false;
        for (const auto& [dx, dy] : cat_->unit_pathing_footprint_offsets(u.catalog_id)) {
            if (!any) { x0 = x1 = dx; y0 = y1 = dy; any = true; }
            x0 = std::min(x0, dx); x1 = std::max(x1, dx);
            y0 = std::min(y0, dy); y1 = std::max(y1, dy);
        }
        if (any) extent = float(std::max(x1 - x0, y1 - y0) + 1);
    }
    const std::uint32_t divs = 4;
    const float cx = float(u.x.to_double()), cy = float(u.y.to_double());
    std::vector<float> heights((divs + 1) * (divs + 1), 0.0f);
    for (std::uint32_t r = 0; r <= divs; ++r)
        for (std::uint32_t c = 0; c <= divs; ++c) {
            const float lx = -extent * 0.5f + extent * float(c) / float(divs);
            const float ly = -extent * 0.5f + extent * float(r) / float(divs);
            heights[r * (divs + 1) + c] = ground_z_at(cx + lx, cy + ly);
        }
    formats::Model quad = flat_ground_quad(extent, divs, heights);
    quad.materials = splat->materials;
    for (formats::M3Material& mat : quad.materials) {
        bool has_diffuse = false;
        const formats::M3Layer* mask = nullptr;
        for (const formats::M3Layer& L : mat.layers) {
            if (L.usage == "Diffuse") has_diffuse = true;
            if (L.usage == "AlphaMask") mask = &L;
        }
        if (!has_diffuse) {
            formats::M3Layer d;
            d.usage = "Diffuse";
            d.inline_w = d.inline_h = 1;
            const float* col = mask ? mask->anim.color : nullptr;
            d.inline_rgba = {std::uint8_t((col ? col[0] : 0.0f) * 255.0f), std::uint8_t((col ? col[1] : 0.0f) * 255.0f),
                             std::uint8_t((col ? col[2] : 0.0f) * 255.0f), 255};
            mat.layers.insert(mat.layers.begin(), std::move(d));
        }
    }
    quad.material_refs.assign(1, formats::M3MaterialRef{1, 0});
    quad.region_material.assign(quad.regions.size(), 0);
    return &splat_quads_.emplace(u.id, std::move(quad)).first->second;
}

std::string MeleeGame::race_music_id(int race_index) const {
    if (!cat_) return {};
    const char* race = race_index == 1 ? "Prot" : race_index == 2 ? "Zerg" : "Terr";
    auto gm = cat_->scalar("CRace", race, "GameMusic");
    if (!gm || gm->empty()) gm = cat_->tag_default_scalar("CRace", "GameMusic");
    if (!gm || gm->empty()) return {};
    std::string id = *gm;
    for (std::size_t p; (p = id.find("##id##")) != std::string::npos;) id.replace(p, 6, race);
    return id;
}

void MeleeGame::start_music(const std::string& soundtrack_id) {
    music_ = MusicState{};
    if (!cat_ || soundtrack_id.empty()) return;
    const data::Soundtrack st = data::resolve_soundtrack(*cat_, soundtrack_id);
    if (!st.found || st.master_sound.empty()) return;
    music_.soundtrack_id = soundtrack_id;
    music_.sound_id = st.master_sound;
    music_.next = st.next;
    music_.delay_min = st.delay_min;
    music_.delay_max = st.delay_max;
    music_.continuous = st.continuous;
    music_.wait_s = 0.0f;
}

void MeleeGame::music_tick(float dt_s) {
    if (music_.sound_id.empty()) return;
    if (music_.playing) {
        if (!audio_ || audio_->voice_active(music_.voice)) return;
        music_.playing = false;
        music_.voice = -1;
        if (!music_.next.empty() && !music_.continuous) {
            const std::string nxt = music_.next;
            const int req = music_.requests;
            const std::string last = music_.last_request;
            start_music(nxt);
            music_.requests = req;
            music_.last_request = last;
            if (music_.sound_id.empty()) return;
        } else {
            const std::uint64_t h = MeleeAudio::play_hash(music_.sound_id, std::uint64_t(music_.requests));
            const float u = float(h % 10000ull) / 10000.0f;
            music_.wait_s = music_.delay_min + (music_.delay_max - music_.delay_min) * u;
        }
    }
    if (music_.wait_s > 0.0f) {
        music_.wait_s -= dt_s;
        return;
    }
    ++music_.requests;
    music_.last_request = music_.sound_id;
    music_.playing = true;
    if (audio_) {
        audio_->play_sound(music_.sound_id, 1.0f);
        music_.voice = audio_->last_voice_id();
        if (music_.voice < 0) music_.playing = false;
    }
}

std::string MeleeGame::cursor_id(CursorState s) const {
    if (!cat_) return {};
    const char* base = nullptr;
    switch (s) {
        case CursorState::Normal: base = "Arrow"; break;
        case CursorState::TargetNormal: base = "TargetNormal"; break;
        case CursorState::TargetAllied: base = "TargetAllied"; break;
        case CursorState::TargetEnemy: base = "TargetEnemy"; break;
        case CursorState::TargetInvalid: base = "TargetInvalid"; break;
        case CursorState::SelectNormal: base = "SelectNormal"; break;
        case CursorState::SelectAllied: base = "SelectAllied"; break;
        case CursorState::SelectEnemy: base = "SelectEnemy"; break;
        case CursorState::EdgeScrollTop: base = "EdgeScrollTop"; break;
        case CursorState::EdgeScrollBottom: base = "EdgeScrollBottom"; break;
        case CursorState::EdgeScrollLeft: base = "EdgeScrollLeft"; break;
        case CursorState::EdgeScrollRight: base = "EdgeScrollRight"; break;
        case CursorState::EdgeScrollTopLeft: base = "EdgeScrollTopLeft"; break;
        case CursorState::EdgeScrollTopRight: base = "EdgeScrollTopRight"; break;
        case CursorState::EdgeScrollBottomLeft: base = "EdgeScrollBottomLeft"; break;
        case CursorState::EdgeScrollBottomRight: base = "EdgeScrollBottomRight"; break;
    }
    if (!base) return {};
    const int race = human_race();
    const std::string suffixed = std::string(base) + (race == 1 ? "_Prot" : race == 2 ? "_Zerg" : "_Terr");
    if (cat_->find("CCursor", suffixed)) return suffixed;
    if (cat_->find("CCursor", base)) return base;
    return {};
}

void MeleeGame::update_cursor(const render::InputState& in, Control& ctrl) {
    const bool armed = pending_attack_ || pending_patrol_ || pending_move_ || pending_build_ ||
                       pending_refinery_ || has_pending_ability_cast();
    const std::uint32_t over = ctrl.pick_target(world_, ScreenPoint{in.mouse_x, in.mouse_y});
    const int allegiance = over == sim::kNoTarget || over >= world_.units().size()
                               ? -1
                               : (world_.unit(over).player == cfg_.human_player
                                      ? 0
                                      : (world_.are_allied(world_.unit(over).player, cfg_.human_player) ? 1 : 2));
    if (armed) {
        cursor_state_ = allegiance == 2   ? CursorState::TargetEnemy
                        : allegiance == 1 ? CursorState::TargetAllied
                        : allegiance == 0 ? CursorState::TargetAllied
                                          : CursorState::TargetNormal;
        return;
    }
    if (in.view_w && in.view_h) {
        const bool left = in.mouse_x <= cfg_.edge_margin;
        const bool right = in.mouse_x >= float(in.view_w) - cfg_.edge_margin;
        const bool top = in.mouse_y <= cfg_.edge_margin;
        const bool bottom = in.mouse_y >= float(in.view_h) - cfg_.edge_margin;
        if (top && left) { cursor_state_ = CursorState::EdgeScrollTopLeft; return; }
        if (top && right) { cursor_state_ = CursorState::EdgeScrollTopRight; return; }
        if (bottom && left) { cursor_state_ = CursorState::EdgeScrollBottomLeft; return; }
        if (bottom && right) { cursor_state_ = CursorState::EdgeScrollBottomRight; return; }
        if (top) { cursor_state_ = CursorState::EdgeScrollTop; return; }
        if (bottom) { cursor_state_ = CursorState::EdgeScrollBottom; return; }
        if (left) { cursor_state_ = CursorState::EdgeScrollLeft; return; }
        if (right) { cursor_state_ = CursorState::EdgeScrollRight; return; }
    }
    cursor_state_ = allegiance == 2   ? CursorState::SelectEnemy
                    : allegiance == 1 ? CursorState::SelectAllied
                    : allegiance == 0 ? CursorState::SelectNormal
                                      : CursorState::Normal;
}

std::vector<EquipmentEntry> MeleeGame::equipment_for(int player) const {
    std::vector<EquipmentEntry> out;
    if (player < 0 || !cat_) return out;
    for (const std::string& id : world_.researched(player)) {
        std::size_t end = id.size();
        while (end > 0 && id[end - 1] >= '0' && id[end - 1] <= '9') --end;
        int level = 0;
        for (std::size_t i = end; i < id.size(); ++i) level = level * 10 + (id[i] - '0');
        if (level <= 0) level = 1;
        std::string line = id.substr(0, end);
        if (line.size() > 5 && line.compare(line.size() - 5, 5, "Level") == 0) line.resize(line.size() - 5);
        auto it = out.begin();
        for (; it != out.end(); ++it)
            if (it->line == line) break;
        auto icon_of = [this](const std::string& up) {
            if (auto ic = cat_->scalar("CUpgrade", up, "Icon"); ic && !ic->empty()) return *ic;
            if (auto ic = cat_->button_icon(up)) return *ic;
            return std::string();
        };
        if (it == out.end()) {
            EquipmentEntry e;
            e.line = line;
            e.level = level;
            e.icon = icon_of(id);
            out.push_back(e);
        } else if (level > it->level) {
            it->level = level;
            it->icon = icon_of(id);
        }
    }
    std::sort(out.begin(), out.end(), [](const EquipmentEntry& a, const EquipmentEntry& b) {
        return a.line < b.line;
    });
    return out;
}

bool MeleeGame::jump_to_last_alert() {
    const std::vector<HudMessage>& msgs = messages_.all();
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        if (it->zone != MsgZone::Alert || !it->has_target) continue;
        camctl_.focus = {it->tx, it->ty, ground_z_at(it->tx, it->ty)};
        user_scrolled_ = true;
        return true;
    }
    return false;
}

static bool worker_is_idle(const sim::Unit& u, int human_player) {
    return u.alive && u.player == human_player && u.is_worker && u.orders.empty() &&
           u.construct_target == sim::kNoTarget && u.mine_cd.raw() <= 0;
}

int MeleeGame::idle_worker_count() const {
    if (cfg_.human_player < 0) return 0;
    int n = 0;
    for (const Unit& u : world_.units())
        if (worker_is_idle(u, cfg_.human_player)) ++n;
    return n;
}

std::uint32_t MeleeGame::select_next_idle_worker() {
    const std::size_t n = world_.units().size();
    const std::size_t start = (selection_.size() == 1 && selection_[0] < n) ? selection_[0] + 1 : 0;
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t id = (start + k) % n;
        if (!worker_is_idle(world_.unit(std::uint32_t(id)), cfg_.human_player)) continue;
        set_selection({std::uint32_t(id)});
        return std::uint32_t(id);
    }
    return sim::kNoTarget;
}

std::string MeleeGame::error_sound_id(sim::CmdError e) const {
    if (!cat_) return {};
    const char* base = nullptr;
    switch (e) {
        case sim::CmdError::NotEnoughMinerals: base = "NeedMoreMinerals"; break;
        case sim::CmdError::NotEnoughVespene: base = "NeedMoreGas"; break;
        case sim::CmdError::NotEnoughFood: base = "NeedMoreFood"; break;
        case sim::CmdError::CantBuildOnThat: base = "PlaceOnVespeneGeyser"; break;
        case sim::CmdError::CantBuildLocationBlocked:
        case sim::CmdError::CantBuildTooCloseToResources:
        case sim::CmdError::CantBuildTooFarFromCreepSource:
        case sim::CmdError::CantBuildTooFarFromBuildPowerSource:
        case sim::CmdError::TechRequirementsNotMet: base = "BuildError"; break;
        case sim::CmdError::None: break;
    }
    if (!base) return {};
    const int race = human_race();
    const std::string id = std::string("Alert_") + (race == 1 ? "Protoss" : race == 2 ? "Zerg" : "Terran") + base;
    return cat_->find("CSound", id) ? id : std::string();
}

std::string MeleeGame::alert_sound_id(AlertKind kind) const {
    if (!cat_) return {};
    const char* base = nullptr;
    switch (kind) {
        case AlertKind::AttackUnit: base = "AttackUnit"; break;
        case AlertKind::AttackTown: base = "AttackTown"; break;
        case AlertKind::UpgradeComplete: base = "UpgradeComplete"; break;
        case AlertKind::BuildComplete: base = "BuildComplete"; break;
        case AlertKind::ResourceExhausted: base = "ResourceExhausted_Mineral"; break;
    }
    if (!base) return {};
    const int race = human_race();
    const char* suffix = race == 1 ? "_Prot" : race == 2 ? "_Zerg" : "_Terr";
    if (auto s = cat_->scalar("CAlert", std::string(base) + suffix, "Sound"))
        if (!s->empty()) return *s;
    return {};
}

void MeleeGame::spawn_corpse(const sim::Unit& u) {
    const formats::Model* model = nullptr;
    std::vector<std::string> props = {"Death"};
    if (actor_cat_ && !u.catalog_id.empty()) {
        if (const data::CActorUnit* actor = actor_cat_->actor_for_unit(u.catalog_id)) {
            const data::ActorDeath* row = nullptr;
            for (const data::ActorDeath& d : actor->death_array)
                if (d.index == "Normal" || d.index.empty()) { row = &d; break; }
            if (!row && !actor->death_array.empty()) row = &actor->death_array.front();
            if (row) {
                if (!row->anim_props.empty()) {
                    props.clear();
                    std::string tok;
                    for (char ch : row->anim_props) {
                        if (ch == ',' || ch == ' ') { if (!tok.empty()) props.push_back(tok); tok.clear(); }
                        else tok.push_back(ch);
                    }
                    if (!tok.empty()) props.push_back(tok);
                    if (props.empty()) props.push_back("Death");
                }
                if (models_ && ui_store_ && !row->model_link.empty())
                    model = models_->death_model(*ui_store_, row->model_link);
            }
        }
    }
    if (!model && u.id < driver_.actors.size()) model = driver_.actors[u.id].model;
    if (!model) return;
    int seq = render::select_sequence(*model, props, u.id);
    if (seq < 0 && props != std::vector<std::string>{"Death"}) seq = render::select_sequence(*model, {"Death"}, u.id);
    if (seq < 0 || std::size_t(seq) >= model->sequences.size()) return;
    Corpse c;
    c.model = model;
    c.seq = seq;
    c.length_ms = float(model->sequences[std::size_t(seq)].length_ms());
    if (c.length_ms <= 0.0f) return;
    c.x = float(u.x.to_double());
    c.y = float(u.y.to_double());
    c.z = cat_ ? cat_->unit_display_height(u.catalog_id) : 0.0f;
    c.scale = cat_ ? cat_->unit_render_scale(u.catalog_id) : std::array<float, 3>{1.0f, 1.0f, 1.0f};
    c.player = u.player;
    c.fog_visibility = u.fog_visibility;
    corpses_.push_back(c);
}

const render::ActorRules* MeleeGame::rules_for(const std::string& catalog_id) {
    if (!actor_cat_ || catalog_id.empty()) return nullptr;
    auto it = actor_rules_.find(catalog_id);
    if (it != actor_rules_.end()) return it->second.valid ? &it->second : nullptr;
    const data::CActorUnit* actor = actor_cat_->actor_for_unit(catalog_id);
    if (!actor) {
        actor_rules_.emplace(catalog_id, render::ActorRules{});
        return nullptr;
    }
    it = actor_rules_.emplace(catalog_id, render::ActorRules::from_actor(*actor)).first;
    return it->second.valid ? &it->second : nullptr;
}

bool MeleeGame::consume_modal_target(sim::Fixed wx, sim::Fixed wy) {
    if (!(pending_attack_ || pending_patrol_ || pending_move_) || selection_.empty()) return false;
    std::vector<app::FormationTarget> slots;
    if (pending_move_ || pending_attack_)
        slots = app::formation_targets(world_, selection_, wx, wy, formation_diameter_[0], formation_diameter_[1]);
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        sim::Order o;
        sim::Fixed ux = wx, uy = wy;
        for (const app::FormationTarget& t : slots)
            if (t.id == id) { ux = t.x; uy = t.y; break; }
        if (pending_move_) {
            o.type = sim::OrderType::Move;
            o.tx = ux;
            o.ty = uy;
        } else if (pending_attack_) {
            o.type = sim::OrderType::AttackMove;
            o.tx = ux;
            o.ty = uy;
        } else {
            const Unit& u = world_.unit(id);
            o.type = sim::OrderType::Patrol;
            o.tx = wx;
            o.ty = wy;
            o.px = u.x;
            o.py = u.y;
        }
        world_.issue(id, o);
    }
    audio_ui_event(human_lead_caster(), data::UnitEvent::Confirm, kUiCueTrim);
    pending_attack_ = pending_patrol_ = pending_move_ = false;
    return true;
}

void MeleeGame::handle_commands(const render::InputState& in, float aspect) {
    if (cfg_.human_player < 0 || !in.view_w || !in.view_h) return;
    render::Mat4 mvp = camctl_.view_proj(aspect);
    Control ctrl = make_control(mvp, in.view_w, in.view_h, cfg_.human_player);
    update_cursor(in, ctrl);

    ensure_console_texture();
    refresh_hud_layout(in.view_w, in.view_h);
    const ConsolePanel console = console_view();

    const HudRect mm = hud_minimap_rect(in.view_w, in.view_h, console_tex_.ok() ? &console : nullptr);
    const bool over_minimap = in.mouse_x >= float(mm.x0) && in.mouse_x < float(mm.x1) &&
                              in.mouse_y >= float(mm.y0) && in.mouse_y < float(mm.y1);
    const bool over_bar =
        hud_covers_point(in.view_w, in.view_h, &console, in.mouse_x, in.mouse_y);
    auto minimap_world = [&](float& wx, float& wy) {
        hud_minimap_pixel_to_world(mm, minimap_extent(), in.mouse_x, in.mouse_y, wx, wy);
    };

    const int hot_slot =
        (in.key_char && !in.key_ctrl && !in.key_alt && !selection_.empty()) ? card_slot_for_key(command_card_layout().card,
                                                                 in.key_char)
                                             : -1;

    if (!selection_.empty() && hot_slot < 0) {
        bool ordered = false;
        if (in.key_hold && !prev_keys_.hold) {
            for (std::uint32_t id : selection_)
                world_.issue(id, sim::Order{sim::OrderType::HoldPosition});
            ordered = true;
        }
        if (in.key_stop && !prev_keys_.stop) {
            for (std::uint32_t id : selection_) world_.stop(id);
            ordered = true;
        }
        if (in.key_patrol && !prev_keys_.patrol && !over_bar) {
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                for (std::uint32_t id : selection_) {
                    if (id >= world_.units().size()) continue;
                    const Unit& u = world_.unit(id);
                    sim::Order o;
                    o.type = sim::OrderType::Patrol;
                    o.tx = wx; o.ty = wy; o.px = u.x; o.py = u.y;
                    world_.issue(id, o);
                }
                ordered = true;
            }
        }
        if (ordered) audio_ui_event(human_lead_caster(), data::UnitEvent::Confirm, kUiCueTrim);
    }
    prev_keys_.hold = in.key_hold;
    prev_keys_.stop = in.key_stop;
    prev_keys_.patrol = in.key_patrol;

    if (pending_build_ || pending_refinery_) {
        sim::Fixed wx, wy;
        if (!over_bar && ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
            build_px_ = wx;
            build_py_ = wy;
            bool clear = wx.to_double() >= 0.0 && wy.to_double() >= 0.0 &&
                         wx.to_double() <= double(map_w_) && wy.to_double() <= double(map_h_);
            if (clear)
                for (const Unit& u : world_.units()) {
                    if (!u.alive) continue;
                    if (build_proto_.requires_vespene && u.is_gas && u.resource_left > 0) continue;
                    const sim::Fixed dx = u.x - wx, dy = u.y - wy;
                    const sim::Fixed rr = build_proto_.radius + u.radius;
                    if (dx * dx + dy * dy < rr * rr) { clear = false; break; }
                }
            if (clear && !world_.creep_placement_ok(build_proto_, wx, wy)) clear = false;
            if (clear && !world_.resource_halo_ok(build_proto_, wx, wy)) clear = false;
            if (clear && build_proto_.requires_vespene && !on_live_gas_node(wx, wy)) clear = false;
            if (clear && build_proto_.needs_power &&
                !world_.powered_at(wx, wy, cfg_.human_player))
                clear = false;
            if (clear && world_.placement().configured()) {
                int gcx = 0, gcy = 0;
                world_.placement().to_cell(wx, wy, gcx, gcy);
                int gminx = 0, gmaxx = 0, gminy = 0, gmaxy = 0;
                for (const std::pair<std::int16_t, std::int16_t>& o : build_proto_.footprint_offsets) {
                    if (o.first < gminx) gminx = o.first;
                    if (o.first > gmaxx) gmaxx = o.first;
                    if (o.second < gminy) gminy = o.second;
                    if (o.second > gmaxy) gmaxy = o.second;
                }
                if (!world_.placement().can_place(gcx + gminx, gcy + gminy, gmaxx - gminx + 1,
                                                  gmaxy - gminy + 1,
                                                   build_proto_.requires_vespene))
                    clear = false;
            }
            build_valid_ = clear &&
                           world_.player(cfg_.human_player).minerals >= build_cost_ &&
                           world_.player(cfg_.human_player).gas >= build_gas_cost_;
        } else {
            build_valid_ = false;
        }
    }

    if (minimap_drag_ && (!in.left_down || map_w_ <= 0.0f)) minimap_drag_ = false;
    if (minimap_drag_ && !in.left_pressed) {
        float wx, wy; minimap_world(wx, wy);
        camctl_.focus = {wx, wy, ground_z_at(wx, wy)};
        user_scrolled_ = true;
        dragging_ = false;
        return;
    }
    if (in.left_pressed && idle_worker_count() > 0) {
        const HudRect ib = hud_idle_button_rect(in.view_w, in.view_h, console_tex_.ok() ? &console : nullptr);
        if (ib.w() > 2 && ib.h() > 2 && in.mouse_x >= float(ib.x0) && in.mouse_x < float(ib.x1) &&
            in.mouse_y >= float(ib.y0) && in.mouse_y < float(ib.y1)) {
            select_next_idle_worker();
            return;
        }
    }
    if (over_minimap && map_w_ > 0.0f && map_h_ > 0.0f) {
        if (in.left_pressed) {
            float wx, wy; minimap_world(wx, wy);
            const bool spent = consume_modal_target(
                sim::Fixed::from_ratio(int(std::lround(double(wx) * 4096.0)), 4096),
                sim::Fixed::from_ratio(int(std::lround(double(wy) * 4096.0)), 4096));
            if (!spent) {
                camctl_.focus = {wx, wy, ground_z_at(wx, wy)};
                user_scrolled_ = true;
                minimap_drag_ = in.left_down;
            }
            dragging_ = false;
        }
        if (in.right_pressed && !selection_.empty()) {
            float wx, wy; minimap_world(wx, wy);
            sim::Order o;
            o.type = sim::OrderType::Move;
            o.tx = sim::Fixed::from_ratio(int(std::lround(double(wx) * 64.0)), 64);
            o.ty = sim::Fixed::from_ratio(int(std::lround(double(wy) * 64.0)), 64);
            for (std::uint32_t id : selection_) {
                if (id >= world_.units().size()) continue;
                const Unit& u = world_.unit(id);
                if (cat_ && u.alive && u.player == cfg_.human_player && !u.constructing && !u.catalog_id.empty()) {
                    const std::string props = u.flying ? cat_->liftoff_target(u.catalog_id) : u.catalog_id;
                    if (!props.empty() && !cat_->unit_rally_ability(props).empty()) {
                        world_.set_rally(id, o.tx, o.ty);
                        continue;
                    }
                }
                world_.issue(id, o, in.shift);
            }
            audio_ui_event(human_lead_caster(), data::UnitEvent::Confirm, kUiCueTrim);
        }
        return;
    }

    if (in.right_pressed && over_bar && cast_policy_ && cat_ && !selection_.empty()) {
        const CardLayout rc_layout = command_card_layout();
        const std::vector<AbilityCardEntry>& rc_abils = rc_layout.abilities;
        for (int i = 0; i < kCommandCols * kCommandRows && i < CommandCard::kSlots; ++i) {
            const HudRect c = hud_command_cell(in.view_w, in.view_h, i, &console);
            if (in.mouse_x < float(c.x0) || in.mouse_x >= float(c.x1) ||
                in.mouse_y < float(c.y0) || in.mouse_y >= float(c.y1))
                continue;
            for (const AbilityCardEntry& ab : rc_abils) {
                if (ab.slot != i) continue;
                if (!cat_->ability_autocast(ab.ability).offered) break;
                for (std::uint32_t id : selection_) {
                    if (id >= world_.units().size()) continue;
                    const sim::Unit& su = world_.units()[id];
                    if (!su.alive || su.player != cfg_.human_player) continue;
                    cast_policy_->toggle_autocast(world_, id, ab.ability);
                }
                audio_ui_event(human_lead_caster(), data::UnitEvent::Confirm, kUiCueTrim);
                break;
            }
            break;
        }
        return;
    }

    if (in.left_pressed || hot_slot >= 0) {
        if (over_bar || hot_slot >= 0) {
            dragging_ = false;
            if (hot_slot < 0 && selection_.size() == 1 && selection_[0] < world_.units().size() &&
                world_.unit(selection_[0]).player == cfg_.human_player) {
                const std::uint32_t carrier = selection_[0];
                const std::vector<std::uint32_t> hold = world_.passengers(carrier);
                for (std::size_t c = 0; c < hold.size(); ++c) {
                    const HudRect s = hud_cargo_slot(in.view_w, in.view_h, int(c), &console);
                    if (s.w() <= 0 || in.mouse_x < float(s.x0) || in.mouse_x >= float(s.x1) ||
                        in.mouse_y < float(s.y0) || in.mouse_y >= float(s.y1))
                        continue;
                    world_.unload_one(carrier, int(c));
                    return;
                }
            }
            if (hot_slot < 0 && selection_.size() == 1 && selection_[0] < world_.units().size() &&
                world_.unit(selection_[0]).player == cfg_.human_player) {
                const std::uint32_t producer = selection_[0];
                const int queued = world_.queued_count(producer);
                for (int q = 0; q < queued; ++q) {
                    const HudRect s = hud_queue_slot(in.view_w, in.view_h, q, &console);
                    if (s.w() <= 0 || in.mouse_x < float(s.x0) || in.mouse_x >= float(s.x1) ||
                        in.mouse_y < float(s.y0) || in.mouse_y >= float(s.y1))
                        continue;
                    world_.cancel_train_slot(producer, q);
                    return;
                }
            }
            if (hot_slot < 0 && !hud_group_pane_ids_.empty()) {
                for (const HudPageButton& pb : hud_page_buttons_)
                    if (in.mouse_x >= float(pb.rect.x0) && in.mouse_x < float(pb.rect.x1) &&
                        in.mouse_y >= float(pb.rect.y0) && in.mouse_y < float(pb.rect.y1)) {
                        group_page_ = pb.page;
                        return;
                    }
                for (std::size_t i = 0; i < hud_group_pane_ids_.size(); ++i) {
                    const HudRect s = hud_group_slot(in.view_w, in.view_h, int(i), &console);
                    if (s.w() <= 0 || in.mouse_x < float(s.x0) || in.mouse_x >= float(s.x1) ||
                        in.mouse_y < float(s.y0) || in.mouse_y >= float(s.y1))
                        continue;
                    const std::uint32_t id = hud_group_pane_ids_[i];
                    if (in.shift) {
                        std::vector<std::uint32_t> rest;
                        for (std::uint32_t k : selection_)
                            if (k != id) rest.push_back(k);
                        set_selection(rest);
                    } else {
                        set_selection({id});
                    }
                    return;
                }
            }
            CardLayout layout = command_card_layout();
            CommandCard& card = layout.card;
            const std::vector<BuildCardEntry>& build_abils = layout.builds;
            const std::vector<SubmenuCardEntry>& card_submenus = layout.submenus;
            const std::vector<AbilityCardEntry>& card_abils = layout.abilities;
            const std::vector<TrainCardEntry>& train_abils = layout.trains;
            const std::uint32_t cancel_building = selected_constructing_building();
            const std::uint32_t cancel_producer = selected_training_producer();
            for (int i = 0; i < kCommandCols * kCommandRows && i < CommandCard::kSlots; ++i) {
                const HudRect c = hud_command_cell(in.view_w, in.view_h, i, &console);
                const bool cell_hit =
                    hot_slot >= 0
                        ? (i == hot_slot)
                        : (in.mouse_x >= float(c.x0) && in.mouse_x < float(c.x1) &&
                           in.mouse_y >= float(c.y0) && in.mouse_y < float(c.y1));
                if (cell_hit) {
                    if (card.slots[i].is_passive) break;
                    if (card.slots[i].select_larva != sim::kNoTarget) {
                        const std::uint32_t hall = card.slots[i].select_larva;
                        std::vector<std::uint32_t> brood;
                        for (const sim::Unit& l : world_.units())
                            if (l.alive && l.spawn_parent == hall) brood.push_back(l.id);
                        if (!brood.empty()) {
                            cancel_ability_cast();
                            pending_attack_ = pending_patrol_ = pending_move_ = false;
                            set_selection(brood);
                        }
                        break;
                    }
                    if (card.slots[i].select_builder != sim::kNoTarget) {
                        const std::uint32_t b = card.slots[i].select_builder;
                        if (b < world_.units().size() && world_.units()[b].alive) {
                            cancel_ability_cast();
                            pending_attack_ = pending_patrol_ = pending_move_ = false;
                            set_selection({b});
                        }
                        break;
                    }
                    if (card.slots[i].is_cancel && cancel_building == sim::kNoTarget &&
                        cancel_producer != sim::kNoTarget) {
                        cancel_ability_cast();
                        pending_attack_ = pending_patrol_ = pending_move_ = false;
                        world_.cancel_train(cancel_producer);
                        break;
                    }
                    if (card.slots[i].is_cancel && cancel_building != sim::kNoTarget && cat_) {
                        const sim::Unit& b = world_.units()[cancel_building];
                        const data::UnitCost bc = data::unit_cost(*cat_, b.catalog_id);
                        cancel_ability_cast();
                        pending_attack_ = pending_patrol_ = pending_move_ = false;
                        const auto rf = cat_->refund_fraction(cat_->build_ability_of(b.catalog_id), "Minerals");
                        const sim::Fixed refund = rf ? sim::Fixed::from_double(*rf) : sim::Fixed::from_ratio(3, 4);
                        if (world_.cancel_construction(cancel_building, bc.minerals, bc.gas, refund)) {
                            selection_.erase(std::remove(selection_.begin(), selection_.end(), cancel_building),
                                             selection_.end());
                        }
                        break;
                    }
                    bool routed = false;
                    for (const AbilityCardEntry& ab : card_abils)
                        if (ab.slot == i) {
                            if (ab.requirement_met && cat_) {
                                pending_attack_ = pending_patrol_ = pending_move_ = false;
                                cancel_ability_cast();
                                if (human_toggle_cloak(ab.ability, ab.command)) {
                                } else if (human_toggle_weapon(ab.ability, ab.command)) {
                                } else if (human_toggle_creep(ab.ability, ab.command)) {
                                } else if (human_salvage(ab.ability)) {
                                } else if (const data::Catalog::Entry* re =
                                               cat_->find("CAbil", ab.ability);
                                           re && re->tag == "CAbilResearch") {
                                    human_research(ab.ability, ab.command, human_lead_caster());
                                } else if (ab.command == "Return" &&
                                           cat_->is_harvest_ability(ab.ability)) {
                                    order_return_cargo(*cat_, world_, human_lead_caster());
                                } else if (const auto ms = cat_->morph_spec(ab.ability);
                                           ms && !ms->target.empty()) {
                                    human_morph_selection(ab.ability);
                                } else {
                                    const AbilTargetMode m = ability_target_mode(*cat_, ab.ability);
                                    if (m == AbilTargetMode::Instant)
                                        human_cast(ab.ability, human_lead_caster());
                                    else {
                                        begin_ability_cast(ab.ability, ab.command);
                                        pending_cast_mode_ = m;
                                    }
                                }
                            }
                            routed = true;
                            break;
                        }
                    if (routed) break;
                    for (const TrainCardEntry& tr : train_abils)
                        if (tr.slot == i) {
                            if (tr.requirement_met) {
                                cancel_ability_cast();
                                pending_attack_ = pending_patrol_ = pending_move_ = false;
                                if (tr.warp) {
                                    pending_build_ = pending_refinery_ = false;
                                    pending_warp_ = true;
                                    pending_warp_gate_ = tr.producer;
                                    pending_warp_unit_ = tr.unit;
                                } else {
                                    human_train(tr.producer, tr.unit);
                                }
                            }
                            routed = true;
                            break;
                        }
                    if (routed) break;
                    for (const SubmenuCardEntry& sm : card_submenus)
                        if (sm.slot == i) {
                            cancel_ability_cast();
                            pending_attack_ = pending_patrol_ = pending_move_ = false;
                            set_card_page(sm.target_page);
                            routed = true;
                            break;
                        }
                    if (routed) break;
                    for (const BuildCardEntry& bd : build_abils)
                        if (bd.slot == i) {
                            if (bd.requirement_met) human_build(bd.worker, bd.unit);
                            routed = true;
                            break;
                        }
                    if (routed) break;
                    cancel_ability_cast();
                    switch (card.slots[i].ability) {
                        case kMeleeAbilLiberatorDefender:
                            pending_liberator_ = selected_liberator();
                            if (pending_liberator_ != sim::kNoTarget) pending_defender_ = true;
                            pending_attack_ = pending_patrol_ = pending_move_ = false;
                            break;
                        case kMeleeAbilLiberatorFighter:
                            human_morph(selected_liberator_ag(), "LiberatorMorphtoAA");
                            break;
                        case kMeleeAbilMergeArchon: human_merge(); break;
                        case kMeleeAbilBurrow:
                            if (cat_)
                                for (std::uint32_t id : selected_burrowers(false))
                                    if (burrow_command_allowed(id)) sim::set_burrowed(world_, id, true);
                            break;
                        case kMeleeAbilUnburrow:
                            if (cat_)
                                for (std::uint32_t id : selected_burrowers(true))
                                    sim::set_burrowed(world_, id, false);
                            break;
                        case kMeleeAbilLiftOff:
                            if (cat_)
                                if (const std::uint32_t id = selected_liftoff_structure(false);
                                    id != sim::kNoTarget) {
                                    const auto ls = cat_->liftoff_spec(cat_->liftoff_ability(world_.units()[id].catalog_id));
                                    world_.lift_off(id, ls ? sim::Fixed::from_double(ls->lock) : sim::Fixed(),
                                                    ls ? sim::Fixed::from_double(ls->rise_delay) : sim::Fixed(),
                                                    ls ? sim::Fixed::from_double(ls->rise) : sim::Fixed());
                                }
                            break;
                        case kMeleeAbilLowerDepot:
                            if (cat_)
                                if (const std::uint32_t id = selected_depot(false); id != sim::kNoTarget)
                                    world_.lower(id);
                            break;
                        case kMeleeAbilRaiseDepot:
                            if (cat_)
                                if (const std::uint32_t id = selected_depot(true); id != sim::kNoTarget)
                                    world_.raise(id);
                            break;
                        case kMeleeAbilLand:
                            pending_land_unit_ = selected_liftoff_structure(true);
                            if (pending_land_unit_ != sim::kNoTarget) pending_land_ = true;
                            pending_defender_ = false;
                            pending_attack_ = pending_patrol_ = pending_move_ = false;
                            break;
                        case kMeleeAbilUnloadAll:
                            pending_unload_unit_ = selected_loaded_transport();
                            if (pending_unload_unit_ != sim::kNoTarget) pending_unload_ = true;
                            pending_defender_ = false;
                            pending_land_ = false;
                            pending_attack_ = pending_patrol_ = pending_move_ = false;
                            break;
                        case kMeleeAbilStop:
                            for (std::uint32_t id : selection_) world_.stop(id);
                            pending_attack_ = pending_patrol_ = pending_unload_ = false;
                            break;
                        case kMeleeAbilHold:
                            for (std::uint32_t id : selection_)
                                world_.issue(id, sim::Order{sim::OrderType::HoldPosition});
                            pending_attack_ = pending_patrol_ = pending_unload_ = false;
                            break;
                        case kMeleeAbilAttack:
                            pending_attack_ = true;
                            pending_patrol_ = pending_unload_ = false;
                            break;
                        case kMeleeAbilMove:
                            pending_move_ = true;
                            pending_attack_ = pending_patrol_ = false;
                            break;

                        case kMeleeAbilPatrol:
                            pending_patrol_ = true;
                            pending_attack_ = pending_unload_ = false;
                            break;
                        default: break;
                    }
                    break;
                }
            }
        } else if (pending_warp_) {
            dragging_ = false;
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy))
                human_warp_train(pending_warp_gate_, pending_warp_unit_, wx, wy);
            if (!in.shift) {
                pending_warp_ = false;
                pending_warp_gate_ = sim::kNoTarget;
                pending_warp_unit_.clear();
            }
        } else if (pending_build_) {
            dragging_ = false;
            bool placed = false;
            std::uint32_t bw = sim::kNoTarget;
            if (build_caster_ != sim::kNoTarget) {
                if (build_caster_ < world_.units().size() && world_.unit(build_caster_).alive) bw = build_caster_;
            } else {
                for (std::uint32_t id : selection_)
                    if (id < world_.units().size() && world_.unit(id).alive &&
                        world_.unit(id).is_worker && world_.unit(id).player == cfg_.human_player) {
                        bw = id;
                        break;
                    }
            }
            sim::Fixed wx, wy;
            if (bw != sim::kNoTarget &&
                ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                bool clear = wx.to_double() >= 0.0 && wy.to_double() >= 0.0 &&
                             wx.to_double() <= double(map_w_) && wy.to_double() <= double(map_h_);
                if (clear)
                    for (const Unit& u : world_.units()) {
                        if (!u.alive) continue;
                        if (build_proto_.requires_vespene && u.is_gas && u.resource_left > 0)
                            continue;
                        const sim::Fixed dx = u.x - wx, dy = u.y - wy;
                        const sim::Fixed rr = build_proto_.radius + u.radius;
                        if (dx * dx + dy * dy < rr * rr) { clear = false; break; }
                    }
                if (clear && !world_.creep_placement_ok(build_proto_, wx, wy)) clear = false;
                if (clear && !world_.resource_halo_ok(build_proto_, wx, wy)) clear = false;
                if (clear && build_proto_.requires_vespene && !on_live_gas_node(wx, wy))
                    clear = false;
                if (clear && build_proto_.needs_power && !world_.powered_at(wx, wy, cfg_.human_player))
                    clear = false;
                if (clear && build_caster_ != sim::kNoTarget) {
                    placed = world_.found_charged_build(bw, build_proto_, wx, wy, build_time_) != sim::kNoTarget;
                } else if (clear) {
                    build_queue_.push_back({bw, wx, wy, build_proto_, build_cost_, build_gas_cost_,
                                            build_time_,
                                            build_proto_.supply_provides.raw() == 0});
                    placed = true;
                }
            }
            if ((placed && (!in.shift || build_caster_ != sim::kNoTarget)) || bw == sim::kNoTarget) {
                pending_build_ = false;
                build_caster_ = sim::kNoTarget;
            }
        } else if (pending_refinery_) {
            dragging_ = false;
            std::uint32_t bw = sim::kNoTarget;
            for (std::uint32_t id : selection_)
                if (id < world_.units().size() && world_.unit(id).alive &&
                    world_.unit(id).is_worker && world_.unit(id).player == cfg_.human_player) {
                    bw = id;
                    break;
                }
            sim::Fixed wx, wy;
            if (bw != sim::kNoTarget && cat_ &&
                ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                std::uint32_t geyser = sim::kNoTarget;
                sim::Fixed best_d2;
                for (const Unit& u : world_.units()) {
                    if (!u.alive || u.player >= 0 || !u.is_gas || u.resource_left <= 0) continue;
                    const sim::Fixed r = u.radius + sim::Fixed::from_int(2);
                    const sim::Fixed dx = u.x - wx, dy = u.y - wy, d2 = dx * dx + dy * dy;
                    if (d2 > r * r) continue;
                    if (geyser == sim::kNoTarget || d2 < best_d2) { geyser = u.id; best_d2 = d2; }
                }
                if (geyser != sim::kNoTarget) {
                    const std::string gas = human_gas_structure();
                    const int cost = data::unit_cost(*cat_, gas).minerals;
                    if (world_.player(cfg_.human_player).minerals >= cost) {
                        Unit& gy = world_.unit(geyser);
                        sim::Unit proto = make_unit(*cat_, gas, cfg_.human_player);
                        proto.speed = sim::Fixed();
                        proto.is_gas = true;
                        proto.resource_left = gy.resource_left;
                        const sim::Fixed gpx = gy.x, gpy = gy.y;
                        if (world_.issue_worker_build(bw, gpx, gpy, proto, cost, 0,
                                                      sim::Fixed::from_int(30),
                                                      sim::Fixed::from_int(2), build_verb_for(bw)))
                            world_.unit(geyser).resource_left = 0;
                    }
                }
            }
            pending_refinery_ = false;
        } else if (pending_defender_) {
            dragging_ = false;
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy))
                human_siege_defender(pending_liberator_, wx, wy);
            pending_defender_ = false;
            pending_liberator_ = sim::kNoTarget;
        } else if (pending_land_) {
            dragging_ = false;
            sim::Fixed wx, wy;
            if (pending_land_unit_ < world_.units().size() && world_.unit(pending_land_unit_).alive &&
                world_.unit(pending_land_unit_).flying &&
                ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                const bool in_bounds = wx.to_double() >= 0.0 && wy.to_double() >= 0.0 &&
                                       wx.to_double() <= double(map_w_) && wy.to_double() <= double(map_h_);
                if (in_bounds && world_.order_land(pending_land_unit_, wx, wy)) {
                    pending_land_ = false;
                    pending_land_unit_ = sim::kNoTarget;
                }
            } else {
                pending_land_ = false;
                pending_land_unit_ = sim::kNoTarget;
            }
        } else if (pending_unload_) {
            dragging_ = false;
            sim::Fixed wx, wy;
            if (pending_unload_unit_ < world_.units().size() && world_.unit(pending_unload_unit_).alive &&
                ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy))
                world_.unload_all_at(pending_unload_unit_, wx, wy);
            pending_unload_ = false;
            pending_unload_unit_ = sim::kNoTarget;
        } else if ((pending_attack_ || pending_patrol_ || pending_move_) && !selection_.empty()) {
            dragging_ = false;
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                consume_modal_target(wx, wy);
            } else {
                audio_ui_event(human_lead_caster(), data::UnitEvent::Confirm, kUiCueTrim);
                pending_attack_ = pending_patrol_ = pending_move_ = false;
            }
        } else if (has_pending_ability_cast()) {
            dragging_ = false;
            if (pending_cast_mode_ == AbilTargetMode::Point) {
                sim::Fixed wx, wy;
                if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                    const std::uint32_t caster = human_lead_caster();
                    if (caster != sim::kNoTarget && is_nova_cast(caster, pending_cast_abil_)) {
                        if (world_.launch_nova(caster, wx, wy)) cancel_ability_cast();
                    } else {
                        cast_pending_at_point(wx, wy);
                    }
                }
            } else {
                std::uint32_t tgt = ctrl.pick_target(world_, ScreenPoint{in.mouse_x, in.mouse_y});
                if (tgt == sim::kNoTarget)
                    tgt = ctrl.pick_own(world_, ScreenPoint{in.mouse_x, in.mouse_y});
                if (tgt != sim::kNoTarget) cast_pending_at_target(tgt);
            }
        } else {
            dragging_ = true;
            drag_x0_ = in.mouse_x;
            drag_y0_ = in.mouse_y;
        }
    }
    if (in.left_released && dragging_) {
        dragging_ = false;
        float x0 = drag_x0_, y0 = drag_y0_, x1 = in.mouse_x, y1 = in.mouse_y;
        const bool marquee = std::fabs(x1 - x0) >= 6.0f || std::fabs(y1 - y0) >= 6.0f;
        if (!marquee) {
            x0 = in.mouse_x - 6.0f; x1 = in.mouse_x + 6.0f;
            y0 = in.mouse_y - 6.0f; y1 = in.mouse_y + 6.0f;
        }
        const std::vector<std::uint32_t> prev_selection = selection_;
        set_selection(ctrl.box_select(world_, ScreenRect{x0, y0, x1, y1}, marquee));
        if (!marquee && selection_.size() > 1) {
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                std::uint32_t best = selection_[0];
                sim::Fixed best_d2;
                bool first = true;
                for (std::uint32_t id : selection_) {
                    if (id >= world_.units().size()) continue;
                    const sim::Fixed dx = world_.unit(id).x - wx, dy = world_.unit(id).y - wy;
                    const sim::Fixed d2 = dx * dx + dy * dy;
                    if (first || d2 < best_d2) { best = id; best_d2 = d2; first = false; }
                }
                set_selection({best});
            } else {
                selection_.resize(1);
            }
        }
        const bool shift_merge = in.shift && !prev_selection.empty() && !selection_.empty() &&
                                 selection_[0] < world_.units().size() && world_.unit(selection_[0]).player == cfg_.human_player;
        if (shift_merge) {
            std::vector<std::uint32_t> merged = prev_selection;
            if (!marquee && selection_.size() == 1) {
                auto it = std::find(merged.begin(), merged.end(), selection_[0]);
                if (it != merged.end()) merged.erase(it);
                else merged.push_back(selection_[0]);
            } else {
                for (std::uint32_t id : selection_)
                    if (std::find(merged.begin(), merged.end(), id) == merged.end()) merged.push_back(id);
            }
            set_selection(std::move(merged));
        }
        if (!shift_merge && !marquee && selection_.size() == 1 && selection_[0] < world_.units().size()) {
            const bool twice = in.key_ctrl || (clock_ms_ - last_click_ms_ <= kDoubleClickMs && std::fabs(in.mouse_x - last_click_x_) <= 6.0f &&
                                               std::fabs(in.mouse_y - last_click_y_) <= 6.0f);
            const Unit& picked = world_.unit(selection_[0]);
            if (twice && picked.player == cfg_.human_player) {
                std::vector<std::uint32_t> same;
                for (const Unit& u : world_.units()) {
                    if (!u.alive || u.player != picked.player || u.loaded) continue;
                    const bool same_type = !picked.catalog_id.empty()
                                               ? (cat_ ? cat_->unit_select_alias(u.catalog_id) == cat_->unit_select_alias(picked.catalog_id)
                                                       : u.catalog_id == picked.catalog_id)
                                                                      : (u.id < layout_.unit_type.size() && picked.id < layout_.unit_type.size() &&
                                                                         layout_.unit_type[u.id] == layout_.unit_type[picked.id]);
                    if (!same_type) continue;
                    float sx = 0.0f, sy = 0.0f;
                    if (!ctrl.project_on_ground(float(u.x.to_double()), float(u.y.to_double()), sx, sy)) continue;
                    if (sx < 0.0f || sy < 0.0f || sx >= float(in.view_w) || sy >= float(in.view_h)) continue;
                    same.push_back(u.id);
                }
                if (!same.empty()) set_selection(same);
                last_click_ms_ = -1e9;
            } else {
                last_click_ms_ = clock_ms_;
                last_click_x_ = in.mouse_x;
                last_click_y_ = in.mouse_y;
            }
        } else {
            last_click_ms_ = -1e9;
        }
        if (selection_.empty() && !marquee) {
            const std::uint32_t enemy = ctrl.pick_target(world_, ScreenPoint{in.mouse_x, in.mouse_y});
            if (enemy != sim::kNoTarget) set_selection({enemy});
        }
        if (selection_.empty()) {
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                for (const Unit& u : world_.units()) {
                    if (!u.alive || u.player >= 0) continue;
                    const sim::Fixed dx = u.x - wx, dy = u.y - wy;
                    const sim::Fixed rr = u.radius + sim::Fixed::from_ratio(1, 2);
                    if (dx * dx + dy * dy <= rr * rr) { set_selection({u.id}); break; }
                }
            }
        }
    }

    if (in.right_pressed && pending_build_) pending_build_ = false;
    if (in.right_pressed && pending_warp_) {
        pending_warp_ = false;
        pending_warp_gate_ = sim::kNoTarget;
        pending_warp_unit_.clear();
    }
    if (in.right_pressed && pending_refinery_) pending_refinery_ = false;

    const bool selection_owned = std::any_of(selection_.begin(), selection_.end(), [&](std::uint32_t id) {
        return id < world_.units().size() && world_.unit(id).player == cfg_.human_player;
    });
    if (in.right_pressed && !pending_build_ && !pending_refinery_ && selection_owned && !over_bar) {
        std::uint32_t node = sim::kNoTarget;
        sim::Fixed gx, gy;
        const bool have_pt = ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, gx, gy);
        if (have_pt) {
            sim::Fixed best_d2;
            for (const Unit& u : world_.units()) {
                const bool neutral_resource = u.alive && u.player < 0;
                const bool own_gas = u.alive && u.is_gas && u.player == cfg_.human_player;
                if (!neutral_resource && !own_gas) continue;
                sim::Fixed r = u.radius + sim::Fixed::from_int(2);
                sim::Fixed dx = u.x - gx, dy = u.y - gy, d2 = dx * dx + dy * dy;
                if (d2 > r * r) continue;
                if (node == sim::kNoTarget || d2 < best_d2) { node = u.id; best_d2 = d2; }
            }
        }
        const bool enemy_under = ctrl.pick_target(world_, ScreenPoint{in.mouse_x, in.mouse_y}) != sim::kNoTarget;
        if (enemy_under) node = sim::kNoTarget;
        std::uint32_t resume = sim::kNoTarget;
        if (node == sim::kNoTarget && !enemy_under) {
            for (const Unit& u : world_.units()) {
                if (!u.alive || u.player != cfg_.human_player || !world_.is_halted(u.id)) continue;
                const sim::Fixed r = u.radius + sim::Fixed::from_int(1);
                const sim::Fixed dx = u.x - gx, dy = u.y - gy;
                if (dx * dx + dy * dy <= r * r) { resume = u.id; break; }
            }
        }

        std::uint32_t transport = sim::kNoTarget;
        if (node == sim::kNoTarget && resume == sim::kNoTarget && !enemy_under && cfg_.human_player >= 0) {
            for (const Unit& u : world_.units()) {
                if (!u.alive || u.cargo_space <= 0) continue;
                if (!world_.are_allied(u.player, cfg_.human_player)) continue;
                const sim::Fixed r = u.radius + sim::Fixed::from_int(1);
                const sim::Fixed dx = u.x - gx, dy = u.y - gy;
                if (dx * dx + dy * dy <= r * r) { transport = u.id; break; }
            }
        }

        std::vector<std::uint32_t> producers;
        if (have_pt && cat_ && cfg_.human_player >= 0) {
            for (std::uint32_t id : selection_) {
                if (id >= world_.units().size()) continue;
                const Unit& u = world_.unit(id);
                if (!u.alive || u.player != cfg_.human_player || u.constructing || u.catalog_id.empty())
                    continue;
                const std::string props =
                    u.flying ? cat_->liftoff_target(u.catalog_id) : u.catalog_id;
                if (props.empty() || cat_->unit_rally_ability(props).empty()) continue;
                world_.set_rally(id, gx, gy);
                producers.push_back(id);
            }
        }

        if (resume != sim::kNoTarget) {
            for (std::uint32_t id : selection_)
                if (id < world_.units().size() && world_.unit(id).alive &&
                    world_.unit(id).is_worker) { world_.assign_builder(id, resume); break; }
        } else if (transport != sim::kNoTarget) {
            for (std::uint32_t id : selection_) {
                if (id >= world_.units().size() || id == transport) continue;
                const Unit& p = world_.unit(id);
                if (!p.alive || p.player != cfg_.human_player || p.cargo_size <= 0 || p.cargo_space > 0)
                    continue;
                world_.issue(id, sim::Order{sim::OrderType::Load, sim::Fixed(), sim::Fixed(), transport},
                             in.shift);
            }
        } else if (node != sim::kNoTarget && cfg_.human_player >= 0 &&
                   size_t(cfg_.human_player) < layout_.players.size()) {
            const std::uint32_t depot = layout_.players[size_t(cfg_.human_player)].base.depot;
            const bool gas_node = node < world_.units().size() && world_.unit(node).is_gas;
            std::vector<std::uint32_t> non_workers;
            for (std::uint32_t id : selection_) {
                if (id >= world_.units().size()) continue;
                const bool is_scv = id < layout_.unit_type.size() && layout_.unit_type[id] == "SCV";
                if (!(is_scv || world_.unit(id).is_worker)) {
                    if (world_.unit(id).player == cfg_.human_player) non_workers.push_back(id);
                    continue;
                }
                {
                    Unit& w = world_.unit(id);
                    w.home_depot = depot;
                    w.gather_target = node;
                    w.gathers_gas = gas_node;
                    world_.issue(id, sim::Order{sim::OrderType::Gather, sim::Fixed(),
                                                sim::Fixed(), sim::kNoTarget}, in.shift);
                }
            }
            if (!non_workers.empty())
                ctrl.issue_command(world_, non_workers, ScreenPoint{in.mouse_x, in.mouse_y},
                                    false,  in.shift);
        } else if (producers.empty()) {
            ctrl.issue_command(world_, selection_, ScreenPoint{in.mouse_x, in.mouse_y},
                                false,  in.shift);
        } else {
            std::vector<std::uint32_t> movers;
            movers.reserve(selection_.size());
            for (std::uint32_t id : selection_)
                if (std::find(producers.begin(), producers.end(), id) == producers.end())
                    movers.push_back(id);
            if (!movers.empty())
                ctrl.issue_command(world_, movers, ScreenPoint{in.mouse_x, in.mouse_y},
                                    false,  in.shift);
        }
        audio_ui_event(human_lead_caster(), data::UnitEvent::Confirm, kUiCueTrim);
    }

    const std::uint32_t lead = human_lead_caster();
    if (lead != audio_sel_lead_) {
        if (lead != sim::kNoTarget) audio_ui_event(lead, data::UnitEvent::Select, 1.0f);
        audio_sel_lead_ = lead;
    }
}

std::uint32_t MeleeGame::spawn(sim::Unit proto, const std::string& token,
                               const formats::Model* model, sim::Fixed x, sim::Fixed y) {
    proto.x = x;
    proto.y = y;
    std::uint32_t id = world_.add_unit(proto);
    if (layout_.unit_type.size() <= id) layout_.unit_type.resize(id + 1);
    layout_.unit_type[id] = token;
    driver_.set_model(id, model);
    if (bound_count_ <= id) bound_count_ = id + 1;
    return id;
}

void MeleeGame::snapshot_positions(std::vector<render::Vec3>& out) const {
    const std::vector<Unit>& units = world_.units();
    if (out.size() < units.size()) out.resize(units.size());
    for (const Unit& u : units)
        out[u.id] = {float(u.x.to_double()), float(u.y.to_double()), 0.0f};
}

void MeleeGame::emit_scene(std::vector<render::SceneInstance>& scene) {
    scene.clear();
    if (ground_) {
        render::SceneInstance g;
        g.model = ground_;
        g.world = render::Mat4::identity();
        g.team = {0, 0, 0, -1.0f};
        g.seq = -1;
        scene.push_back(g);
    }
    if (water_ && !water_->regions.empty() && water_->vertex_count) {
        render::SceneInstance w;
        w.model = water_;
        w.world = render::Mat4::identity();
        w.team = {water_rgba_[0], water_rgba_[1], water_rgba_[2], 2.0f};
        w.seq = -1;
        scene.push_back(w);
    }
    const bool splat_fog_ready = cfg_.human_player >= 0 && vision_.width() > 0;
    if (models_ && ui_store_)
        for (const Unit& u : world_.units()) {
            if (!u.alive || u.player < 0 || u.speed.raw() != 0 || u.flying) continue;
            if (splat_fog_ready && u.player != cfg_.human_player &&
                !hud_fog_body_drawn(int(vision_.state_at(u.x, u.y)), FogVisibility(u.fog_visibility)))
                continue;
            const formats::Model* q = splat_quad_for(u);
            if (!q) continue;
            render::SceneInstance inst;
            inst.model = q;
            inst.world = render::translation(float(u.x.to_double()), float(u.y.to_double()), 0.03f);
            inst.team = {0.0f, 0.0f, 0.0f, -1.0f};
            inst.seq = -1;
            scene.push_back(inst);
        }

    {
        constexpr float kCreepDrawLift = 0.05f;
        constexpr float kCreepUvTilesPerCell = 1.0f;
        const sim::CreepGrid& cg = world_.creep().grid();
        const std::uint64_t sum = cg.checksum();
        if (!creep_model_built_ || sum != creep_drawn_checksum_) {
            creep_model_ = render::build_creep_model(
                cg, [this](float wx, float wy) { return ground_z_at(wx, wy); }, kCreepDrawLift,
                kCreepUvTilesPerCell, creep_diffuse_);
            creep_drawn_checksum_ = sum;
            creep_model_built_ = true;
            ++creep_model_gen_;
        }
        if (creep_model_.vertex_count > 0) {
            render::SceneInstance c;
            c.model = &creep_model_;
            c.mesh_version = creep_model_gen_;
            c.world = render::Mat4::identity();
            c.team = {0, 0, 0, -1.0f};
            c.seq = -1;
            scene.push_back(c);
        }
    }
    const float tick_ms =
        (1000.0f / float(sim::World::kTicksPerSecond)) / float(clock_.multiplier(clock_.speed()));
    const float alpha = tick_ms > 0.0f ? std::min(1.0f, accum_ms_ / tick_ms) : 0.0f;
    const bool fog_ready = cfg_.human_player >= 0 && vision_.width() > 0;
    for (const Unit& u : world_.units()) {
        if (!u.alive) continue;
        if (fog_ready && u.player != cfg_.human_player &&
            !hud_fog_body_drawn(int(vision_.state_at(u.x, u.y)),
                                FogVisibility(u.fog_visibility)))
            continue;
        if (u.gathers_gas && u.mine_cd.raw() > 0 && u.gather_target != sim::kNoTarget &&
            u.gather_target < world_.units().size() && world_.units()[u.gather_target].is_gas)
            continue;
        render::SceneInstance inst;
        const formats::Model* m =
            (u.id < driver_.actors.size()) ? driver_.actors[u.id].model : nullptr;
        inst.model = m;
        const std::array<float, 3> ms =
            cat_ ? cat_->unit_render_scale(u.catalog_id) : std::array<float, 3>{1.0f, 1.0f, 1.0f};
        const float footprint_scale = std::max(ms[0], ms[1]);
        const float native_span = cached_diameter(m);
        const float visual_r =
            (native_span > 1e-3f ? native_span * 0.5f : float(u.radius.to_double())) *
            footprint_scale;
        float wx = float(u.x.to_double()), wy = float(u.y.to_double());
        if (u.id < prev_pos_.size() && u.id < curr_pos_.size()) {
            const render::Vec3& p = prev_pos_[u.id];
            const render::Vec3& c = curr_pos_[u.id];
            wx = p.x + (c.x - p.x) * alpha;
            wy = p.y + (c.y - p.y) * alpha;
        }
        float wz = 0.0f;
        if (cat_) {
            wz = cat_->unit_display_height(u.catalog_id);
            if (u.flying && wz <= 0.0f) {
                const float fh = cat_->unit_display_height(u.catalog_id + "Flying");
                wz = fh > 0.0f ? fh : 3.25f;
                const float rise = float(u.lift_rise.to_double());
                const float total = float(u.lift_rise_delay.to_double()) + rise;
                if (rise > 0.0f && float(u.lift_t.to_double()) < total) {
                    const float t = float(u.lift_t.to_double()) - float(u.lift_rise_delay.to_double());
                    wz *= t <= 0.0f ? 0.0f : t / rise;
                }
                if (u.land_pending && u.land_timer.raw() >= 0 && u.land_desc.raw() > 0) {
                    const float elapsed = float((u.land_time - u.land_timer).to_double());
                    const float t = elapsed - float(u.land_desc_delay.to_double());
                    if (t > 0.0f) wz *= std::max(0.0f, 1.0f - t / float(u.land_desc.to_double()));
                }
            }
        }
        wz += (u.flying || wz > 0.0f) ? air_z_at(wx, wy) : ground_z_at(wx, wy);
        inst.world = render::translation(wx, wy, wz) * render::scaling(ms[0], ms[1], ms[2]);
        if (u.id < driver_.actors.size() && m) {
            render::ActorInstance& a = driver_.actors[u.id];
            inst.world =
                render::translation(wx, wy, wz) * a.orient() * render::scaling(ms[0], ms[1], ms[2]);
            inst.seq = a.current_sequence();
            if (u.constructing) {
                const auto& ladder = construction_ladder(u.catalog_id);
                if (!ladder.empty()) {
                    double p = u.build_progress.to_double();
                    if (!(p >= 0.0)) p = 0.0;
                    if (p > 1.0) p = 1.0;
                    int stage = int(p * double(ladder.size()));
                    if (stage >= int(ladder.size())) stage = int(ladder.size()) - 1;
                    if (a.construction_stage != stage) {
                        const std::vector<std::string>& props = ladder[std::size_t(stage)];
                        const bool transition = std::find(props.begin(), props.end(), "Start") != props.end() ||
                                                std::find(props.begin(), props.end(), "End") != props.end();
                        const bool hold = !transition && std::find(props.begin(), props.end(), "Stand") != props.end();
                        a.anim_play("Construction", props,  hold,  1);
                        a.construction_stage = stage;
                    }
                    inst.seq = a.current_sequence();
                } else {
                    const int cseq = construction_sequence(*m, u.build_progress.to_double());
                    if (cseq >= 0) inst.seq = cseq;
                }
            } else if (a.construction_stage >= 0) {
                a.anim_clear("Construction");
                a.construction_stage = -1;
                inst.seq = a.current_sequence();
            }
            const bool on_the_job = u.construct_target != sim::kNoTarget;
            if (on_the_job && !a.anim_playing("Build")) {
                a.anim_play("Build", {"Work"},  true,  1);
                inst.seq = a.current_sequence();
            } else if (!on_the_job && a.anim_playing("Build")) {
                a.anim_clear("Build");
                inst.seq = a.current_sequence();
            }
            inst.time_ms = a.current_time();
        }
        bool selected = false;
        for (uint32_t s : selection_)
            if (s == u.id) { selected = true; break; }
        inst.team = team_colour_for(cat_, u.player, selected);
        scene.push_back(inst);
        if (u.id < driver_.actors.size() && models_ && ui_store_ && cat_) {
            render::ActorInstance& host = driver_.actors[u.id];
            for (std::size_t ci = 0; ci < host.children.size();) {
                render::ActorChild& ch = host.children[ci];
                if (!ch.rig) {
                    float sc3[3] = {1.0f, 1.0f, 1.0f};
                    const formats::Model* sm = models_->scaffold_model(*ui_store_, ch.name, sc3);
                    if (!sm) { ++ci; continue; }
                    ch.rig = std::make_shared<render::ActorInstance>();
                    ch.rig->model = sm;
                    ch.rig->seed = u.id + 7;
                    ch.rig->has_oneshot = false;
                    for (int k = 0; k < 3; ++k) ch.scale[k] = sc3[k];
                    if (auto f = cat_->actor_host_forward(ch.name)) ch.yaw = std::atan2(f->second, f->first);
                    for (const render::ActorEventRule& r : ch.rules)
                        if (r.head == "ActorCreation" && r.verb == "Create" && !r.args.empty()) {
                            auto ladder = cat_->actor_progress_stages(r.args[0]);
                            if (!ladder.empty()) ch.ladder = std::move(ladder);
                        }
                }
                render::ActorInstance& rig = *ch.rig;
                if (u.constructing && u.alive) {
                    if (!ch.ladder.empty()) {
                        double p = u.build_progress.to_double();
                        if (!(p >= 0.0)) p = 0.0;
                        if (p > 1.0) p = 1.0;
                        int stage = int(p * double(ch.ladder.size()));
                        if (stage >= int(ch.ladder.size())) stage = int(ch.ladder.size()) - 1;
                        if (ch.construction_stage != stage) {
                            const std::vector<std::string>& props = ch.ladder[std::size_t(stage)];
                            const bool transition = std::find(props.begin(), props.end(), "Start") != props.end() ||
                                                    std::find(props.begin(), props.end(), "End") != props.end();
                            const bool hold = !transition && std::find(props.begin(), props.end(), "Stand") != props.end();
                            rig.anim_play("Construction", props,  hold,  1);
                            ch.construction_stage = stage;
                        }
                    }
                } else if (!ch.finishing) {
                    const render::ActorEventRule* fin = nullptr;
                    for (const render::ActorEventRule& r : ch.rules)
                        if (r.head == "UnitConstruction" && r.sub == "Finish" && r.verb == "AnimPlay" && r.args.size() >= 2) fin = &r;
                    if (!fin || !u.alive) {
                        host.children.erase(host.children.begin() + long(ci));
                        continue;
                    }
                    rig.anim_clear("Construction");
                    std::vector<std::string> fprops;
                    for (std::size_t k = 1; k < fin->args.size(); ++k) {
                        std::size_t s0 = 0;
                        const std::string& a = fin->args[k];
                        while (s0 <= a.size()) {
                            const std::size_t e = a.find(',', s0);
                            const std::string tok = a.substr(s0, e == std::string::npos ? std::string::npos : e - s0);
                            if (!tok.empty()) fprops.push_back(tok);
                            if (e == std::string::npos) break;
                            s0 = e + 1;
                        }
                    }
                    rig.anim_play(fin->args[0], fprops, false, 2);
                    const int fseq = rig.current_sequence();
                    ch.finish_len_ms = (fseq >= 0 && std::size_t(fseq) < rig.model->sequences.size())
                                           ? float(rig.model->sequences[std::size_t(fseq)].length_ms()) : 0.0f;
                    ch.finishing = true;
                } else if (rig.current_time() + 0.5f >= ch.finish_len_ms) {
                    host.children.erase(host.children.begin() + long(ci));
                    continue;
                }
                render::SceneInstance c;
                c.model = rig.model;
                c.world = render::translation(wx, wy, wz) * render::rotation_z(ch.yaw) *
                          render::scaling(ch.scale[0], ch.scale[1], ch.scale[2]);
                c.seq = rig.current_sequence();
                c.time_ms = rig.current_time();
                c.team = inst.team;
                scene.push_back(c);
                ++ci;
            }
        }

        if (u.carrying > 0) {
            float fx = 0.0f, fy = 0.0f;
            if (u.id < prev_pos_.size() && u.id < curr_pos_.size()) {
                fx = curr_pos_[u.id].x - prev_pos_[u.id].x;
                fy = curr_pos_[u.id].y - prev_pos_[u.id].y;
            }
            const float fl = std::sqrt(fx * fx + fy * fy);
            float ox, oy, oz = 0.55f;
            render::Mat4 att{};
            const formats::Model* wmesh =
                u.id < driver_.actors.size() ? driver_.actors[u.id].model : nullptr;
            const int wseq = inst.seq;
            const float wclock =
                u.id < driver_.actors.size() ? driver_.actors[u.id].current_time() : 0.0f;
            if (wmesh && render::attachment_transform(*wmesh, "Ref_Hardpoint",
                                                      wseq >= 0 ? std::uint32_t(wseq) : 0u, wclock,
                                                      att)) {
                const float lx = att.m[12] * ms[0], ly = att.m[13] * ms[1];
                oz = att.m[14] * ms[2];
                const float cf = fl > 1e-3f ? fx / fl : 1.0f;
                const float sf = fl > 1e-3f ? fy / fl : 0.0f;
                ox = lx * cf - ly * sf;
                oy = lx * sf + ly * cf;
            } else {
                const float off = visual_r + 0.15f;
                ox = fl > 1e-3f ? fx / fl * off : off;
                oy = fl > 1e-3f ? fy / fl * off : 0.0f;
            }
            const float cs = 0.14f;
            const formats::Model* cm = nullptr;
            if (models_ && ui_store_ && u.carry_source != sim::kNoTarget &&
                u.carry_source < world_.units().size()) {
                const std::string& src = world_.units()[u.carry_source].catalog_id;
                if (!src.empty()) cm = models_->carry_model(*ui_store_, src);
            }
            if (!cm) continue;
            render::SceneInstance chunk;
            chunk.model = cm;
            chunk.world = render::translation(wx + ox, wy + oy, wz + oz) *
                          render::scaling(cs, cs, cs);
            chunk.team = {0, 0, 0, -1.0f};
            chunk.seq = -1;
            scene.push_back(chunk);
        }

        if (selected) {
            const float rr = visual_r + 0.12f;
            const float band = 0.09f;
            float inner = rr > band ? (rr - band) / rr : 0.0f;
            const int key = int(inner * 200.0f + 0.5f);
            static std::map<int, formats::Model> ring_cache;
            auto rit = ring_cache.find(key);
            if (rit == ring_cache.end()) rit = ring_cache.emplace(key, render::make_ring_model(inner)).first;
            const formats::Model* rm = &rit->second;
            constexpr std::array<float, 3> kSelectedRingRgb = {0.0f, 168.0f / 255.0f, 90.0f / 255.0f};
            std::array<float, 4> rc = u.player >= 0
                                          ? std::array<float, 4>{kSelectedRingRgb[0], kSelectedRingRgb[1],
                                                                 kSelectedRingRgb[2], 1.0f}
                                          : std::array<float, 4>{0.9f, 0.85f, 0.30f, 1.0f};
            rc[3] = 2.0f;
            render::SceneInstance ring;
            ring.model = rm;
            ring.world = render::translation(wx, wy, wz + 0.05f) * render::scaling(rr, rr, 1.0f);
            ring.team = rc;
            ring.seq = -1;
            scene.push_back(ring);
        }
    }

    for (const Corpse& c : corpses_) {
        if (!c.model) continue;
        if (fog_ready && c.player != cfg_.human_player &&
            !hud_fog_body_drawn(int(vision_.state_at(sim::Fixed::from_double(c.x), sim::Fixed::from_double(c.y))), FogVisibility(c.fog_visibility)))
            continue;
        render::SceneInstance inst;
        inst.model = c.model;
        inst.world = render::translation(c.x, c.y, c.z) * render::scaling(c.scale[0], c.scale[1], c.scale[2]);
        inst.seq = c.seq;
        inst.time_ms = c.time_ms;
        inst.team = team_colour_for(cat_, c.player, false);
        scene.push_back(inst);
    }

    if (models_ && ui_store_) {
        for (const sim::World::ShotView& sv : world_.shots_in_flight()) {
            if (sv.attacker >= world_.units().size()) continue;
            const sim::Unit& shooter = world_.units()[sv.attacker];
            const std::string& wid = sv.slot == 1 ? shooter.wpn2_id : shooter.wpn_id;
            const formats::Model* mm = models_->missile_model(*ui_store_, wid);
            if (!mm) continue;
            if (fog_ready && shooter.player != cfg_.human_player &&
                vision_.state_at(sv.x, sv.y) != sim::Visibility::Visible)
                continue;
            const float sx = float(sv.x.to_double()), sy = float(sv.y.to_double());
            float sz = ground_z_at(sx, sy);
            float yaw = 0.0f;
            if (sv.target < world_.units().size()) {
                const sim::Unit& t = world_.units()[sv.target];
                const float tx = float(t.x.to_double()), ty = float(t.y.to_double());
                if (tx != sx || ty != sy) yaw = std::atan2(ty - sy, tx - sx);
                if (t.id < driver_.actors.size()) {
                    render::ActorInstance& ta = driver_.actors[t.id];
                    render::Mat4 att;
                    const int tseq = ta.model ? ta.current_sequence() : -1;
                    if (tseq >= 0 &&
                        render::attachment_transform(*ta.model, "Ref_Center", std::uint32_t(tseq),
                                                     ta.current_time(), att))
                        sz = ground_z_at(tx, ty) + att.m[14];
                }
            }
            render::SceneInstance shot;
            shot.model = mm;
            shot.world = render::translation(sx, sy, sz) * render::rotation_z(yaw + 1.5707963f);
            shot.team = {0, 0, 0, -1.0f};
            shot.seq = -1;
            scene.push_back(shot);
        }
    }

    if (doodads_ && models_ && ui_store_) {
        for (const formats::PlacedDoodad& d : *doodads_) {
            const formats::Model* dm = models_->doodad_model(*ui_store_, d.type, d.variation);
            if (!dm) continue;
            render::SceneInstance inst;
            inst.model = dm;
            inst.world = app::doodad_world_matrix(d);
            inst.team = {0, 0, 0, -2.0f};
            std::uint64_t dseed = 1469598103934665603ull;
            for (char ch : d.type) dseed = (dseed ^ std::uint64_t(std::uint8_t(ch))) * 1099511628211ull;
            for (float f : {d.x, d.y}) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &f, sizeof(bits));
                dseed = (dseed ^ std::uint64_t(bits)) * 1099511628211ull;
            }
            inst.seq = render::select_sequence(*dm, {"Stand"}, dseed);
            scene.push_back(inst);
        }
    }
    if (cliffs_) scene.insert(scene.end(), cliffs_->begin(), cliffs_->end());
}

void MeleeGame::set_spawn_models(int player, const formats::Model* worker,
                                 const formats::Model* army, const formats::Model* producer) {
    if (player < 0) return;
    const size_t p = size_t(player);
    if (spawn_worker_model_.size() <= p) spawn_worker_model_.resize(p + 1, nullptr);
    if (spawn_army_model_.size() <= p) spawn_army_model_.resize(p + 1, nullptr);
    if (spawn_producer_model_.size() <= p) spawn_producer_model_.resize(p + 1, nullptr);
    if (worker) spawn_worker_model_[p] = worker;
    if (army) spawn_army_model_[p] = army;
    if (producer) spawn_producer_model_[p] = producer;
}

bool MeleeGame::cancel_pending_modal() {
    if (has_pending_ability_cast()) { cancel_ability_cast(); return true; }
    if (pending_warp_) {
        pending_warp_ = false;
        pending_warp_gate_ = sim::kNoTarget;
        pending_warp_unit_.clear();
    }
    if (pending_build_ || pending_refinery_) {
        pending_build_ = pending_refinery_ = false;
        return true;
    }
    if (pending_land_) { pending_land_ = false; pending_land_unit_ = sim::kNoTarget; return true; }
    if (pending_unload_) { pending_unload_ = false; pending_unload_unit_ = sim::kNoTarget; return true; }
    if (pending_defender_) { pending_defender_ = false; pending_liberator_ = sim::kNoTarget; return true; }
    if (pending_attack_ || pending_patrol_ || pending_move_) {
        pending_attack_ = pending_patrol_ = pending_move_ = false;
        return true;
    }
    if (dragging_) { dragging_ = false; return true; }
    return false;
}

void MeleeGame::pause_menu_rects(std::uint32_t view_w, std::uint32_t view_h, PauseRect& resume,
                                 PauseRect& quit) const {
    if (ui_store_ && view_w && view_h) {
        if (const HudPauseRects* pr = hud_pause_rects(*ui_store_, float(view_w) / float(view_h))) {
            const double sc = double(view_h) / double(kRefHeight);
            auto at = [sc](const HudAuthoredRects::R& r) {
                return PauseRect{int(r.x0 * sc + 0.5), int(r.y0 * sc + 0.5), int(r.x1 * sc + 0.5), int(r.y1 * sc + 0.5)};
            };
            resume = at(pr->resume);
            quit = at(pr->quit);
            if (resume.y0 != quit.y0 || resume.x0 != quit.x0) return;
        }
    }
    const int bw = 320, bh = 56, gap = 20;
    const int cx = int(view_w) / 2;
    const int cy = int(view_h) / 2;
    const int x0 = cx - bw / 2;
    resume = {x0, cy - bh - gap / 2, x0 + bw, cy - gap / 2};
    quit = {x0, cy + gap / 2, x0 + bw, cy + gap / 2 + bh};
}

MeleeGame::PauseRects MeleeGame::pause_rects_all(std::uint32_t view_w, std::uint32_t view_h) const {
    PauseRects out;
    pause_menu_rects(view_w, view_h, out.resume, out.quit);
    if (ui_store_ && view_w && view_h)
        if (const HudPauseRects* pr = hud_pause_rects(*ui_store_, float(view_w) / float(view_h))) {
            const double sc = double(view_h) / double(kRefHeight);
            auto at = [sc](const HudAuthoredRects::R& r) {
                return PauseRect{int(r.x0 * sc + 0.5), int(r.y0 * sc + 0.5), int(r.x1 * sc + 0.5), int(r.y1 * sc + 0.5)};
            };
            if (pr->save.ok()) { out.save = at(pr->save); out.has_save = true; }
            if (pr->load.ok()) { out.load = at(pr->load); out.has_load = true; }
        }
    return out;
}

void MeleeGame::ensure_audio() {
    if (audio_ || !ui_store_ || !cat_) return;
    audio_.reset(new MeleeAudio());
    if (!actor_cat_) build_actor_catalog(*cat_);
    audio_->bind_catalog(*cat_, *actor_cat_, ui_store_);
    audio_->start_device();
    driver_.audio = &audio_sink_;
}

void MeleeGame::audio_observe_events(std::vector<std::uint32_t>& played) {
    if (!audio_) return;
    const std::vector<Unit>& units = world_.units();
    const std::size_t n = units.size();
    if (audio_known_.size() < n) {
        audio_known_.resize(n, 0);
        audio_alive_.resize(n, 0);
        audio_cd_.resize(n);
    }
    const sim::Fixed zero = sim::Fixed::from_int(0);
    auto mark = [&](std::size_t id, data::UnitEvent ev) {
        played.push_back((std::uint32_t(id) << 2) | std::uint32_t(ev));
    };
    for (std::size_t i = 0; i < n; ++i) {
        const Unit& u = units[i];
        const bool audible = audio_event_audible(u);
        if (!audio_known_[i]) {
            audio_known_[i] = 1;
            if (audible && audio_seeded_ && u.player >= 0 && !u.catalog_id.empty())
                if (audio_->play_event(u.catalog_id, data::UnitEvent::Ready, 1.0f, listener_distance_to(u)))
                    mark(i, data::UnitEvent::Ready);
            audio_alive_[i] = u.alive ? 1 : 0;
            audio_cd_[i] = u.cooldown;
            continue;
        }
        if (audible && audio_alive_[i] && !u.alive && !u.catalog_id.empty())
            if (audio_->play_event(u.catalog_id, data::UnitEvent::Death, 1.0f, listener_distance_to(u)))
                mark(i, data::UnitEvent::Death);
        if (audible && u.alive && u.cooldown > audio_cd_[i] && audio_cd_[i] == zero && !u.catalog_id.empty())
            if (audio_->play_event(u.catalog_id, data::UnitEvent::Attack, kCombatCueTrim, listener_distance_to(u)))
                mark(i, data::UnitEvent::Attack);
        audio_alive_[i] = u.alive ? 1 : 0;
        audio_cd_[i] = u.cooldown;
    }
    audio_seeded_ = true;
}

void MeleeGame::audio_ui_event(std::uint32_t unit_id, data::UnitEvent ev, float gain) {
    if (!audio_ || unit_id == sim::kNoTarget || unit_id >= world_.units().size()) return;
    const Unit& u = world_.units()[unit_id];
    if (!u.alive || u.player != cfg_.human_player || u.catalog_id.empty()) return;
    audio_->play_event(u.catalog_id, ev, gain, listener_distance_to(u));
}

bool MeleeGame::step(const render::InputState& in, float dt_ms,
                     std::vector<render::SceneInstance>& scene, render::Camera& cam) {
    ensure_audio();
    const float dt_s = dt_ms / 1000.0f;
    clock_ms_ += double(dt_ms);
    user_scrolled_ = false;
    const float aspect = in.view_h ? float(in.view_w) / float(in.view_h) : 1.0f;

    if (dt_ms > 0.0f) {
        const float inst = 1000.0f / dt_ms;
        fps_smooth_ = fps_smooth_ > 0.0f ? fps_smooth_ * 0.9f + inst * 0.1f : inst;
    }
    mouse_x_ = in.mouse_x;
    mouse_y_ = in.mouse_y;

    const bool esc_edge = in.quit && !prev_esc_;
    prev_esc_ = in.quit;
    if (esc_edge) {
        if (pause_menu_open_) { pause_menu_open_ = false; clock_.set_paused(false); }
        else if (cancel_pending_modal()) {   }
        else { pause_menu_open_ = true; clock_.set_paused(true); }
    }
    if (pause_menu_open_) {
        if (in.left_pressed && in.view_w && in.view_h) {
            PauseRect resume, quit;
            pause_menu_rects(in.view_w, in.view_h, resume, quit);
            auto hit = [&](const PauseRect& r) {
                return in.mouse_x >= float(r.x0) && in.mouse_x < float(r.x1) &&
                       in.mouse_y >= float(r.y0) && in.mouse_y < float(r.y1);
            };
            if (hit(resume)) { pause_menu_open_ = false; clock_.set_paused(false); }
            else if (hit(quit)) {
                if (cfg_.human_player >= 0) surrender(cfg_.human_player);
                return false;
            }
            else {
                const PauseRects all = pause_rects_all(in.view_w, in.view_h);
                if (all.has_save && hit(all.save) && on_save_) on_save_();
                else if (all.has_load && hit(all.load) && on_load_) on_load_();
            }
        }
        emit_scene(scene);
        cam = camctl_.camera();
        return true;
    }

    update_camera(in, dt_s);

    if (!framed_ && in.view_w && in.view_h) {
        std::vector<render::Vec3> pts;
        for (const Unit& u : world_.units())
            if (u.alive) pts.push_back({float(u.x.to_double()), float(u.y.to_double()), 0.0f});
        camctl_.fit(pts, aspect);
        framed_ = true;
    }

    if (!replay_drive_) handle_commands(in, aspect);

    if (!replay_drive_ && cfg_.human_player >= 0 && size_t(cfg_.human_player) < layout_.players.size()) {
        const MeleePlayerLayout& pl = layout_.players[size_t(cfg_.human_player)];

        if (human_producer_ != sim::kNoTarget &&
            (human_producer_ >= world_.units().size() || !world_.unit(human_producer_).alive))
            human_producer_ = sim::kNoTarget;

        if (human_producer_ == sim::kNoTarget) {
            for (const Unit& u : world_.units())
                if (u.alive && u.player == cfg_.human_player && u.speed.raw() == 0 &&
                    !u.constructing && u.id != pl.base.depot && u.supply_provides.raw() == 0) {
                    human_producer_ = u.id;
                    human_producer_pending_ = false;
                    break;
                }
        }

        auto selected_worker = [&]() -> std::uint32_t {
            for (std::uint32_t id : selection_)
                if (id < world_.units().size() && world_.unit(id).alive &&
                    world_.unit(id).is_worker && world_.unit(id).player == cfg_.human_player)
                    return id;
            return sim::kNoTarget;
        };

        const bool do_build = (in.key_build && !prev_keys_.build);
        if (do_build && !human_producer_pending_ && human_producer_ == sim::kNoTarget &&
            selected_worker() != sim::kNoTarget) {
            pending_build_ = true;
            build_proto_ = human_build_.producer_proto;
            build_cost_ = human_build_.producer_cost;
            build_gas_cost_ = human_build_.producer_gas;
            build_time_ = human_build_.producer_build_time;
            pending_attack_ = pending_patrol_ = pending_move_ = false;
        }

        const bool do_train = (in.key_train && !prev_keys_.train);
        if (do_train && human_producer_ != sim::kNoTarget) {
            const ai::BuildItem& au = pl.config.army_unit;
            MeleeCommands::train(world_, human_producer_, au.proto, au.minerals, au.gas,
                                 au.build_time);
        }

        std::vector<std::uint32_t> issued_this_frame;
        for (std::size_t i = 0; i < build_queue_.size();) {
            const QueuedBuild& q = build_queue_[i];
            const bool worker_free =
                q.worker < world_.units().size() && world_.unit(q.worker).alive &&
                !world_.worker_committed(q.worker) &&
                std::find(issued_this_frame.begin(), issued_this_frame.end(), q.worker) ==
                    issued_this_frame.end();
            if (!worker_free || world_.player(cfg_.human_player).minerals < q.cost ||
                world_.player(cfg_.human_player).gas < q.gas) {
                ++i;
                continue;
            }
            if (world_.issue_worker_build(q.worker, q.x, q.y, q.proto, q.cost, q.gas, q.time,
                                          sim::Fixed::from_int(2), build_verb_for(q.worker)) &&
                q.is_producer)
                human_producer_pending_ = true;
            issued_this_frame.push_back(q.worker);
            build_queue_.erase(build_queue_.begin() + i);
        }
    }
    prev_keys_.train = in.key_train;
    prev_keys_.build = in.key_build;

    if (cfg_.human_player >= 0 && cat_ && !cat_->hotkeys().empty()) {
        groups_.prune(world_);
        render::InputState kin = in;
        for (int d = 0; d < 10; ++d) {
            if (in.key_num[d] && !prev_num_[d]) kin.key_tokens.push_back(std::string(1, char('0' + d)));
            prev_num_[d] = in.key_num[d];
        }
        const data::StringTable& hk = cat_->hotkeys();
        auto steal = [&](int keep) {
            for (int o = 0; o < 10; ++o) {
                if (o == keep) continue;
                std::vector<uint32_t> rest;
                for (uint32_t id : groups_.recall(o))
                    if (std::find(selection_.begin(), selection_.end(), id) == selection_.end()) rest.push_back(id);
                set_control_group(o, rest);
            }
        };
        for (int d = 0; d < 10; ++d) {
            const std::string n = std::to_string(d);
            if (ui_hotkey_fires(hk, ("ControlGroupAssignAndSteal" + n).c_str(), kin)) {
                steal(d);
                set_control_group(d, selection_);
            } else if (ui_hotkey_fires(hk, ("ControlGroupAppendAndSteal" + n).c_str(), kin)) {
                steal(d);
                groups_.append(d, selection_);
            } else if (ui_hotkey_fires(hk, ("ControlGroupAssign" + n).c_str(), kin)) {
                set_control_group(d, selection_);
            } else if (ui_hotkey_fires(hk, ("ControlGroupAppend" + n).c_str(), kin)) {
                groups_.append(d, selection_);
            } else if (ui_hotkey_fires(hk, ("ControlGroupRecall" + n).c_str(), kin)) {
                std::vector<uint32_t> g = groups_.recall(d);
                if (!g.empty()) set_selection(g);
                recall_double_tap(d);
            }
        }
        if (ui_hotkey_fires(hk, "IdleWorker", kin)) select_next_idle_worker();
        if (ui_hotkey_fires(hk, "ArmySelect", kin)) {
            std::vector<uint32_t> army;
            for (const Unit& u : world_.units())
                if (u.alive && u.player == cfg_.human_player && !u.catalog_id.empty() &&
                    cat_->unit_flag(u.catalog_id, "ArmySelect"))
                    army.push_back(u.id);
            set_selection(army);
        }
        if (ui_hotkey_fires(hk, "TownCamera", kin)) {
            std::vector<uint32_t> halls;
            for (const Unit& u : world_.units())
                if (u.alive && u.player == cfg_.human_player && !u.constructing && !u.flying &&
                    (u.attributes & sim::kAttrStructure) != 0 && !u.catalog_id.empty() &&
                    is_resource_drop_off(*cat_, u.catalog_id, "Minerals"))
                    halls.push_back(u.id);
            if (!halls.empty()) {
                town_camera_next_ %= halls.size();
                const Unit& h = world_.unit(halls[town_camera_next_++]);
                camctl_.focus.x = float(h.x.to_double());
                camctl_.focus.y = float(h.y.to_double());
                camctl_.focus.z = ground_z_at(camctl_.focus.x, camctl_.focus.y);
            }
        }
    } else if (cfg_.human_player >= 0) {
        groups_.prune(world_);
        for (int d = 0; d < 10; ++d) {
            if (in.key_num[d] && !prev_num_[d]) {
                if (in.key_ctrl) {
                    set_control_group(d, selection_);
                } else {
                    std::vector<uint32_t> g = groups_.recall(d);
                    if (!g.empty()) set_selection(g);
                    recall_double_tap(d);
                }
            }
            prev_num_[d] = in.key_num[d];
        }
    }

    if (cfg_.human_player >= 0 && cat_ && !cat_->hotkeys().empty()) {
        const data::StringTable& hk = cat_->hotkeys();
        if (ui_hotkey_fires(hk, "SubgroupNext", in) || ui_hotkey_fires(hk, "SubgroupPrev", in)) {
            rebuild_subgroups();
            const std::size_t n = sel_groups_.subgroups.size();
            if (n > 1) {
                const bool next = ui_hotkey_fires(hk, "SubgroupNext", in);
                sel_groups_.active = next ? (sel_groups_.active + 1) % n : (sel_groups_.active + n - 1) % n;
            }
        }
        for (int k = 0; k < CameraBookmarks::kSlots; ++k) {
            const std::string save = "CameraSave" + std::to_string(k), view = "CameraView" + std::to_string(k);
            if (ui_hotkey_fires(hk, save.c_str(), in)) {
                bookmarks_.save(k, CameraPose{camctl_.focus.x, camctl_.focus.y, camctl_.distance});
            } else if (ui_hotkey_fires(hk, view.c_str(), in)) {
                CameraPose pose;
                if (bookmarks_.recall(k, pose)) {
                    camctl_.focus.x = pose.x;
                    camctl_.focus.y = pose.y;
                    camctl_.focus.z = ground_z_at(pose.x, pose.y);
                    camctl_.distance = pose.zoom;
                }
            }
        }
        if (ui_hotkey_fires(hk, "CameraFollow", in)) {
            if (follow_.active) follow_.stop();
            else if (!selection_.empty()) follow_.start();
        } else if (ui_hotkey_fires(hk, "CameraCenter", in)) {
            float cx = 0.0f, cy = 0.0f;
            if (selection_centre(cx, cy)) camctl_.focus = {cx, cy, ground_z_at(cx, cy)};
        }
        const int sp = int(clock_.speed());
        if (ui_hotkey_fires(hk, "GameSpeedInc", in) && sp < int(GameClock::Speed::Faster))
            set_speed(GameClock::Speed(sp + 1));
        else if (ui_hotkey_fires(hk, "GameSpeedDec", in) && sp > int(GameClock::Speed::Slower))
            set_speed(GameClock::Speed(sp - 1));
    }

    const float tick_ms =
        (1000.0f / float(sim::World::kTicksPerSecond)) / float(clock_.multiplier(clock_.speed()));
    accum_ms_ += clock_.paused() ? 0.0f : dt_ms;
    if (accum_ms_ > tick_ms * 8.0f) accum_ms_ = tick_ms * 8.0f;
    if (curr_pos_.empty()) snapshot_positions(curr_pos_);
    bool ticked = false;
    while (accum_ms_ >= tick_ms) {
        if (replay_drive_) {
            if (world_.tick() >= replay_total_) { accum_ms_ = 0.0f; break; }
            inject_replay_orders(world_.tick());
        }
        prev_pos_ = curr_pos_;
        world_.step();
        if (research_) research_->apply_completed();
        if (research_ && cfg_.human_player >= 0)
            for (const std::pair<std::int32_t, std::string>& done : research_->just_completed())
                if (int(done.first) == cfg_.human_player) {
                    const std::string sid = alert_sound_id(AlertKind::UpgradeComplete);
                    if (!sid.empty()) {
                        last_alert_sound_ = sid;
                        if (audio_) audio_->play_sound(sid, 1.0f);
                    }
                    break;
                }
        snapshot_positions(curr_pos_);
        if (prev_pos_.size() < curr_pos_.size()) {
            std::size_t old = prev_pos_.size();
            prev_pos_.resize(curr_pos_.size());
            for (std::size_t k = old; k < curr_pos_.size(); ++k) prev_pos_[k] = curr_pos_[k];
        }
        accum_ms_ -= tick_ms;
        ticked = true;
        if (!replay_drive_ && ++tick_counter_ >= cfg_.ai_period_ticks) {
            tick_counter_ = 0;
            for (size_t p = 0; p < ais_.size(); ++p)
                if (cfg_.human_player < 0 || int(p) != cfg_.human_player) {
                    ais_[p]->update(world_);
                    maybe_ai_research(int(p));
                    if (cast_policy_) cast_policy_->run(world_, int(p));
                }
            if (cast_policy_ && cfg_.human_player >= 0)
                cast_policy_->run_autocast(world_, cfg_.human_player);
        }
    }

    for (std::size_t id = bound_count_; id < world_.units().size(); ++id) {
        const Unit& u = world_.unit(std::uint32_t(id));
        if (const render::ActorRules* ar = rules_for(u.catalog_id))
            driver_.set_rules(std::uint32_t(id), ar);
        if (u.player < 0) continue;
        if (cat_ && u.speed.raw() == 0 && (u.attributes & sim::kAttrStructure) != 0)
            driver_.set_spawn_facing(std::uint32_t(id), cat_->unit_facing_radians(u.catalog_id));
        if (models_ && ui_store_) {
            if (u.constructing) {
                if (const formats::Model* bm = models_->ensure_build(*ui_store_, u.catalog_id)) {
                    driver_.set_model(std::uint32_t(id), bm);
                    continue;
                }
            }
            if (const formats::Model* own = models_->ensure(*ui_store_, u.catalog_id)) {
                driver_.set_model(std::uint32_t(id), own);
                continue;
            }
        }
        if (u.spawn_parent != sim::kNoTarget) {
            driver_.set_model(std::uint32_t(id), models_ ? models_->model_for(u.catalog_id) : nullptr);
            continue;
        }
        const std::vector<const formats::Model*>* tbl;
        const formats::Model* fallback;
        if (u.speed.raw() == 0) { tbl = &spawn_producer_model_; fallback = producer_model_; }
        else if (u.is_worker)   { tbl = &spawn_worker_model_;   fallback = nullptr; }
        else                    { tbl = &spawn_army_model_;     fallback = driver_.default_model; }
        const formats::Model* m =
            (size_t(u.player) < tbl->size() && (*tbl)[size_t(u.player)]) ? (*tbl)[size_t(u.player)]
                                                                         : fallback;
        driver_.set_model(std::uint32_t(id), m);
        if (u.speed.raw() == 0 && (u.attributes & sim::kAttrStructure) != 0)
            driver_.set_spawn_facing(std::uint32_t(id), cat_ && !u.catalog_id.empty()
                                                            ? cat_->unit_facing_radians(u.catalog_id)
                                                            : kPlacedStructureFacing);
    }
    bound_count_ = world_.units().size();
    if (models_ && ui_store_) {
        const std::size_t n = world_.units().size();
        if (bound_cat_.size() < n) bound_cat_.resize(n);
        if (bound_building_.size() < n) bound_building_.resize(n, 0);
        for (std::size_t id = 0; id < n; ++id) {
            const Unit& bu = world_.unit(std::uint32_t(id));
            const std::string& cid = bu.catalog_id;
            const std::uint8_t building = bu.constructing ? 1 : 0;
            if (bound_cat_[id] == cid && bound_building_[id] == building) continue;
            bound_cat_[id] = cid;
            bound_building_[id] = building;
            if (cid.empty()) continue;
            const formats::Model* m = building ? models_->ensure_build(*ui_store_, cid) : nullptr;
            if (!m) m = models_->ensure(*ui_store_, cid);
            if (m) driver_.set_model(std::uint32_t(id), m);
        }
    }

    driver_.observe(world_, dt_ms, ticked);
    if (audio_) {
        std::vector<std::uint32_t> played;
        if (ticked) audio_observe_events(played);
        for (const render::AudioSink::Cue& cue : audio_sink_.drain()) {
            std::optional<data::UnitEvent> ev = classify_actor_cue(cue.name);
            if (!ev || cue.unit >= world_.units().size()) continue;
            const Unit& cu = world_.units()[cue.unit];
            if (cu.catalog_id.empty()) continue;
            const std::uint32_t key = (cue.unit << 2) | std::uint32_t(*ev);
            if (std::find(played.begin(), played.end(), key) != played.end()) continue;
            audio_->play_event(cu.catalog_id, *ev, kCombatCueTrim, listener_distance_to(cu));
        }
    }
    if (ticked) {
        alerts_.observe(world_, messages_, dt_s);
        for (const AlertFired& f : alerts_.fired()) {
            if (cat_ && (f.kind == AlertKind::AttackUnit || f.kind == AlertKind::AttackTown)) {
                const int race = human_race();
                const std::string id = std::string(f.kind == AlertKind::AttackUnit ? "AttackUnit" : "AttackTown") +
                                       (race == 1 ? "_Prot" : race == 2 ? "_Zerg" : "_Terr");
                const auto icon = cat_->scalar("CAlert", id, "Icon");
                const auto life = cat_->scalar("CAlert", id, "Life");
                for (auto it = messages_.mutable_all().rbegin(); it != messages_.mutable_all().rend(); ++it)
                    if (it->zone == MsgZone::Alert && it->has_target && std::fabs(it->tx - f.x) < 1e-3f && std::fabs(it->ty - f.y) < 1e-3f) {
                        if (icon && !icon->empty()) it->icon = *icon;
                        if (life && !life->empty()) it->ttl = std::max(it->ttl, float(std::atof(life->c_str())));
                        break;
                    }
            }
            const std::string sid = alert_sound_id(f.kind);
            if (sid.empty()) continue;
            last_alert_sound_ = sid;
            if (audio_) audio_->play_sound(sid, 1.0f);
        }
    }
    if (world_.last_error() != sim::CmdError::None) {
        if (cat_ && world_.last_error_player() == cfg_.human_player) {
            const char* key = sim::cmd_error_key(world_.last_error());
            if (const std::string* text = cat_->strings().find(key); text && !text->empty())
                messages_.emit(MsgZone::Error, *text);
            last_error_sound_ = error_sound_id(world_.last_error());
            if (audio_ && !last_error_sound_.empty()) audio_->play_sound(last_error_sound_, 1.0f);
        }
        world_.clear_last_error();
    }
    messages_.advance(dt_s);

    if (ticked) {
        auto is_structure = [](const Unit& u) { return u.player >= 0 && u.speed.raw() == 0; };
        score_.observe(world_, is_structure);
        if (death_seen_.size() < world_.units().size())
            death_seen_.resize(world_.units().size(), 0);
        for (std::uint32_t id = 0; id < world_.units().size(); ++id) {
            const Unit& u = world_.unit(id);
            if (!u.alive && !death_seen_[id]) {
                death_seen_[id] = 1;
                if (u.player >= 0) {
                    spawn_corpse(u);
                    splat_quads_.erase(id);
                }
            }
        }
    }
    if (!corpses_.empty()) {
        for (Corpse& c : corpses_) c.time_ms += dt_s * 1000.0f;
        corpses_.erase(std::remove_if(corpses_.begin(), corpses_.end(),
                                      [](const Corpse& c) { return c.time_ms >= c.length_ms; }),
                       corpses_.end());
    }
    if (!cfg_.no_victory && surrendered_player_ < 0)
        outcome_ = MatchResult::evaluate(world_, cfg_.human_player >= 0 ? cfg_.human_player : 0, stalemate_ticks_);
    if (audio_) {
        const MeleeAudio::MixState want = outcome_ == MatchResult::Outcome::Victory
                                              ? MeleeAudio::MixState::Victory
                                              : MeleeAudio::MixState::Game;
        if (audio_->mix_state() != want) audio_->apply_mix_state(want);
    }
    if (outcome_ == MatchResult::Outcome::Victory && cfg_.human_player >= 0 && cat_) {
        const int race = human_race();
        const std::string victory = std::string("MusicVictory_") + (race == 1 ? "Prot" : race == 2 ? "Zerg" : "Terr");
        if (music_.soundtrack_id != victory && (music_.next.empty() || music_.soundtrack_id.rfind("MusicVictory_", 0) != 0))
            start_music(victory);
    }
    music_tick(dt_s);
    emit_scene(scene);
    cam = camctl_.camera();
    return !in.quit;
}

static std::uint32_t type_hash(const std::string& s) {
    std::uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= std::uint8_t(c);
        h *= 16777619u;
    }
    return h;
}

void MeleeGame::rebuild_subgroups() {
    std::vector<std::uint32_t> units;
    std::vector<std::uint32_t> types;
    std::vector<int> prio;
    units.reserve(selection_.size());
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size() || !world_.unit(id).alive) continue;
        const Unit& u = world_.unit(id);
        const std::string& tok = id < layout_.unit_type.size() && !layout_.unit_type[id].empty()
                                     ? layout_.unit_type[id]
                                     : u.catalog_id;
        units.push_back(id);
        types.push_back(type_hash(cat_ && !tok.empty() ? cat_->unit_subgroup_alias(tok) : tok));
        std::optional<int> authored;
        if (cat_ && !tok.empty()) authored = cat_->unit_subgroup_priority(tok);
        int p;
        if (authored) {
            p = *authored;
        } else {
            p = 1;
            if (u.speed.raw() == 0) p = 8;
            else if (u.wpn_damage.raw() > 0) p = 5;
        }
        prio.push_back(p);
    }
    const std::size_t prev_active = sel_groups_.active;
    sel_groups_ = SelectionGroups::build(units, types, prio);
    std::vector<std::uint32_t> members = units;
    std::sort(members.begin(), members.end());
    if (members == sel_groups_members_ && prev_active < sel_groups_.subgroups.size()) sel_groups_.active = prev_active;
    sel_groups_members_ = std::move(members);
}

std::vector<std::uint32_t> MeleeGame::subgroup_ordered_selection() const {
    if (sel_groups_.subgroups.empty()) return selection_;
    const std::size_t a =
        sel_groups_.active < sel_groups_.subgroups.size() ? sel_groups_.active : 0;
    std::vector<std::uint32_t> ordered = sel_groups_.subgroups[a].units;
    for (std::uint32_t id : selection_) {
        bool in_active = false;
        for (std::uint32_t x : sel_groups_.subgroups[a].units)
            if (x == id) { in_active = true; break; }
        if (!in_active) ordered.push_back(id);
    }
    return ordered;
}

void MeleeGame::ensure_minimap_terrain() {
    if (minimap_terrain_baked_) return;
    minimap_terrain_baked_ = true;
    const sim::NavGrid& nav = world_.nav();
    if (!nav.configured()) return;
    const int gw = nav.width(), gh = nav.height();
    if (gw <= 0 || gh <= 0) return;
    std::vector<std::uint8_t> level(std::size_t(gw) * std::size_t(gh), 0);
    std::vector<std::uint8_t> blocked(std::size_t(gw) * std::size_t(gh), 0);
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            const std::size_t i = std::size_t(y) * std::size_t(gw) + std::size_t(x);
            level[i] = nav.level_cell(x, y);
            blocked[i] = nav.is_blocked_cell(x, y) ? 1 : 0;
        }
    auto cap = [](int n) { return n > 128 ? 128 : (n < 1 ? 1 : n); };
    minimap_tw_ = cap(gw);
    minimap_th_ = cap(gh);
    minimap_terrain_ =
        hud_bake_minimap_terrain(level, blocked, gw, gh, minimap_tw_, minimap_th_);
    if (minimap_terrain_.empty()) { minimap_tw_ = minimap_th_ = 0; }
}

void MeleeGame::update_minimap_fog(int viewed) {
    if (map_w_ <= 0.0f || map_h_ <= 0.0f) { minimap_fog_.clear(); minimap_fw_ = minimap_fh_ = 0; return; }
    const sim::Bounds bounds{sim::Fixed(), sim::Fixed(),
                             sim::Fixed::from_double(map_w_), sim::Fixed::from_double(map_h_)};
    vision_.compute(world_, viewed,   sim::Fixed::from_int(10),
                      sim::Fixed::from_int(2), bounds,
                    world_.placement().configured() ? &world_.placement() : nullptr);
    const int fw = vision_.width(), fh = vision_.height();
    if (fw <= 0 || fh <= 0) { minimap_fog_.clear(); minimap_fw_ = minimap_fh_ = 0; return; }
    minimap_fw_ = fw;
    minimap_fh_ = fh;
    minimap_fog_.assign(std::size_t(fw) * std::size_t(fh), 0);
    for (int cy = 0; cy < fh; ++cy)
        for (int cx = 0; cx < fw; ++cx)
            minimap_fog_[std::size_t(cy) * std::size_t(fw) + std::size_t(cx)] =
                std::uint8_t(vision_.state(cx, cy));
    ground_fog_field_.w = std::uint32_t(fw);
    ground_fog_field_.h = std::uint32_t(fh);
    ground_fog_field_.world_w = map_w_;
    ground_fog_field_.world_h = map_h_;
    ground_fog_field_.alpha.resize(minimap_fog_.size());
    for (std::size_t i = 0; i < minimap_fog_.size(); ++i) ground_fog_field_.alpha[i] = hud_fog_veil_alpha(int(minimap_fog_[i]));
}

void MeleeGame::ensure_overlay_font() {
    if (overlay_font_tried_) {
        if (overlay_font_.valid()) draw::set_active_font(&overlay_font_);
        return;
    }
    overlay_font_tried_ = true;
    if (!ui_store_) return;
    if (const ui::StyleSet* ss = hud_style_set()) {
        const ui::FontStyle rp = ss->resolve(kHudReadoutStyle);
        if (!rp.font.empty()) {
            try {
                auto bytes = ui_store_->read_by_name("mods/core.sc2mod/base.sc2data/" + rp.font);
                if (overlay_font_.load(bytes)) {
                    draw::set_active_font(&overlay_font_);
                    return;
                }
            } catch (const std::exception&) {   }
        }
    }
    static const char* kCandidates[] = {
        "mods/core.sc2mod/base.sc2data/UI/Fonts/Eurostile-Reg.otf",
        "mods/core.sc2mod/base.sc2data/UI/Fonts/Eurostile-Bol.otf",
        "mods/core.sc2mod/base.sc2data/UI/Fonts/BLQ55.ttf",
        "mods/core.sc2mod/base.sc2data/UI/Fonts/BLQ85.ttf",
        "mods/core.sc2mod/base.sc2data/UI/Fonts/bl.ttf",
    };
    for (const char* path : kCandidates) {
        try {
            auto bytes = ui_store_->read_by_name(path);
            if (overlay_font_.load(bytes)) { draw::set_active_font(&overlay_font_); return; }
        } catch (const std::exception&) {   }
    }
}

const ui::StyleSet* MeleeGame::hud_style_set() {
    if (!hud_style_tried_) {
        hud_style_tried_ = true;
        if (ui_store_) {
            try {
                auto b = ui_store_->read_by_name(
                    "mods/core.sc2mod/base.sc2data/UI/FontStyles.SC2Style");
                hud_style_.ingest(std::string(b.begin(), b.end()));
            } catch (const std::exception&) {   }
        }
    }
    return hud_style_.style_count() ? &hud_style_ : nullptr;
}

void MeleeGame::ensure_console_texture() {
    if (console_tried_) return;
    console_tried_ = true;
    if (!ui_store_) return;
    if (console_renderer_ && cat_) {
        console_tex_ = console_renderer_(*ui_store_, *cat_,
                                         console_skin_id_for_race(human_race()), kConsoleFrameWidth);
        if (console_tex_.ok()) return;
    }
    console_tex_ = load_console(*ui_store_, human_race(), cat_);
}

ConsolePanel MeleeGame::console_view() const {
    ConsolePanel p;
    if (console_tex_.ok()) {
        p.rgba = &console_tex_.rgba;
        p.tw = int(console_tex_.w);
        p.th = int(console_tex_.h);
        p.race = human_race();
    }
    p.authored = hud_authored_;
    return p;
}

void MeleeGame::refresh_hud_layout(std::uint32_t view_w, std::uint32_t view_h) {
    if (!ui_store_ || !view_w || !view_h) return;
    hud_authored_ = hud_authored_rects(*ui_store_, float(view_w) / float(view_h));
}

void MeleeGame::build_overlay(std::uint32_t view_w, std::uint32_t view_h,
                              std::vector<std::uint8_t>& out) {
    out.clear();
    if (!view_w || !view_h) return;
    hud_unit_pane_ = HudUnitPane{};
    hud_portrait_ = HudPortrait{};
    ensure_overlay_font();
    const int viewed = cfg_.human_player >= 0 ? cfg_.human_player : 0;

    const float aspect = float(view_w) / float(view_h);

    rebuild_subgroups();

    HudContent content = HudModel::build(world_, viewed, subgroup_ordered_selection());
    if (cat_ && content.lead_unit < world_.units().size()) {
        switch (cat_->unit_race_index(world_.units()[content.lead_unit].catalog_id)) {
            case 1: content.lead_wireframe.race = WireRace::Protoss; break;
            case 2: content.lead_wireframe.race = WireRace::Zerg; break;
            case 0: content.lead_wireframe.race = WireRace::Terran; break;
            default: break;
        }
    }
    if (cfg_.human_player >= 0) {
        const char* banner = MatchResult::banner(outcome_);
        if (banner[0] != '\0') content.selection_summary = banner;
    }
    if (selection_.size() == 1 && selection_[0] < world_.units().size()) {
        const Unit& s = world_.unit(selection_[0]);
        if (s.player < 0) {
            const bool gas = selection_[0] < layout_.unit_type.size() &&
                             layout_.unit_type[selection_[0]] == "VespeneGeyser";
            char b[40];
            std::snprintf(b, sizeof(b), "%s: %d", gas ? "Vespene" : "Minerals", s.resource_left);
            content.selection_summary = b;
        }
    }
    const bool authored_alert_panel = cfg_.human_player >= 0 && ui_store_ && cat_ && hud_authored_ && hud_authored_->alert.ok();
    content.game_seconds = world_.tick() / sim::World::kTicksPerSecond;
    content.idle_workers = idle_worker_count();
    if (const std::uint32_t lead = cast_lead(); lead < world_.units().size()) {
        const Unit& lu = world_.unit(lead);
        content.equipment = equipment_for(lu.player);
        content.lead_kills = lu.kills;
    }
    for (const HudMessage& mm : messages_.all()) {
        if (authored_alert_panel && mm.zone == MsgZone::Alert) continue;
        std::string t = mm.text;
        if (mm.count > 1) t += " x" + std::to_string(mm.count);
        content.messages.push_back({mm.zone, t});
    }

    CameraFootprint cam;
    if (map_w_ > 0.0f && map_h_ > 0.0f) {
        Control ctrl = make_control(camctl_.view_proj(aspect), view_w, view_h, viewed);
        const float cxs[4] = {0.0f, float(view_w), float(view_w), 0.0f};
        const float cys[4] = {0.0f, 0.0f, float(view_h), float(view_h)};
        cam.valid = true;
        for (int k = 0; k < 4 && cam.valid; ++k) {
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{cxs[k], cys[k]}, wx, wy)) {
                cam.x[k] = float(wx.to_double());
                cam.y[k] = float(wy.to_double());
            } else {
                cam.valid = false;
            }
        }
    }

    ensure_minimap_terrain();
    update_minimap_fog(viewed);
    MinimapBackdrop backdrop;
    backdrop.extent = minimap_extent();
    if (!minimap_terrain_.empty()) {
        backdrop.terrain = &minimap_terrain_;
        backdrop.tw = minimap_tw_;
        backdrop.th = minimap_th_;
    }
    if (!minimap_fog_.empty()) {
        backdrop.fog = &minimap_fog_;
        backdrop.fw = minimap_fw_;
        backdrop.fh = minimap_fh_;
        backdrop.viewer_player = cfg_.human_player;
    }
    ensure_console_texture();
    refresh_hud_layout(view_w, view_h);
    int forced_px = 0;
    if (const ui::StyleSet* ss = hud_style_set()) {
        const ui::FontStyle rp = ss->resolve(kHudReadoutStyle);
        if (rp.ok()) forced_px = int(double(rp.height) * double(view_h) / double(kRefHeight) + 0.5);
    }
    const ConsolePanel console_panel = console_view();
    std::vector<std::uint32_t> group_pane;
    int group_pages = 0;
    if (cfg_.human_player >= 0 && console_panel.authored && console_panel.authored->group_slots_ok) {
        std::vector<std::uint32_t> full;
        for (std::uint32_t id : subgroup_ordered_selection()) {
            if (id >= world_.units().size()) continue;
            const Unit& gu = world_.unit(id);
            if (gu.alive && gu.player == cfg_.human_player) full.push_back(id);
        }
        if (full.size() >= 2) {
            group_pages = std::min(6, int((full.size() + std::size_t(kGroupSlots) - 1) / std::size_t(kGroupSlots)));
            if (group_page_ >= group_pages) group_page_ = group_pages - 1;
            if (group_page_ < 0) group_page_ = 0;
            const std::size_t first = std::size_t(group_page_) * std::size_t(kGroupSlots);
            for (std::size_t i = first; i < full.size() && i < first + std::size_t(kGroupSlots); ++i) group_pane.push_back(full[i]);
        }
    }
    if (group_pane.size() < 2 && group_pages < 2) group_pane.clear();
    hud_group_pane_ids_ = group_pane;
    if (!group_pane.empty() && MatchResult::banner(outcome_)[0] == '\0')
        content.selection_summary.clear();
    const bool unit_pane = cfg_.human_player >= 0 && selection_.size() == 1 && selection_[0] < world_.units().size() &&
                           world_.unit(selection_[0]).alive && world_.unit(selection_[0]).player == cfg_.human_player &&
                           console_panel.authored && console_panel.authored->info.ok() && MatchResult::banner(outcome_)[0] == '\0';
    if (unit_pane) content.selection_summary.clear();
    draw::set_forced_font_px(forced_px);
    std::vector<std::uint8_t> hud =
        HudRender::render(content, world_, map_w_, map_h_, view_w, view_h, &cam, &backdrop,
                          console_tex_.ok() ? &console_panel : nullptr);
    draw::set_forced_font_px(0);
    std::vector<std::uint8_t> units =
        unit_overlay_.render(world_, camctl_.view_proj(aspect), view_w, view_h, {}, viewed);

    out.assign(std::size_t(view_w) * view_h * 4, 0);

    if (!ground_fog_ && cfg_.human_player >= 0 && map_w_ > 0.0f && map_h_ > 0.0f && !minimap_fog_.empty()) {
        Control fctrl = make_control(camctl_.view_proj(aspect), view_w, view_h, viewed);
        constexpr int kStep = 6;

        const int mask_w = (int(view_w) + kStep - 1) / kStep;
        const int mask_h = (int(view_h) + kStep - 1) / kStep;
        std::vector<std::uint8_t> own_mask(std::size_t(mask_w) * std::size_t(mask_h), 0);
        {
            const render::Mat4 mvp = camctl_.view_proj(aspect);
            auto project = [&](float x, float y, float z, float& sx, float& sy) -> bool {
                const float cx = mvp.m[0] * x + mvp.m[4] * y + mvp.m[8] * z + mvp.m[12];
                const float cy = mvp.m[1] * x + mvp.m[5] * y + mvp.m[9] * z + mvp.m[13];
                const float cw = mvp.m[3] * x + mvp.m[7] * y + mvp.m[11] * z + mvp.m[15];
                if (cw <= 1e-6f) return false;
                sx = (cx / cw * 0.5f + 0.5f) * float(view_w);
                sy = (cy / cw * 0.5f + 0.5f) * float(view_h);
                return true;
            };
            for (const Unit& u : world_.units()) {
                if (!u.alive || u.player != viewed) continue;
                const float ux = float(u.x.to_double()), uy = float(u.y.to_double());
                const float uz = ground_z_at(ux, uy);
                const formats::Model* m =
                    (u.id < driver_.actors.size()) ? driver_.actors[u.id].model : nullptr;
                float lo[3], hi[3];
                if (m) {
                    for (int k = 0; k < 3; ++k) { lo[k] = m->bounds_min[k]; hi[k] = m->bounds_max[k]; }
                } else {
                    const float r = float(u.radius.to_double());
                    lo[0] = lo[1] = -r; hi[0] = hi[1] = r;
                    lo[2] = 0.0f; hi[2] = r;
                }
                float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                bool any = false;
                for (int c = 0; c < 8; ++c) {
                    const float px = ux + ((c & 1) ? hi[0] : lo[0]);
                    const float py = uy + ((c & 2) ? hi[1] : lo[1]);
                    const float pz = uz + ((c & 4) ? hi[2] : lo[2]);
                    float sx, sy;
                    if (!project(px, py, pz, sx, sy)) continue;
                    any = true;
                    if (sx < x0) x0 = sx;
                    if (sy < y0) y0 = sy;
                    if (sx > x1) x1 = sx;
                    if (sy > y1) y1 = sy;
                }
                if (!any) continue;
                int bx0 = int(x0) / kStep, bx1 = int(x1) / kStep;
                int by0 = int(y0) / kStep, by1 = int(y1) / kStep;
                if (bx0 < 0) bx0 = 0;
                if (by0 < 0) by0 = 0;
                if (bx1 >= mask_w) bx1 = mask_w - 1;
                if (by1 >= mask_h) by1 = mask_h - 1;
                for (int by = by0; by <= by1; ++by)
                    for (int bx = bx0; bx <= bx1; ++bx)
                        own_mask[std::size_t(by) * std::size_t(mask_w) + std::size_t(bx)] = 1;
            }
        }
        for (int by = 0; by < int(view_h); by += kStep) {
            for (int bx = 0; bx < int(view_w); bx += kStep) {
                if (own_mask[std::size_t(by / kStep) * std::size_t(mask_w) +
                             std::size_t(bx / kStep)])
                    continue;
                sim::Fixed wx, wy;
                std::uint8_t a = 0;
                if (fctrl.ground_point(world_,
                                       ScreenPoint{float(bx) + 0.5f * kStep, float(by) + 0.5f * kStep}, wx,
                                       wy)) {
                    switch (vision_.state_at(wx, wy)) {
                        case sim::Visibility::Hidden: a = 200; break;
                        case sim::Visibility::Explored: a = 96; break;
                        case sim::Visibility::Visible: a = 0; break;
                    }
                }
                if (a == 0) continue;
                const int x1 = (bx + kStep < int(view_w)) ? bx + kStep : int(view_w);
                const int y1 = (by + kStep < int(view_h)) ? by + kStep : int(view_h);
                for (int y = by; y < y1; ++y) {
                    std::uint8_t* px = out.data() + (std::size_t(y) * view_w + bx) * 4;
                    for (int x = bx; x < x1; ++x) {
                        px[0] = 0;
                        px[1] = 0;
                        px[2] = 0;
                        px[3] = a;
                        px += 4;
                    }
                }
            }
        }
    }

    alpha_over(out, units);
    alpha_over(out, hud);

    if (dragging_)
        draw_box_rgba(out, view_w, view_h, int(drag_x0_), int(drag_y0_), int(mouse_x_),
                      int(mouse_y_), 80, 235, 110);

    {
        const render::Mat4 mvp = camctl_.view_proj(aspect);
        std::vector<int> assigned(world_.units().size(), 0);
        for (const Unit& u : world_.units())
            if (u.alive && u.player >= 0 && u.gather_target != sim::kNoTarget &&
                u.gather_target < assigned.size())
                assigned[u.gather_target]++;
        Control cctrl = make_control(mvp, view_w, view_h, viewed);
        auto proj = [&](const Unit& n, int& sx, int& sy) -> bool {
            const float x = float(n.x.to_double()), y = float(n.y.to_double());
            const formats::Model* nm =
                (n.id < driver_.actors.size()) ? driver_.actors[n.id].model : nullptr;
            const float top = nm ? nm->bounds_max[2] : float(n.radius.to_double());
            float px = 0.0f, py = 0.0f;
            if (!cctrl.project(x, y, cctrl.ground_z(x, y) + top, px, py)) return false;
            sx = int(px);
            sy = int(py);
            return true;
        };
        for (const MeleePlayerLayout& pl : layout_.players) {
            int total = 0, best_sx = 0, best_sy = 1 << 30, live_patches = 0;
            for (std::uint32_t nid : pl.minerals) {
                if (nid >= world_.units().size() || !world_.unit(nid).alive) continue;
                ++live_patches;
                total += assigned[nid];
                int sx, sy;
                if (proj(world_.unit(nid), sx, sy) && sy < best_sy) { best_sy = sy; best_sx = sx; }
            }
            if (live_patches > 0 && best_sy < (1 << 30)) {
                char b[16];
                std::snprintf(b, sizeof(b), "%d/%d", total, live_patches * 2);
                draw_text_rgba(out, view_w, view_h, best_sx - 10, best_sy - 30, b, 2, 240, 236, 150);
            }
            for (std::uint32_t gid : pl.geysers) {
                if (gid >= world_.units().size() || !world_.unit(gid).alive) continue;
                const Unit& g = world_.unit(gid);
                bool extractor = false;
                for (const Unit& s : world_.units())
                    if (s.alive && s.player >= 0 && s.speed.raw() == 0) {
                        const sim::Fixed dx = s.x - g.x, dy = s.y - g.y;
                        if ((dx * dx + dy * dy).to_double() < 1.0) { extractor = true; break; }
                    }
                if (!extractor) continue;
                int sx, sy;
                if (proj(g, sx, sy)) {
                    char b[12];
                    std::snprintf(b, sizeof(b), "%d/3", assigned[gid]);
                    draw_text_rgba(out, view_w, view_h, sx - 8, sy - 30, b, 2, 150, 240, 170);
                }
            }
        }
    }

    {
        const ConsolePanel* clip_console = &console_panel;
        auto putpx = [&](int x, int y, int r, int g, int b, int a) {
            if (x < 0 || y < 0 || x >= int(view_w) || y >= int(view_h)) return;
            if (hud_covers_point(view_w, view_h, clip_console, float(x), float(y))) return;
            std::size_t i = (std::size_t(y) * view_w + x) * 4;
            out[i] = std::uint8_t(r); out[i + 1] = std::uint8_t(g);
            out[i + 2] = std::uint8_t(b); out[i + 3] = std::uint8_t(a);
        };
        const float aspect2 = view_h > 0 ? float(view_w) / float(view_h) : 1.0f;
        const render::Mat4 mvp = camctl_.view_proj(aspect2);
        Control ovctrl = make_control(mvp, view_w, view_h, viewed);
        auto proj0 = [&](float x, float y, int& sx, int& sy) -> bool {
            float px = 0.0f, py = 0.0f;
            if (!ovctrl.project_on_ground(x, y, px, py)) return false;
            sx = int(px);
            sy = int(py);
            return true;
        };
        auto line = [&](int x0, int y0, int x1, int y1, int r, int g, int b, int a) {
            int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
            int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
            for (int guard = 0; guard < 4000; ++guard) {
                putpx(x0, y0, r, g, b, a);
                if (x0 == x1 && y0 == y1) break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
            }
        };
        auto marker = [&](int cx, int cy, int r, int g, int b) {
            for (int k = -5; k <= 5; ++k) {
                putpx(cx + k, cy, r, g, b, 235); putpx(cx, cy + k, r, g, b, 235);
                putpx(cx + k, cy + (5 - std::abs(k)), r, g, b, 200);
                putpx(cx + k, cy - (5 - std::abs(k)), r, g, b, 200);
            }
        };

        if (pending_build_ || pending_refinery_) {
            const float wx = float(build_px_.to_double()), wy = float(build_py_.to_double());
            const int r = build_valid_ ? 60 : 225, g = build_valid_ ? 215 : 60;
            std::vector<std::pair<int, int>> cells;
            if (cat_ && !build_proto_.catalog_id.empty())
                cells = cat_->unit_pathing_footprint_offsets(build_proto_.catalog_id);
            bool drew = false;
            if (!cells.empty()) {
                const float ox = std::floor(wx), oy = std::floor(wy);
                for (const std::pair<int, int>& c : cells) {
                    int q[4][2];
                    bool ok = true;
                    const float x0 = ox + float(c.first), y0 = oy + float(c.second);
                    const float corner[4][2] = {{x0, y0}, {x0 + 1, y0}, {x0 + 1, y0 + 1}, {x0, y0 + 1}};
                    for (int k = 0; k < 4 && ok; ++k)
                        ok = proj0(corner[k][0], corner[k][1], q[k][0], q[k][1]);
                    if (!ok) continue;
                    int miny = q[0][1], maxy = q[0][1];
                    for (int k = 1; k < 4; ++k) {
                        if (q[k][1] < miny) miny = q[k][1];
                        if (q[k][1] > maxy) maxy = q[k][1];
                    }
                    for (int y = miny; y <= maxy; ++y) {
                        int xs = 1 << 30, xe = -(1 << 30);
                        for (int k = 0; k < 4; ++k) {
                            const int k2 = (k + 1) % 4;
                            const int ay = q[k][1], by = q[k2][1];
                            if ((y < ay && y < by) || (y > ay && y > by) || ay == by) continue;
                            const float t = float(y - ay) / float(by - ay);
                            const int x = int(float(q[k][0]) + t * float(q[k2][0] - q[k][0]));
                            if (x < xs) xs = x;
                            if (x > xe) xe = x;
                        }
                        for (int x = xs; x <= xe; ++x) putpx(x, y, r, g, 60, 70);
                    }
                    for (int k = 0; k < 4; ++k)
                        line(q[k][0], q[k][1], q[(k + 1) % 4][0], q[(k + 1) % 4][1], r, g, 60, 255);
                    drew = true;
                }
            }
            if (!drew) {
                const float rad = float(build_proto_.radius.to_double());
                int cx, cy, ex, ey;
                if (proj0(wx, wy, cx, cy) && proj0(wx + (rad > 0 ? rad : 1.0f), wy, ex, ey)) {
                    int pr = std::abs(ex - cx);
                    pr = pr < 8 ? 8 : (pr > 90 ? 90 : pr);
                    for (int y = cy - pr; y <= cy + pr; ++y)
                        for (int x = cx - pr; x <= cx + pr; ++x) putpx(x, y, r, g, 60, 70);
                    for (int t = 0; t < 2; ++t) {
                        for (int x = cx - pr; x <= cx + pr; ++x) { putpx(x, cy - pr + t, r, g, 60, 255); putpx(x, cy + pr - t, r, g, 60, 255); }
                        for (int y = cy - pr; y <= cy + pr; ++y) { putpx(cx - pr + t, y, r, g, 60, 255); putpx(cx + pr - t, y, r, g, 60, 255); }
                    }
                }
            }
        }

        if (cfg_.human_player >= 0)
            for (std::uint32_t id : selection_) {
                if (id >= world_.units().size()) continue;
                const Unit& u = world_.unit(id);
                if (!u.alive || u.player != cfg_.human_player || u.orders.empty()) continue;
                const sim::Order& o = u.orders.front();
                if (o.type != sim::OrderType::Move && o.type != sim::OrderType::AttackMove) continue;
                int ux, uy, tx, ty;
                if (proj0(float(u.x.to_double()), float(u.y.to_double()), ux, uy) &&
                    proj0(float(o.tx.to_double()), float(o.ty.to_double()), tx, ty)) {
                    const bool atk = o.type == sim::OrderType::AttackMove;
                    const int r = atk ? 230 : 90, g = atk ? 90 : 210, b = atk ? 70 : 90;
                    line(ux, uy, tx, ty, r, g, b, 170);
                    marker(tx, ty, r, g, b);
                }
            }

        if (cfg_.human_player >= 0 && cat_)
            for (std::uint32_t id : selection_) {
                if (id >= world_.units().size()) continue;
                const Unit& u = world_.unit(id);
                if (!u.alive || u.player != cfg_.human_player || !u.has_rally) continue;
                int ux, uy, tx, ty;
                if (!proj0(float(u.x.to_double()), float(u.y.to_double()), ux, uy)) continue;
                if (!proj0(float(u.rally_x.to_double()), float(u.rally_y.to_double()), tx, ty))
                    continue;
                int rr = 200, rg = 200, rb = 200;
                const std::string ab = cat_->unit_rally_ability(u.catalog_id);
                if (!ab.empty())
                    if (auto col = cat_->ability_rally_color(ab)) {
                        rr = (*col)[0]; rg = (*col)[1]; rb = (*col)[2];
                    }
                line(ux, uy, tx, ty, rr, rg, rb, 170);
                marker(tx, ty, rr, rg, rb);
            }

    }

    auto blit_icon_tinted = [&](const IconAtlas::Icon& ic, int dx0, int dy0, int dx1, int dy1,
                                float tr, float tg, float tb) {
        if (!ic.ok() || dx1 <= dx0 || dy1 <= dy0) return;
        const int dw = dx1 - dx0, dh = dy1 - dy0;
        const float tint[3] = {tr, tg, tb};
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x) {
                const int sx = x * int(ic.w) / dw, sy = y * int(ic.h) / dh;
                const std::size_t si = (std::size_t(sy) * ic.w + sx) * 4;
                const float a = ic.rgba[si + 3] / 255.0f;
                if (a <= 0.0f) continue;
                const int px = dx0 + x, py = dy0 + y;
                if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
                const std::size_t di = (std::size_t(py) * view_w + px) * 4;
                for (int c = 0; c < 3; ++c) {
                    const float src = float(ic.rgba[si + c]) * tint[c];
                    out[di + c] = std::uint8_t(src * a + out[di + c] * (1 - a));
                }
                const float da = out[di + 3] / 255.0f;
                out[di + 3] = std::uint8_t((a + da * (1.0f - a)) * 255.0f + 0.5f);
            }
    };

    auto blit_icon = [&](const IconAtlas::Icon& ic, int dx0, int dy0, int dx1, int dy1) {
        if (!ic.ok() || dx1 <= dx0 || dy1 <= dy0) return;
        const int dw = dx1 - dx0, dh = dy1 - dy0;
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x) {
                const int sx = x * int(ic.w) / dw, sy = y * int(ic.h) / dh;
                const std::size_t si = (std::size_t(sy) * ic.w + sx) * 4;
                const float a = ic.rgba[si + 3] / 255.0f;
                if (a <= 0.0f) continue;
                const int px = dx0 + x, py = dy0 + y;
                if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
                const std::size_t di = (std::size_t(py) * view_w + px) * 4;
                for (int c = 0; c < 3; ++c)
                    out[di + c] = std::uint8_t(ic.rgba[si + c] * a + out[di + c] * (1 - a));
                const float da = out[di + 3] / 255.0f;
                out[di + 3] = std::uint8_t((a + da * (1.0f - a)) * 255.0f + 0.5f);
            }
    };

    if (cfg_.human_player >= 0 && selection_.size() == 1 &&
        selection_[0] < world_.units().size() && world_.unit(selection_[0]).alive &&
        world_.unit(selection_[0]).player == cfg_.human_player) {
        const Unit& su = world_.unit(selection_[0]);
        const formats::Model* wm_model =
            selection_[0] < driver_.actors.size() ? driver_.actors[selection_[0]].model : nullptr;
        const std::string tok =
            selection_[0] < layout_.unit_type.size() ? layout_.unit_type[selection_[0]] : "";
        const int race = race_index(cat_, su.catalog_id, tok);
        float hpf = su.hp_max.raw() > 0 ? float(su.hp.to_double() / su.hp_max.to_double()) : 1.0f;
        hpf = hpf < 0 ? 0 : (hpf > 1 ? 1 : hpf);
        auto hc = [&](float f, int& r, int& g, int& b) {
            if (f > 0.6f) { r = 70; g = 220; b = 90; }
            else if (f > 0.3f) { r = 230; g = 210; b = 70; }
            else { r = 225; g = 75; b = 60; }
        };
        const HudRect wp = hud_unit_panel_rect(view_w, view_h, &console_panel);
        const int PW = wp.w(), PH = wp.h();
        const int px0 = wp.x0, py0 = wp.y0;
        std::string wire_name = tok;
        if (cat_)
            if (auto nm = cat_->unit_display_name(su.catalog_id)) wire_name = std::move(*nm);
        if (!wire_name.empty())
            draw_text_rgba(out, view_w, view_h, px0 + 3, py0 + 2, wire_name.c_str(), 1, 200, 208, 220);
        if (unit_pane) {
            const double sc = double(view_h) / double(kRefHeight);
            const HudAuthoredRects::R& ip = console_panel.authored->info;
            HudUnitPane pane;
            pane.name = wire_name;
            pane.pane = {int(ip.x0 * sc + 0.5), int(ip.y0 * sc + 0.5), int(ip.x1 * sc + 0.5), int(ip.y1 * sc + 0.5)};
            const int name_y = int((ip.y0 + 10.0f) * sc + 0.5);
            const int name_w = int(pane.name.size()) * 12;
            const int name_x = std::max(pane.pane.x0, (pane.pane.x0 + pane.pane.x1) / 2 - name_w / 2);
            draw_text_rgba(out, view_w, view_h, name_x, name_y, pane.name.c_str(), 2, 230, 232, 236);
            const float bar_top = ip.y0 + 10.0f + 26.0f;
            pane.life_bar = {int(ip.x0 * sc + 0.5), int(bar_top * sc + 0.5), int((ip.x0 + 128.0f) * sc + 0.5), int((bar_top + 16.0f) * sc + 0.5)};
            pane.life_frac = hpf;
            {
                int r = 0, g = 0, b = 0;
                hc(hpf, r, g, b);
                const int fill_x1 = pane.life_bar.x0 + int((pane.life_bar.x1 - pane.life_bar.x0) * hpf + 0.5f);
                for (int y = pane.life_bar.y0; y < pane.life_bar.y1; ++y)
                    for (int x = pane.life_bar.x0; x < pane.life_bar.x1; ++x) {
                        if (x < 0 || y < 0 || x >= int(view_w) || y >= int(view_h)) continue;
                        const std::size_t di = (std::size_t(y) * view_w + std::size_t(x)) * 4;
                        const bool edge = y == pane.life_bar.y0 || y == pane.life_bar.y1 - 1 || x == pane.life_bar.x0 || x == pane.life_bar.x1 - 1;
                        const bool filled = x < fill_x1;
                        const int cr = edge ? 30 : filled ? r : 18, cg = edge ? 34 : filled ? g : 20, cb = edge ? 40 : filled ? b : 26;
                        out[di] = std::uint8_t(cr); out[di + 1] = std::uint8_t(cg); out[di + 2] = std::uint8_t(cb); out[di + 3] = 255;
                    }
            }
            char info[96];
            std::snprintf(info, sizeof info, "%d / %d", int(su.hp.to_double() + 0.5), int(su.hp_max.to_double() + 0.5));
            pane.info = info;
            if (su.shields_max.raw() > 0) {
                std::snprintf(info, sizeof info, "  + %d / %d shields", int(su.shields.to_double() + 0.5), int(su.shields_max.to_double() + 0.5));
                pane.info += info;
            }
            if (su.energy_max.raw() > 0) {
                std::snprintf(info, sizeof info, "  + %d / %d energy", int(su.energy.to_double() + 0.5), int(su.energy_max.to_double() + 0.5));
                pane.info += info;
            }
            draw_text_rgba(out, view_w, view_h, pane.life_bar.x1 + 8, pane.life_bar.y0, pane.info.c_str(), 1, 210, 214, 220);
            static const std::pair<std::uint32_t, const char*> kAttr[] = {
                {sim::kAttrLight, "Light"}, {sim::kAttrArmored, "Armored"}, {sim::kAttrBiological, "Biological"},
                {sim::kAttrMechanical, "Mechanical"}, {sim::kAttrMassive, "Massive"}, {sim::kAttrPsionic, "Psionic"},
                {sim::kAttrStructure, "Structure"}};
            for (const auto& [bit, label] : kAttr)
                if (su.attributes & bit) {
                    if (!pane.type.empty()) pane.type += " - ";
                    pane.type += label;
                }
            if (!pane.type.empty())
                draw_text_rgba(out, view_w, view_h, pane.pane.x0, int((ip.y0 + 176.0f) * sc + 0.5), pane.type.c_str(), 1, 190, 196, 208);
            hud_unit_pane_ = std::move(pane);
        }
        if (unit_pane && models_ && ui_store_ && console_panel.authored->portrait.ok()) {
            const double sc = double(view_h) / double(kRefHeight);
            const HudAuthoredRects::R& pr = console_panel.authored->portrait;
            if (const MeleeModels::Portrait* p = models_->portrait(*ui_store_, su.catalog_id)) {
                HudPortrait req;
                req.unit = su.catalog_id;
                req.path = p->path;
                req.fov_deg = p->fov_rad * 180.0f / 3.14159265f;
                req.rect = {int(pr.x0 * sc + 0.5), int(pr.y0 * sc + 0.5), int(pr.x1 * sc + 0.5), int(pr.y1 * sc + 0.5)};
                const std::uint32_t pw = std::uint32_t(std::max(1, req.rect.w())), ph = std::uint32_t(std::max(1, req.rect.h()));
                if (portrait_renderer_) {
                    std::vector<std::uint8_t> img;
                    try { img = portrait_renderer_(*ui_store_, p->model, p->fov_rad, p->cam_bone, p->znear, p->zfar, pw, ph); } catch (const std::exception&) {}
                    if (img.size() == std::size_t(pw) * ph * 4) {
                        for (std::uint32_t y = 0; y < ph; ++y)
                            for (std::uint32_t x = 0; x < pw; ++x) {
                                const int dx = req.rect.x0 + int(x), dy = req.rect.y0 + int(y);
                                if (dx < 0 || dy < 0 || dx >= int(view_w) || dy >= int(view_h)) continue;
                                const std::size_t si = (std::size_t(y) * pw + x) * 4, di = (std::size_t(dy) * view_w + std::size_t(dx)) * 4;
                                const unsigned a = img[si + 3];
                                if (!a) continue;
                                for (int k = 0; k < 3; ++k) out[di + k] = std::uint8_t((img[si + k] * a + out[di + k] * (255 - a)) / 255);
                                out[di + 3] = std::uint8_t(a + (out[di + 3] * (255 - a)) / 255);
                            }
                        req.drawn = true;
                    }
                }
                hud_portrait_ = std::move(req);
            }
        }

        bool drew_authored = false;
        if (ui_store_ && !su.catalog_id.empty()) {
            std::string wf_name;
            if (cat_)
                if (auto authored = cat_->unit_wireframe_image(su.catalog_id)) wf_name = *authored;
            if (wf_name.empty()) {
                static const char* const kRaceTok[3] = {"terran", "protoss", "zerg"};
                std::string unit_tok = su.catalog_id;
                for (char& c : unit_tok) c = char(std::tolower((unsigned char)c));
                wf_name = std::string("wireframe-") +
                          kRaceTok[race < 0 || race > 2 ? 0 : race] + "-" + unit_tok;
            }
            const IconAtlas::Icon& wf = icons_.get(*ui_store_, wf_name);
            if (wf.ok()) {
                const HudRect wr = hud_unit_wireframe_rect(view_w, view_h, &console_panel);
                const HudRect dst = wr.w() > 0 ? wr
                                               : HudRect{px0 + 4, py0 + 12, px0 + PW - 4, py0 + PH - 4};
                const bool greyscale_mask = race != 2;
                blit_icon_tinted(wf, dst.x0, dst.y0, dst.x1, dst.y1,
                                 greyscale_mask ? 0.0f : 1.0f, 1.0f,
                                 greyscale_mask ? 0.0f : 1.0f);
                drew_authored = true;
            }
        }
        if (wm_model && !drew_authored) {
            const int MW = PW - 8, MH = PH - 16;
            const WireMask& msk = wire_mask(wm_model, MW, MH);
            const int mx0 = px0 + 4, my0 = py0 + 12;
            int r, g, b; hc(hpf, r, g, b);
            const bool shielded = su.shields.raw() > 0;
            for (int y = 0; y < msk.h; ++y)
                for (int x = 0; x < msk.w; ++x) {
                    if (!msk.cov[std::size_t(y) * msk.w + x]) continue;
                    int cr = r, cg = g, cb = b;
                    if (race == 2) {
                        const float f = hpf * (0.65f + 0.35f * (1.0f - float(y) / std::max(1, msk.h)));
                        hc(f, cr, cg, cb);
                    }
                    const int px = mx0 + x, py = my0 + y;
                    if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
                    std::size_t di = (std::size_t(py) * view_w + px) * 4;
                    out[di] = std::uint8_t(cr); out[di + 1] = std::uint8_t(cg);
                    out[di + 2] = std::uint8_t(cb); out[di + 3] = 255;
                }
            if (race == 1 && shielded) {
                for (int y = 0; y < msk.h; ++y)
                    for (int x = 0; x < msk.w; ++x) {
                        if (!msk.cov[std::size_t(y) * msk.w + x]) continue;
                        const bool edge =
                            x == 0 || y == 0 || x == msk.w - 1 || y == msk.h - 1 ||
                            !msk.cov[std::size_t(y) * msk.w + (x - 1)] ||
                            !msk.cov[std::size_t(y) * msk.w + (x + 1)] ||
                            !msk.cov[std::size_t(y - 1) * msk.w + x] ||
                            !msk.cov[std::size_t(y + 1) * msk.w + x];
                        if (!edge) continue;
                        const int px = mx0 + x, py = my0 + y;
                        if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
                        std::size_t di = (std::size_t(py) * view_w + px) * 4;
                        out[di] = 90; out[di + 1] = 160; out[di + 2] = 255; out[di + 3] = 255;
                    }
            }
        }
    }

    if (cfg_.human_player >= 0 && selection_.size() == 1 &&
        selection_[0] < world_.units().size() && world_.unit(selection_[0]).alive &&
        world_.unit(selection_[0]).player == cfg_.human_player) {
        const HudRect pb = hud_progress_bar_rect(view_w, view_h, &console_panel);
        if (pb.w() > 0 && pb.h() > 0) {
            const std::uint32_t sel = selection_[0];
            float frac = -1.0f;
            sim::Fixed f;
            std::string rid;
            if (world_.unit(sel).constructing) frac = float(world_.build_progress(sel).to_double());
            else if (world_.pending_progress(sel, f)) frac = float(f.to_double());
            else if (world_.research_progress(cfg_.human_player, f, rid)) frac = float(f.to_double());
            if (frac >= 0.0f) {
                frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
                static const int kBorder[3][3] = {{40, 125, 75}, {35, 125, 255}, {230, 95, 5}};
                static const int kFill[3][3] = {{155, 255, 190}, {110, 170, 255}, {245, 140, 70}};
                const int ri = race_index(cat_, world_.unit(sel).catalog_id,
                                          sel < layout_.unit_type.size() ? layout_.unit_type[sel]
                                                                         : std::string());
                const int rc = ri < 0 || ri > 2 ? 0 : ri;
                for (int y = pb.y0; y < pb.y1 && y < int(view_h); ++y)
                    for (int x = pb.x0; x < pb.x1 && x < int(view_w); ++x) {
                        if (x < 0 || y < 0) continue;
                        const std::size_t i = (std::size_t(y) * view_w + x) * 4;
                        out[i] = std::uint8_t(kBorder[rc][0]);
                        out[i + 1] = std::uint8_t(kBorder[rc][1]);
                        out[i + 2] = std::uint8_t(kBorder[rc][2]);
                        out[i + 3] = 255;
                    }
                const float kFillInset = 0.03125f;
                const int ix = int(float(pb.w()) * kFillInset + 0.5f);
                const int iy = int(float(pb.h()) * kFillInset + 0.5f);
                const int fx0 = pb.x0 + ix, fy0 = pb.y0 + iy, fy1 = pb.y1 - iy;
                const int fw = int(float(pb.w() - 2 * ix) * frac + 0.5f);
                for (int y = fy0; y < fy1 && y < int(view_h); ++y)
                    for (int x = fx0; x < fx0 + fw && x < int(view_w); ++x) {
                        if (x < 0 || y < 0) continue;
                        const std::size_t i = (std::size_t(y) * view_w + x) * 4;
                        out[i] = std::uint8_t(kFill[rc][0]);
                        out[i + 1] = std::uint8_t(kFill[rc][1]);
                        out[i + 2] = std::uint8_t(kFill[rc][2]);
                        out[i + 3] = 255;
                    }
            }
        }
    }

    if (cfg_.human_player >= 0 && ui_store_ && cat_ && console_panel.authored &&
        console_panel.authored->resource_icons_ok) {
        const int rr = human_race();
        const char* suffix = rr == 1 ? "_Prot" : rr == 2 ? "_Zerg" : "_Terr";
        const char* kTokens[3] = {"@@UI/ResourceIcon0", "@@UI/ResourceIcon1",
                                  "@@UI/ResourceFrameSupplyIcon"};
        const HudAuthoredRects::R* kRects[3] = {&console_panel.authored->resource_icon[0],
                                                &console_panel.authored->resource_icon[1],
                                                &console_panel.authored->supply_icon};
        const double sc = double(view_h) / double(kRefHeight);
        for (int i = 0; i < 3; ++i) {
            const HudAuthoredRects::R& q = *kRects[i];
            if (!q.ok()) continue;
            const std::string path = cat_->resolve_asset(std::string(kTokens[i]) + suffix);
            if (path.empty() || path[0] == '@') continue;
            blit_icon(icons_.get(*ui_store_, path.c_str()), int(q.x0 * sc + 0.5),
                      int(q.y0 * sc + 0.5), int(q.x1 * sc + 0.5), int(q.y1 * sc + 0.5));
        }
    }

    hud_group_buttons_.clear();
    if (cfg_.human_player >= 0 && ui_store_ && cat_ && console_panel.authored &&
        console_panel.authored->control_group.ok()) {
        const double sc = double(view_h) / double(kRefHeight);
        const HudAuthoredRects::R& panel = console_panel.authored->control_group;
        const std::string normal = cat_->resolve_asset("@@UI/Game_Console_Buttons_ControlGroup_Normal");
        const bool have_normal = !normal.empty() && normal[0] != '@';
        const float bw = 82.0f, bh = 64.0f, step = bw - 6.0f;
        float bx = panel.x0;
        const float by = panel.y1 - bh;
        const int order[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0};
        for (int slot : order) {
            const std::vector<std::uint32_t> ids = groups_.recall(slot);
            if (ids.empty()) continue;
            if (bx + bw > panel.x1 + 0.5f) break;
            HudGroupButton b;
            b.slot = slot;
            b.count = int(ids.size());
            b.rect = {int(bx * sc + 0.5), int(by * sc + 0.5), int((bx + bw) * sc + 0.5), int((by + bh) * sc + 0.5)};
            const std::uint32_t lead = ids.front();
            if (lead < world_.units().size()) {
                b.unit = world_.units()[lead].catalog_id;
                if (auto ic = cat_->button_icon(b.unit)) b.icon = *ic;
            }
            if (have_normal) blit_icon(icons_.get(*ui_store_, normal.c_str()), b.rect.x0, b.rect.y0, b.rect.x1, b.rect.y1);
            if (!b.icon.empty())
                blit_icon(icons_.get(*ui_store_, b.icon.c_str()), int((bx + 9.0f) * sc + 0.5), int((by + 9.0f) * sc + 0.5),
                          int((bx + 37.0f) * sc + 0.5), int((by + 37.0f) * sc + 0.5));
            char count[16];
            std::snprintf(count, sizeof count, "%d", b.count);
            draw_text_rgba(out, view_w, view_h, int((bx + 44.0f) * sc + 0.5), int((by + 10.0f) * sc + 0.5), count, 2, 230, 232, 236);
            char index[4];
            std::snprintf(index, sizeof index, "%d", slot);
            draw_text_rgba(out, view_w, view_h, int((bx + bw * 0.5f - 4.0f) * sc + 0.5), int((by + bh - 6.0f - 12.0f) * sc + 0.5), index, 1, 200, 208, 220);
            hud_group_buttons_.push_back(std::move(b));
            bx += step;
        }
    }

    hud_alert_items_.clear();
    if (authored_alert_panel) {
        const double sc = double(view_h) / double(kRefHeight);
        const HudAuthoredRects::R& panel = hud_authored_->alert;
        const float item_h = 52.0f;
        const int fit = int((panel.y1 - panel.y0) / item_h);
        const std::vector<HudMessage>& all = messages_.all();
        int drawn = 0;
        for (auto it = all.rbegin(); it != all.rend() && drawn < fit; ++it) {
            if (it->zone != MsgZone::Alert) continue;
            const float iy = panel.y0 + float(drawn) * item_h;
            HudAlertItem item;
            item.text = it->text;
            if (it->count > 1) item.text += " x" + std::to_string(it->count);
            item.icon = it->icon;
            item.ttl = it->ttl;
            item.rect = {int(panel.x0 * sc + 0.5), int(iy * sc + 0.5), int(panel.x1 * sc + 0.5), int((iy + item_h) * sc + 0.5)};
            const float ix = panel.x0 + 10.0f, icy = iy + item_h * 0.5f;
            if (!item.icon.empty())
                blit_icon(icons_.get(*ui_store_, item.icon.c_str()), int(ix * sc + 0.5), int((icy - 24.0f) * sc + 0.5),
                          int((ix + 48.0f) * sc + 0.5), int((icy + 24.0f) * sc + 0.5));
            draw_text_rgba(out, view_w, view_h, int((ix + 48.0f + 10.0f) * sc + 0.5), int((icy - 6.0f) * sc + 0.5), item.text.c_str(), 1, 230, 232, 236);
            hud_alert_items_.push_back(std::move(item));
            ++drawn;
        }
    }

    hud_page_buttons_.clear();
    if (group_pages > 1 && ui_store_ && cat_ && console_panel.authored && console_panel.authored->page_buttons_ok) {
        const double sc = double(view_h) / double(kRefHeight);
        const std::string normal = cat_->resolve_asset("@@UI/GroupPageButtonNormal");
        const std::string hover = cat_->resolve_asset("@@UI/GroupPageButtonHover");
        for (int p = 0; p < group_pages; ++p) {
            const HudAuthoredRects::R& q = console_panel.authored->page_buttons[p];
            HudPageButton pb;
            pb.page = p;
            pb.rect = {int(q.x0 * sc + 0.5), int(q.y0 * sc + 0.5), int(q.x1 * sc + 0.5), int(q.y1 * sc + 0.5)};
            const std::string& img = p == group_page_ ? hover : normal;
            if (!img.empty() && img[0] != '@') blit_icon(icons_.get(*ui_store_, img.c_str()), pb.rect.x0, pb.rect.y0, pb.rect.x1, pb.rect.y1);
            char label[4];
            std::snprintf(label, sizeof label, "%d", p + 1);
            draw_text_rgba(out, view_w, view_h, (pb.rect.x0 + pb.rect.x1) / 2 - 3, (pb.rect.y0 + pb.rect.y1) / 2 - 6, label, 1, 220, 224, 232);
            hud_page_buttons_.push_back(pb);
        }
    }
    if (!group_pane.empty() && ui_store_) {
        for (std::size_t i = 0; i < group_pane.size(); ++i) {
            const HudRect s = hud_group_slot(view_w, view_h, int(i), &console_panel);
            if (s.w() <= 0) continue;
            const std::uint32_t id = group_pane[i];
            const Unit& gu = world_.unit(id);
            for (int y = s.y0; y < s.y1 && y < int(view_h); ++y)
                for (int x = s.x0; x < s.x1 && x < int(view_w); ++x) {
                    if (x < 0 || y < 0) continue;
                    const std::size_t di = (std::size_t(y) * view_w + x) * 4;
                    out[di] = std::uint8_t(22 * 0.55f + out[di] * 0.45f);
                    out[di + 1] = std::uint8_t(28 * 0.55f + out[di + 1] * 0.45f);
                    out[di + 2] = std::uint8_t(40 * 0.55f + out[di + 2] * 0.45f);
                    out[di + 3] = 255;
                }
            const std::string& tok =
                id < layout_.unit_type.size() ? layout_.unit_type[id] : std::string();
            if (cat_ && !tok.empty())
                if (auto ic = cat_->button_icon(tok))
                    if (!ic->empty())
                        blit_icon(icons_.get(*ui_store_, ic->c_str()), s.x0 + 1, s.y0 + 1, s.x1 - 1,
                                  s.y1 - 1);
            const int bx0 = s.x0 + 3, bx1 = s.x1 - 3, bh = 9;
            const int by1 = s.y1 - 3, by0 = by1 - bh;
            float lf = gu.hp_max.raw() > 0 ? float(gu.hp.to_double() / gu.hp_max.to_double()) : 1.0f;
            lf = lf < 0 ? 0 : (lf > 1 ? 1 : lf);
            const int fillw = int(float(bx1 - bx0) * lf + 0.5f);
            std::uint8_t lr = 70, lg = 220, lb = 90;
            if (lf <= 0.3f) { lr = 225; lg = 75; lb = 60; }
            else if (lf <= 0.6f) { lr = 230; lg = 210; lb = 70; }
            for (int y = by0; y < by1 && y < int(view_h); ++y)
                for (int x = bx0; x < bx1 && x < int(view_w); ++x) {
                    if (x < 0 || y < 0) continue;
                    const std::size_t di = (std::size_t(y) * view_w + x) * 4;
                    const bool on = x < bx0 + fillw;
                    out[di] = on ? lr : 12;
                    out[di + 1] = on ? lg : 14;
                    out[di + 2] = on ? lb : 18;
                    out[di + 3] = 255;
                }
            const std::size_t a = sel_groups_.active < sel_groups_.subgroups.size()
                                      ? sel_groups_.active
                                      : 0;
            const std::size_t n_active =
                sel_groups_.subgroups.empty() ? group_pane.size()
                                              : sel_groups_.subgroups[a].units.size();
            const bool active = i < n_active;
            draw_box_rgba(out, view_w, view_h, s.x0, s.y0, s.x1, s.y1,
                          active ? 150 : 60, active ? 175 : 70, active ? 215 : 88);
        }
    }

    if (cfg_.human_player >= 0 && ui_store_) {
        std::vector<BuildCardEntry> ov_builds;
        std::vector<AbilityCardEntry> ov_abils;
        std::vector<TrainCardEntry> ov_trains;
        std::vector<SubmenuCardEntry> ov_submenus;
        CommandCard card = compose_command_card(ov_builds, ov_abils, ov_trains, ov_submenus);

        for (int i = 0; i < CommandCard::kSlots; ++i) {
            const HudRect c = hud_command_cell(view_w, view_h, i, &console_panel);
            if (c.w() <= 0) continue;
            const CommandButton& btn = card.slots[i];
            std::uint8_t br = 20, bg = 24, bb = 32;
            if (btn.state == ButtonState::Disabled) { br = 40; bg = 22; bb = 22; }
            else if (btn.state != ButtonState::Empty) { br = 26, bg = 34, bb = 48; }
            for (int y = c.y0; y < c.y1 && y < int(view_h); ++y)
                for (int x = c.x0; x < c.x1 && x < int(view_w); ++x) {
                    if (x < 0 || y < 0) continue;
                    const std::size_t di = (std::size_t(y) * view_w + x) * 4;
                    out[di] = std::uint8_t(br * 0.55f + out[di] * 0.45f);
                    out[di + 1] = std::uint8_t(bg * 0.55f + out[di + 1] * 0.45f);
                    out[di + 2] = std::uint8_t(bb * 0.55f + out[di + 2] * 0.45f);
                    out[di + 3] = 255;
                }
            if (btn.state != ButtonState::Empty && !btn.icon.empty()) {
                blit_icon(icons_.get(*ui_store_, btn.icon.c_str()), c.x0 + 1, c.y0 + 1, c.x1 - 1,
                          c.y1 - 1);
                if (btn.state == ButtonState::Disabled)
                    for (int y = c.y0; y < c.y1 && y < int(view_h); ++y)
                        for (int x = c.x0; x < c.x1 && x < int(view_w); ++x) {
                            if (x < 0 || y < 0) continue;
                            const std::size_t di = (std::size_t(y) * view_w + x) * 4;
                            out[di] = std::uint8_t(out[di] * 0.45f);
                            out[di + 1] = std::uint8_t(out[di + 1] * 0.45f);
                            out[di + 2] = std::uint8_t(out[di + 2] * 0.45f);
                        }
                const std::uint8_t lr = btn.is_cancel ? 200 : 130;
                const std::uint8_t lg = btn.is_cancel ? 90 : 160;
                const std::uint8_t lb = btn.is_cancel ? 90 : 200;
                draw_box_rgba(out, view_w, view_h, c.x0, c.y0, c.x1, c.y1, lr, lg, lb);
            } else {
                draw_box_rgba(out, view_w, view_h, c.x0, c.y0, c.x1, c.y1, 60, 70, 88);
            }
        }

        if (!pause_menu_open_) {
            if (auto tip = hovered_card_tooltip(view_w, view_h, mouse_x_, mouse_y_, &console_panel)) {
                const std::string body = build_tooltip(*tip);
                std::vector<std::string> lines;
                std::string cur;
                for (char ch : body) {
                    if (ch == '\n') { lines.push_back(cur); cur.clear(); }
                    else cur += ch;
                }
                lines.push_back(cur);
                const int sc = 1;
                const int pad = 5, lh = ui::kFontH * sc + 3;
                int tw = 0;
                for (const std::string& ln : lines) tw = std::max(tw, draw::text_w(ln, sc));
                const int pw = tw + pad * 2;
                const int ph = int(lines.size()) * lh - 3 + pad * 2;
                int x0 = int(mouse_x_) + 14, y0 = int(mouse_y_) - ph - 8;
                if (x0 + pw > int(view_w)) x0 = int(view_w) - pw;
                if (y0 < 0) y0 = int(mouse_y_) + 18;
                if (y0 + ph > int(view_h)) y0 = int(view_h) - ph;
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                draw::fill(out, view_w, view_h, x0, y0, x0 + pw, y0 + ph, 12, 16, 26, 235);
                draw::box(out, view_w, view_h, x0, y0, x0 + pw, y0 + ph, 90, 120, 170);
                for (std::size_t li = 0; li < lines.size(); ++li) {
                    std::uint8_t r = 205, g = 218, b = 236;
                    if (li == 0) { r = 235; g = 242; b = 255; }
                    else if (lines[li].rfind("Requires:", 0) == 0) { r = 236; g = 120; b = 110; }
                    draw::text(out, view_w, view_h, x0 + pad, y0 + pad + int(li) * lh, lines[li], sc, r, g, b);
                }
            }
        }
    }

    char fps[24];
    std::snprintf(fps, sizeof(fps), "FPS %d", int(fps_smooth_ + 0.5f));
    draw_text_rgba(out, view_w, view_h, 6, 6, fps, 2, 80, 255, 120);

    if (replay_drive_) {
        char rb[40];
        std::snprintf(rb, sizeof(rb), "REPLAY  %d/%d", world_.tick(), replay_total_);
        const int tw = int(std::string(rb).size()) * (ui::kFontW + 1) * 2;
        draw_text_rgba(out, view_w, view_h, (int(view_w) - tw) / 2, 6, rb, 2, 255, 210, 90);
    }

    if (pause_menu_open_) {
        draw::fill_a(out, view_w, view_h, 0, 0, int(view_w), int(view_h), 0, 0, 0, theme::kAlphaScrim);
        PauseRect resume, quit;
        pause_menu_rects(view_w, view_h, resume, quit);
        int px0 = resume.x0 - 30, py0 = resume.y0 - 90, px1 = resume.x1 + 30, py1 = quit.y1 + 30;
        if (ui_store_ && view_w && view_h)
            if (const HudPauseRects* pr = hud_pause_rects(*ui_store_, float(view_w) / float(view_h))) {
                const double sc = double(view_h) / double(kRefHeight);
                px0 = int(pr->panel.x0 * sc + 0.5); py0 = int(pr->panel.y0 * sc + 0.5);
                px1 = int(pr->panel.x1 * sc + 0.5); py1 = int(pr->panel.y1 * sc + 0.5);
            }
        draw::vgrad(out, view_w, view_h, px0, py0, px1, py1, theme::kSurfaceRaised.r, theme::kSurfaceRaised.g, theme::kSurfaceRaised.b,
                    theme::kSurface.r, theme::kSurface.g, theme::kSurface.b, 255, 255);
        draw::box_a(out, view_w, view_h, px0, py0, px1, py1, theme::kBorderStrong.r, theme::kBorderStrong.g, theme::kBorderStrong.b, 255);
        const std::string title = "GAME MENU";
        draw::text(out, view_w, view_h, (px0 + px1 - draw::text_w(title, 3)) / 2, py0 + std::max(12, (resume.y0 - py0 - ui::kFontH * 3) / 2),
                   title, 3, theme::kTextHi.r, theme::kTextHi.g, theme::kTextHi.b);
        const PauseRects all = pause_rects_all(view_w, view_h);
        const PauseRect* rects[4] = {&all.load, &all.save, &resume, &quit};
        const char* labels[4] = {"LOAD GAME", "SAVE GAME", "RESUME GAME", "QUIT GAME"};
        for (int i = 0; i < 4; ++i) {
            if ((i == 0 && !all.has_load) || (i == 1 && !all.has_save)) continue;
            const PauseRect& r = *rects[i];
            const bool hot = mouse_x_ >= float(r.x0) && mouse_x_ < float(r.x1) &&
                             mouse_y_ >= float(r.y0) && mouse_y_ < float(r.y1);
            const bool is_quit = i == 3;
            draw::vgrad(out, view_w, view_h, r.x0, r.y0, r.x1, r.y1, theme::kMenuBtnTop.r, theme::kMenuBtnTop.g, theme::kMenuBtnTop.b,
                        theme::kMenuBtnBottom.r, theme::kMenuBtnBottom.g, theme::kMenuBtnBottom.b, 255, 255);
            const theme::Rgb edge = hot ? theme::kAccent : is_quit ? theme::kBorderQuit : theme::kBorderCtl;
            draw::box_a(out, view_w, view_h, r.x0, r.y0, r.x1, r.y1, edge.r, edge.g, edge.b, 255);
            const theme::Rgb ink = hot ? theme::kTextHi : theme::kTextBody;
            draw::text(out, view_w, view_h, r.x0 + (r.x1 - r.x0 - draw::text_w(labels[i], 2)) / 2,
                       r.y0 + (r.y1 - r.y0 - ui::kFontH * 2) / 2, labels[i], 2, ink.r, ink.g, ink.b);
        }
    }
}

}
