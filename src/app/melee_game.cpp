#include "omc/core/env_once.hpp"
#include "omc/render/frame_prof.hpp"
#include "omc/app/melee_game.hpp"
#include "omc/app/ui_hotkeys.hpp"
#include "omc/data/environment.hpp"

#include "omc/formats/placed_objects.hpp"
#include "omc/formats/dds.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

#include "omc/app/caster_composition.hpp"
#include "omc/app/splat_place.hpp"
#include "omc/app/command_bind.hpp"
#include "omc/app/control.hpp"
#include "omc/app/doodad_place.hpp"
#include "omc/app/hud.hpp"
#include "omc/app/hud_render.hpp"
#include "omc/app/map_doodads.hpp"
#include "omc/app/melee_commands.hpp"
#include "omc/app/order_cues.hpp"
#include "omc/app/overlay_draw.hpp"
#include "omc/app/ui_theme.hpp"
#include "omc/casc/storage.hpp"
#include "omc/app/ability.hpp"
#include "omc/app/morph_plan.hpp"
#include "omc/app/spawn.hpp"
#include "omc/app/tooltip.hpp"
#include "omc/app/kill_rank.hpp"
#include "omc/app/shield_flare.hpp"
#include "omc/data/abil_charge.hpp"
#include "omc/data/economy.hpp"
#include "omc/audio/mixer.hpp"
#include "omc/audio/vorbis.hpp"
#include "omc/audio/wasapi.hpp"
#include "omc/data/actor_model.hpp"
#include "omc/data/actor_catalog.hpp"
#include "omc/render/anim.hpp"
#include "omc/data/audio.hpp"
#include "omc/data/catalog.hpp"
#include "omc/formats/m3.hpp"
#include "omc/render/mat4.hpp"
#include "omc/sim/weapon.hpp"
#include "omc/sim/build.hpp"
#include "omc/render/creep_layer.hpp"
#include "omc/app/water_material.hpp"
#include "omc/data/upgrades.hpp"
#include "omc/sim/transport.hpp"
#include "omc/render/prim.hpp"
#include "omc/render/quality.hpp"
#include "omc/ui/font.hpp"

namespace omc::app {

static int fog_ground_state(int state, bool type_hidden) {
    static const bool black = std::getenv("OMC_FOG_BLACK_MASK") != nullptr;
    return !black && !type_hidden && state == int(sim::Visibility::Hidden) ? int(sim::Visibility::Explored) : state;
}
static int fog_body_state(int player, int state, bool type_hidden) {
    static const bool explored = std::getenv("OMC_FOG_EXPLORED") != nullptr;
    if (player < 0) return fog_ground_state(state, type_hidden);
    return explored && state == int(sim::Visibility::Hidden) ? int(sim::Visibility::Explored) : state;
}

static float selection_ring_radius(const data::Catalog* cat, const std::string& id, float body_radius) {
    static const bool kBodyRadius = std::getenv("OMC_SELECTION_BODY_RADIUS") != nullptr;
    if (!cat || kBodyRadius) return body_radius;
    static std::unordered_map<std::string, float> authored;
    auto it = authored.find(id);
    if (it == authored.end()) it = authored.emplace(id, float(cat->scalar_num("CUnit", id, "Radius").value_or(0.0))).first;
    return (it->second > 1e-3f ? it->second : body_radius) + 0.03f;
}

static std::array<float, 3> unit_draw_scale(const data::Catalog* cat, const std::string& id) {
    if (!cat) return {1.0f, 1.0f, 1.0f};
    static const bool off = std::getenv("OMC_NO_MODEL_SCALE") != nullptr;
    static const data::Catalog* cached_for = nullptr;
    static std::unordered_map<std::string, std::array<float, 3>> cache;
    if (cached_for != cat) { cache.clear(); cached_for = cat; }
    const auto it = cache.find(id);
    if (it != cache.end()) return it->second;
    std::array<float, 3> s = cat->unit_render_scale(data::unit_actor_id(*cat, id));
    if (!off) {
        const std::array<float, 3> m = data::resolve_unit_model_scale(*cat, id);
        for (std::size_t i = 0; i < 3; ++i) s[i] *= m[i];
    }
    cache.emplace(id, s);
    return s;
}

using sim::Unit;

namespace {

bool same_placement(const formats::PlacedDoodad& a, const formats::PlacedDoodad& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.rotation == b.rotation && a.pitch == b.pitch && a.roll == b.roll &&
           a.sx == b.sx && a.sy == b.sy && a.sz == b.sz && a.variation == b.variation && a.type == b.type;
}

std::uint64_t doodad_seed(const formats::PlacedDoodad& d) {
    std::uint64_t dseed = 1469598103934665603ull;
    for (char ch : d.type) dseed = (dseed ^ std::uint64_t(std::uint8_t(ch))) * 1099511628211ull;
    for (float f : {d.x, d.y}) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        dseed = (dseed ^ std::uint64_t(bits)) * 1099511628211ull;
    }
    return dseed;
}

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
static const float kCloakOpacity = 0.45f;
static const float kCloakDisplaceStrength = 5.0f;
static const float kCloakUvPerStrength = 0.002f;
static const int kPissedClicks = 4;
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
    v.pan = pending_pan_;
    pending_pan_ = 0.0f;
    const int id = mixer_->add_voice(v);
    recent_ids_.push_back(id);
    last_voice_id_ = id;
    return 1;
}

int MeleeAudio::play_clip_limited(const std::string& sound_id, int dupe_destroy, int dupe_mute, const std::string& key,
                                  const PcmClip& clip, float gain, double rate_factor, bool dupe_prevent, int priority) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<LiveVoice> live;
        live.reserve(voice_sounds_.size());
        for (const LiveVoice& vs : voice_sounds_)
            if (mixer_ && mixer_->is_active(vs.id)) live.push_back(vs);
        voice_sounds_.swap(live);
        int same = 0;
        for (const LiveVoice& vs : voice_sounds_) same += vs.sound == sound_id;
        if (dupe_mute > 0 && same >= dupe_mute) return 0;
        if (dupe_destroy > 0 && same >= dupe_destroy) {
            if (dupe_prevent || !mixer_) return 0;
            for (auto it = voice_sounds_.begin(); it != voice_sounds_.end(); ++it)
                if (it->sound == sound_id) {
                    mixer_->stop(it->id);
                    voice_sounds_.erase(it);
                    break;
                }
        }
        if (mixer_ && int(voice_sounds_.size()) >= max_active_voices_) {
            auto lowest = voice_sounds_.end();
            for (auto it = voice_sounds_.begin(); it != voice_sounds_.end(); ++it)
                if (lowest == voice_sounds_.end() || it->priority < lowest->priority) lowest = it;
            if (lowest != voice_sounds_.end() && lowest->priority < priority) {
                mixer_->stop(lowest->id);
                voice_sounds_.erase(lowest);
            }
        }
        pending_priority_ = priority;
    }
    const int n = play_clip(key, clip, gain, rate_factor);
    if (n > 0 && last_voice_id_ >= 0) {
        std::lock_guard<std::mutex> lk(mtx_);
        voice_sounds_.push_back(LiveVoice{last_voice_id_, sound_id, priority});
    }
    return n;
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

static double play_unit_draw(const std::string& sound_id, std::uint64_t play_index) {
    std::uint64_t h = 1099511628211ull;
    for (char ch : sound_id) {
        h ^= std::uint64_t(static_cast<unsigned char>(ch));
        h *= 1469598103934665603ull;
    }
    h ^= play_index * 0x9E3779B97F4A7C15ull + 0x5851F42D4C957F2Dull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return double(h >> 11) / double(1ull << 53);
}

float MeleeAudio::volume_db_pair_to_gain(const std::string& db_pair, const std::string& sound_id,
                                         std::uint64_t play_index) {
    if (db_pair.empty()) return 1.0f;
    const std::size_t comma = db_pair.find(',');
    double lo = 0.0, hi = 0.0;
    try {
        lo = std::stod(comma == std::string::npos ? db_pair : db_pair.substr(0, comma));
        hi = comma == std::string::npos ? lo : std::stod(db_pair.substr(comma + 1));
    } catch (const std::exception&) {
        return 1.0f;
    }
    if (hi < lo) std::swap(lo, hi);
    const double db = hi == lo ? lo : lo + (hi - lo) * play_unit_draw(sound_id + "|Volume", play_index);
    if (db <= -96.0) return 0.0f;
    if (db >= 12.0) return 4.0f;
    return float(std::pow(10.0, db / 20.0));
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
        if (def->volume) g *= volume_db_pair_to_gain(*def->volume, sound_id, play_index);
        if (def->category) g *= category_gain(*def->category);
        if (a.volume) g *= volume_db_pair_to_gain(*a.volume, sound_id, play_index);
        const double rate = def->pitch ? pitch_rate_factor(*def->pitch, sound_id, play_index) : 1.0;
        return play_clip_limited(sound_id, def->dupe_destroy, def->dupe_mute, a.file, *clip, g, rate, def->dupe_prevent, def->resource_priority);
    }
    return 0;
}

int MeleeAudio::play_event(const std::string& unit_catalog_id, data::UnitEvent ev, float gain, float listener_distance,
                           float listener_pan) {
    if (!bound()) return 0;
    last_rolloff_gain_ = 1.0f;
    pending_pan_ = listener_pan;
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
            if (def->volume) g *= volume_db_pair_to_gain(*def->volume, sid, play_index);
            if (def->category) g *= category_gain(*def->category);
            if (const data::SoundAsset* a = asset_of(chosen); a && a->volume) g *= volume_db_pair_to_gain(*a->volume, sid, play_index);
            const bool two_d = def->mode && *def->mode == "2D";
            if (two_d) pending_pan_ = 0.0f;
            if (!two_d && listener_distance >= 0.0f && !def->rolloff.empty()) {
                const float r = data::rolloff_gain(*def, listener_distance);
                last_rolloff_gain_ = r;
                g *= r;
                if (r <= 0.0f) continue;
            }
        }
        const double rate = def && def->pitch ? pitch_rate_factor(*def->pitch, sid, play_index) : 1.0;
        voices += def ? play_clip_limited(sid, def->dupe_destroy, def->dupe_mute, chosen->asset_path, *clip, g, rate, def->dupe_prevent, def->resource_priority)
                      : play_clip(chosen->asset_path, *clip, g, rate);
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

template <int R>
void fog_box_pass(std::uint8_t* fa, std::uint8_t* ta, int W, int H) {
    constexpr int D = 2 * R + 1;
    const int i0 = std::min(R, W), i1 = std::max(i0, W - R);
    for (int y = 0; y < H; ++y) {
        const std::uint8_t* src = fa + std::size_t(y) * std::size_t(W);
        std::uint8_t* dst = ta + std::size_t(y) * std::size_t(W);
        auto edge = [&](int x) {
            const int lo = x - R < 0 ? 0 : x - R, hi = x + R >= W ? W - 1 : x + R;
            int sum = 0;
            for (int k = lo; k <= hi; ++k) sum += src[k];
            dst[x] = std::uint8_t(sum / (hi - lo + 1));
        };
        for (int x = 0; x < i0; ++x) edge(x);
        for (int x = i0; x < i1; ++x) {
            int sum = 0;
            for (int k = -R; k <= R; ++k) sum += src[x + k];
            dst[x] = std::uint8_t(sum / D);
        }
        for (int x = i1; x < W; ++x) edge(x);
    }
    const int j0 = std::min(R, H), j1 = std::max(j0, H - R);
    auto edge_row = [&](int y) {
        const int lo = y - R < 0 ? 0 : y - R, hi = y + R >= H ? H - 1 : y + R;
        const int n = hi - lo + 1;
        std::uint8_t* dst = fa + std::size_t(y) * std::size_t(W);
        for (int x = 0; x < W; ++x) {
            int sum = 0;
            for (int k = lo; k <= hi; ++k) sum += ta[std::size_t(k) * std::size_t(W) + std::size_t(x)];
            dst[x] = std::uint8_t(sum / n);
        }
    };
    for (int y = 0; y < j0; ++y) edge_row(y);
    for (int y = j0; y < j1; ++y) {
        const std::uint8_t* rows[D];
        for (int k = 0; k < D; ++k) rows[k] = ta + std::size_t(y - R + k) * std::size_t(W);
        std::uint8_t* dst = fa + std::size_t(y) * std::size_t(W);
        for (int x = 0; x < W; ++x) {
            int sum = 0;
            for (int k = 0; k < D; ++k) sum += rows[k][x];
            dst[x] = std::uint8_t(sum / D);
        }
    }
    for (int y = j1; y < H; ++y) edge_row(y);
}

inline void alpha_over_px(uint8_t* b, const uint8_t* t) {
    const uint8_t ta = t[3];
    if (ta == 0) return;
    if (ta == 255) {
        b[0] = t[0];
        b[1] = t[1];
        b[2] = t[2];
        b[3] = 255;
        return;
    }
    float a = ta / 255.0f;
    for (int c = 0; c < 3; ++c)
        b[c] = uint8_t(float(t[c]) * a + float(b[c]) * (1.0f - a) + 0.5f);
    b[3] = uint8_t(std::min(255.0f, float(ta) + float(b[3]) * (1.0f - a) + 0.5f));
}

void alpha_over(std::vector<uint8_t>& base, const std::vector<uint8_t>& top, size_t begin = 0, size_t end = SIZE_MAX) {
    const size_t n = std::min(std::min(base.size(), top.size()), end);
    uint8_t* const b = base.data();
    const uint8_t* const t = top.data();
    size_t i = begin;
    while (i + 15 < n) {
        const uint8_t a0 = t[i + 3], a1 = t[i + 7], a2 = t[i + 11], a3 = t[i + 15];
        if ((a0 | a1 | a2 | a3) == 0) {
            i += 16;
            continue;
        }
        if ((a0 & a1 & a2 & a3) == 255) {
            std::memcpy(b + i, t + i, 16);
            i += 16;
            continue;
        }
        for (int k = 0; k < 4; ++k, i += 4) alpha_over_px(b + i, t + i);
    }
    for (; i + 3 < n; i += 4) alpha_over_px(b + i, t + i);
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

const WireMask& wire_mask(std::unordered_map<const formats::Model*, WireMask>& cache, const formats::Model* m,
                          int W, int H) {
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
    WireMask& slot = cache[m];
    slot = std::move(wm);
    return slot;
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

float cached_diameter(std::unordered_map<const formats::Model*, float>& c, const formats::Model* m) {
    if (!m) return 0.0f;
    auto it = c.find(m);
    if (it != c.end()) return it->second;
    float d = model_diameter(*m);
    c[m] = d;
    return d;
}

void draw_text_rgba(std::vector<std::uint8_t>& buf, std::uint32_t w, std::uint32_t h, int x,
                    int y, const std::string& text, int s, std::uint8_t r, std::uint8_t g,
                    std::uint8_t b) {
    draw::text(buf, w, h, x, y, text, s, r, g, b);
}

struct LabelLook {
    bool set = false;
    std::uint8_t r = 255, g = 255, b = 255;
    bool centred = false;
    std::string font;
    int height = 0;
    bool shadow = true;
    int shadow_offset = 0;
};
struct ShadowScope {
    bool saved_off;
    int saved_px;
    ShadowScope(const LabelLook& l, double sc) : saved_off(draw::text_shadow_off()), saved_px(draw::text_shadow_px()) {
        static const bool kOld = std::getenv("OMC_LABEL_SHADOW_OLD") != nullptr;
        if (kOld || !l.set) return;
        draw::text_shadow_off() = !l.shadow;
        draw::text_shadow_px() = std::max(1, int(double(l.shadow_offset) * sc + 0.5));
    }
    ~ShadowScope() {
        draw::text_shadow_off() = saved_off;
        draw::text_shadow_px() = saved_px;
    }
    ShadowScope(const ShadowScope&) = delete;
    ShadowScope& operator=(const ShadowScope&) = delete;
};
struct FaceScope {
    const render::OverlayFont* saved;
    explicit FaceScope(const render::OverlayFont* f) : saved(draw::active_font()) {
        if (f) draw::set_active_font(f);
    }
    ~FaceScope() { draw::set_active_font(saved); }
    FaceScope(const FaceScope&) = delete;
    FaceScope& operator=(const FaceScope&) = delete;
};
LabelLook resolve_label_style(const ui::StyleSet* styles, const std::string& authored, int race) {
    LabelLook out;
    if (!styles || authored.empty()) return out;
    std::size_t at = 0;
    while (at < authored.size() && authored[at] == '@') ++at;
    const std::string bare = authored.substr(at);
    if (bare.empty()) return out;
    const char* suffix = ui::layout_race_suffix(race);
    ui::FontStyle st;
    if (suffix && *suffix) st = styles->resolve(bare + "_" + suffix);
    if (!st.ok()) st = styles->resolve(bare);
    if (!st.ok()) return out;
    out.set = true;
    out.centred = st.hjustify == "Center";
    if (st.has_color) { out.r = st.r; out.g = st.g; out.b = st.b; }
    out.font = st.font;
    out.height = st.height;
    out.shadow = st.shadow;
    out.shadow_offset = st.shadow_offset;
    if (std::getenv("OMC_LABEL_FACE_TRACE")) {
        static std::set<std::string> seen;
        if (seen.insert(bare).second)
            std::fprintf(stderr, "[labelface] style %s font %s height %d vjustify %s hjustify %s\n", bare.c_str(), st.font.c_str(), st.height,
                         st.vjustify.c_str(), st.hjustify.c_str());
    }
    return out;
}

int text_w_rgba(const std::string& text, int s) { return draw::text_w(text, s); }

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

static std::array<int, 16> g_map_team_slot = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
void set_map_team_slots(const std::array<int, 16>& slots) {
    g_map_team_slot = slots;
    if (std::getenv("OMC_NO_MAP_TEAM_COLOR")) g_map_team_slot.fill(-1);
}
static int team_slot_of(int player) {
    if (player < 0) return 0;
    if (player < 16 && g_map_team_slot[std::size_t(player)] >= 0) return g_map_team_slot[std::size_t(player)];
    return player + 1;
}

std::array<float, 4> team_colour_for(const data::Catalog* cat, int player, bool selected) {
    static const bool neutral_slot = std::getenv("OMC_NO_NEUTRAL_TEAM") == nullptr;
    if (player < 0 && !(cat && neutral_slot)) return {0, 0, 0, -1.0f};
    if (player < 0) selected = false;
    if (cat) {
        static const data::Catalog* cached_for = nullptr;
        static std::vector<data::TeamColor> palette;
        if (cached_for != cat) {
            palette = data::team_colors(*cat);
            cached_for = cat;
        }
        for (const data::TeamColor& tc : palette)
            if (tc.slot == team_slot_of(player)) {
                if (selected) return {tc.selected[0], tc.selected[1], tc.selected[2], 1.0f};
                return {tc.diffuse[0], tc.diffuse[1], tc.diffuse[2], 1.0f};
            }
    }
    return team_colour(player, selected);
}

std::array<float, 4> team_emissive_for(const data::Catalog* cat, int player) {
    static const bool kNoTeamEmissive = std::getenv("OMC_NO_TEAM_EMISSIVE") != nullptr;
    if (!cat || kNoTeamEmissive) return {0, 0, 0, -1.0f};
    static const data::Catalog* cached_for = nullptr;
    static std::vector<data::TeamColor> palette;
    if (cached_for != cat) {
        palette = data::team_colors(*cat);
        cached_for = cat;
    }
    const int slot = team_slot_of(player);
    for (const data::TeamColor& tc : palette)
        if (tc.slot == slot && tc.emissive[3] >= 0.0f) return {tc.emissive[0], tc.emissive[1], tc.emissive[2], 1.0f};
    return {0, 0, 0, -1.0f};
}

std::array<std::uint8_t, 3> minimap_attacked_rgb(const data::Catalog* cat, int player) {
    std::array<std::uint8_t, 3> out = {255, 0, 0};
    if (!cat) return out;
    static const data::Catalog* cached_for = nullptr;
    static std::vector<data::TeamColor> palette;
    if (cached_for != cat) {
        palette = data::team_colors(*cat);
        cached_for = cat;
    }
    static const bool kOldSlot = std::getenv("OMC_MINIMAP_FLASH_SLOT_OLD") != nullptr;
    const int slot = kOldSlot ? player + 1 : team_slot_of(player);
    for (const data::TeamColor& tc : palette)
        if (tc.slot == slot) {
            for (int k = 0; k < 3; ++k) out[std::size_t(k)] = std::uint8_t(tc.attacked[k] * 255.0f + 0.5f);
            break;
        }
    return out;
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
            static const bool kLiftedOld = std::getenv("OMC_UNIV_FALLBACK_OLD") != nullptr;
            if (!kLiftedOld && u.flying && u.speed_air.raw() > 0) {
                ctx.commandable_selected = true;
                ctx.mobile_selected = true;
                ctx.combat_selected = true;
            }
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
    static const bool kUnivOld = std::getenv("OMC_UNIV_FALLBACK_OLD") != nullptr;
    static const bool kOpenerKeysOld = std::getenv("OMC_OPENER_KEYS_OLD") != nullptr;
    ctx.opener_keys_old = kOpenerKeysOld;
    if (cat_ && !selection_.empty()) {
        const std::uint32_t lead = selection_.front();
        if (lead < world_.units().size() && world_.unit(lead).alive &&
            !world_.unit(lead).catalog_id.empty()) {
            ctx.authored_layout = !kUnivOld;
            auto take = [&](const data::Catalog::CommandCardSlot& sl,
                            MeleeCommandContext::UniversalCell& cell) {
                if (sl.row < 0 || sl.col < 0) return;
                const int slot = sl.row * CommandCard::kCols + sl.col;
                if (slot < 0 || slot >= CommandCard::kSlots) return;
                cell.slot = slot;
                if (auto ic = cat_->button_icon(sl.face)) cell.icon = std::move(*ic);
                if (auto nm = cat_->button_display_name(sl.face)) cell.name = std::move(*nm);
                if (auto hk = cat_->button_hotkey(sl.face)) cell.hotkey = std::move(*hk);
            };
            const sim::Unit& lead_u = world_.unit(lead);
            std::string lead_id = card_unit_id(lead_u);
            if (lead_u.burrowed && lead_id == lead_u.catalog_id) {
                const std::string var = cat_->burrowed_variant(lead_id);
                if (!var.empty()) lead_id = var;
            }
            const std::string burrow_ab = cat_->burrow_ability(lead_id);
            const std::string unburrow_ab = cat_->unburrow_ability(lead_id);
            std::string liftoff_ab = cat_->liftoff_ability(lead_id);
            if (liftoff_ab.empty() && lead_id != lead_u.catalog_id) liftoff_ab = cat_->land_ability(lead_id);
            const std::string unload_ab = cat_->transport_ability(lead_id);
            const std::string merge_ab = cat_->merge_ability(lead_id);
            bool attack_acquire = false;
            static const bool kStopArrayOld = std::getenv("OMC_CARD_ABILARRAY_OLD") != nullptr;
            const std::vector<std::string> carried =
                kStopArrayOld ? std::vector<std::string>{} : cat_->array_links("CUnit", lead_id, "AbilArray");
            auto carries = [&](const std::string& ab) {
                return carried.empty() || std::find(carried.begin(), carried.end(), ab) != carried.end();
            };
            for (const data::Catalog::CommandCardLayout& c : cat_->unit_command_cards(lead_id)) {
                if (c.index != 0) continue;
                for (const data::Catalog::CommandCardSlot& sl : c.slots) {
                    if (sl.type != "AbilCmd") continue;
                    if (sl.face == "Move") take(sl, ctx.univ_move);
                    else if (sl.face == "Stop") {
                        if (carries(sl.ability)) take(sl, ctx.univ_stop);
                    }
                    else if (sl.face == "MoveHoldPosition") take(sl, ctx.univ_hold);
                    else if (sl.face == "Attack" || sl.face == "AttackWorker" ||
                             (sl.face == "AttackBuilding" && !std::getenv("OMC_ATTACK_UNIT_CLICK_OLD")))
                        take(sl, ctx.univ_attack);
                    else if (sl.face == "AcquireMove" && ctx.univ_attack.slot < 0 && !kUnivOld) {
                        take(sl, ctx.univ_attack);
                        attack_acquire = true;
                    }
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
            if (!kUnivOld) {
                const sim::Unit& lu = world_.unit(lead);
                const bool moves = (lu.speed.raw() != 0 || (lu.flying && lu.speed_air.raw() > 0)) &&
                                   (!lu.burrowed || lu.can_burrow_move);
                if (!moves) ctx.univ_move.slot = ctx.univ_patrol.slot = ctx.univ_hold.slot = -1;
                if (!moves && attack_acquire) ctx.univ_attack.slot = -1;
                static const bool kImmobileAttackOld = std::getenv("OMC_IMMOBILE_ATTACK_OLD") != nullptr;
                if (!kImmobileAttackOld && !moves && cat_->array_links("CUnit", lead_id, "WeaponArray").empty())
                    ctx.univ_attack.slot = -1;
                if (lu.burrowed && !lu.can_burrow_attack && !lu.can_burrow_move) ctx.univ_attack.slot = -1;
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
    static const bool kLiberatorOpenerOld = std::getenv("OMC_LIBERATOR_OPENER_OLD") != nullptr;
    if (cat_ && kLiberatorOpenerOld) {
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
        if (const std::uint32_t lid = selected_liftoff_structure(false); lid != sim::kNoTarget) {
            ctx.liftoff_selected = true;
            ctx.liftoff_busy = liftoff_busy(lid);
        }
        if (selected_liftoff_structure(true) != sim::kNoTarget) ctx.land_selected = true;
    }
    if (cat_) {
        static const bool kDepotDupOld = std::getenv("OMC_DEPOT_LOWER_DUP_OLD") != nullptr;
        auto authored_cell = [&](const std::string& unit_id, const char* face) {
            const std::string ab = cat_->morph_button_ability(unit_id, face);
            if (ab.empty()) return -1;
            for (const data::Catalog::CommandCardLayout& c : cat_->unit_command_cards(unit_id))
                for (const data::Catalog::CommandCardSlot& sl : c.slots)
                    if (c.index == 0 && sl.type == "AbilCmd" && sl.ability == ab && sl.row >= 0 && sl.col >= 0)
                        return sl.row * CommandCard::kCols + sl.col;
            return -1;
        };
        auto lowered_form = [&](const sim::Unit& du) {
            const auto ms = cat_->morph_spec(cat_->depot_lower_ability(du.catalog_id));
            return ms && !ms->target.empty() ? ms->target : du.catalog_id;
        };
        if (const std::uint32_t d = selected_depot(false); d != sim::kNoTarget) {
            ctx.depot_lower_selected = true;
            if (!kDepotDupOld) {
                ctx.depot_lower_slot = authored_cell(card_unit_id(world_.unit(d)), "Lower");
                if (auto ic = cat_->button_icon("Lower"); ic && ctx.depot_lower_slot >= 0) ctx.depot_lower_icon = *ic;
            }
        }
        if (const std::uint32_t d = selected_depot(true); d != sim::kNoTarget) {
            ctx.depot_raise_selected = true;
            if (!kDepotDupOld) {
                ctx.depot_raise_slot = authored_cell(lowered_form(world_.unit(d)), "Raise");
                if (auto ic = cat_->button_icon("Raise"); ic && ctx.depot_raise_slot >= 0) ctx.depot_raise_icon = *ic;
            }
        }
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
    for (std::uint32_t id : card_full_selection_ ? *card_full_selection_ : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (!cat_->merge_ability(u.catalog_id).empty()) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool MeleeGame::liftoff_busy(std::uint32_t id) const {
    if (!cat_ || std::getenv("OMC_LIFTOFF_WHILE_TRAINING") || id >= world_.units().size()) return false;
    sim::Fixed frac;
    if (!world_.pending_progress(id, frac)) return false;
    return cat_->ability_disables_class(cat_->liftoff_ability(world_.units()[id].catalog_id), "CAbilQueue");
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
        if (u.lowered != lowered || world_.is_depot_morphing(id)) continue;
        if (!cat_->depot_lower_ability(u.catalog_id).empty()) return id;
    }
    return sim::kNoTarget;
}

std::string MeleeGame::zone_marker_of(const std::string& unit_id) const {
    static std::unordered_map<std::string, std::string> cache;
    if (!cat_ || unit_id.empty()) return {};
    auto it = cache.find(unit_id);
    if (it != cache.end()) return it->second;
    std::string found;
    for (const std::string& ab : cat_->array_links("CUnit", unit_id, "AbilArray")) {
        if (ab.empty()) continue;
        const data::Catalog::Entry* e = cat_->find("CAbil", ab);
        if (!e || e->tag != "CAbilEffectTarget") continue;
        found = cat_->ability_point_marker(ab);
        if (!found.empty()) break;
    }
    return cache.emplace(unit_id, found).first->second;
}

const MeleeGame::MorphShow* MeleeGame::morph_show(const sim::Unit& u) {
    if (!cat_ || !models_ || !ui_store_) return nullptr;
    const std::string to = world_.morph_target_id(u.id);
    if (to.empty()) return nullptr;
    const auto ab = morph_abil_.find(u.id);
    const std::string abil = ab != morph_abil_.end() ? ab->second : std::string();
    const std::string key = (abil.empty() ? u.catalog_id : abil) + ">" + to;
    auto it = morph_show_.find(key);
    if (it == morph_show_.end()) {
        MorphShow s;
        if (const auto t = cat_->morph_transition(to))
            if (const formats::Model* m = models_->cmodel(*ui_store_, t->model)) {
                const int sq = render::select_sequence(*m, t->props, 0);
                if (sq >= 0 && std::size_t(sq) < m->sequences.size()) {
                    s.model = m;
                    s.seq = sq;
                    s.len_ms = float(m->sequences[std::size_t(sq)].length_ms());
                    if (!abil.empty()) s.delay_s = cat_->morph_actor_section(abil).first;
                }
                if (std::getenv("OMC_MORPH_TRACE"))
                    std::fprintf(stderr, "[morph] transition %s: actor %s model %s clip %d (%.0f ms) delay %.2f s\n", key.c_str(), t->actor.c_str(),
                                 t->model.c_str(), sq, double(s.len_ms), s.delay_s);
            }
        if (!s.model && morph_cocoon_models_ && abil.empty() && !cat_->merge_ability(u.catalog_id).empty())
            if (const formats::Model* m = models_->ensure(*ui_store_, to)) {
                const int birth = render::select_sequence(*m, {"Birth"}, 0);
                if (birth >= 0 && std::size_t(birth) < m->sequences.size()) {
                    s.model = m;
                    s.seq = birth;
                    s.len_ms = float(m->sequences[std::size_t(birth)].length_ms());
                }
                if (std::getenv("OMC_MORPH_TRACE"))
                    std::fprintf(stderr, "[morph] merge %s: %s birth %d (%.0f ms)\n", key.c_str(), to.c_str(), birth, double(s.len_ms));
            }
        if (!s.model && morph_cocoon_models_ && !abil.empty())
            if (const auto ms = cat_->morph_spec(abil); ms && ms->chain.size() >= 2 && ms->chain.front() != ms->target)
                if (const formats::Model* m = models_->ensure(*ui_store_, ms->chain.front())) {
                    const int birth = render::select_sequence(*m, {"Birth"}, 0);
                    const int stand = render::select_sequence(*m, {"Stand"}, 0);
                    s.model = m;
                    s.seq = birth >= 0 ? birth : stand;
                    s.len_ms = s.seq >= 0 && std::size_t(s.seq) < m->sequences.size() ? float(m->sequences[std::size_t(s.seq)].length_ms()) : 0.0f;
                    if (birth >= 0 && stand >= 0 && std::size_t(stand) < m->sequences.size()) {
                        s.stand_seq = stand;
                        s.stand_ms = float(m->sequences[std::size_t(stand)].length_ms());
                    }
                    if (std::getenv("OMC_MORPH_TRACE"))
                        std::fprintf(stderr, "[morph] cocoon %s: %s birth %d (%.0f ms) stand %d\n", key.c_str(), ms->chain.front().c_str(), birth,
                                     double(s.len_ms), stand);
                }
        it = morph_show_.emplace(key, s).first;
    }
    return it->second.model ? &it->second : nullptr;
}

bool MeleeGame::geyser_covered(const sim::Unit& u) const {
    static const bool kOld = std::getenv("OMC_GEYSER_DRAWN_UNDER") != nullptr;
    if (kOld || !u.alive || !u.resource_raw || !u.is_gas) return false;
    const sim::Fixed tol = sim::Fixed::from_ratio(1, 2);
    for (const sim::Unit& s : world_.units()) {
        if (!s.alive || s.id == u.id || !s.is_gas || s.resource_raw) continue;
        const sim::Fixed dx = s.x > u.x ? s.x - u.x : u.x - s.x, dy = s.y > u.y ? s.y - u.y : u.y - s.y;
        if (dx < tol && dy < tol) return true;
    }
    return false;
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
        if (world_.is_burrowing(id)) continue;
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
    if (setup.no_melee_end) cfg_.no_victory = true;
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
    world_.set_order_morph([this](std::uint32_t unit, const std::string& abil) {
        if (OMC_ENV_ONCE("OMC_EFFECT_MORPH_OLD")) return human_morph(unit, abil);
        if (unit >= world_.units().size() || world_.units()[unit].player < 0) return false;
        const std::int32_t was = morph_as_player_;
        morph_as_player_ = world_.units()[unit].player;
        const bool ok = human_morph(unit, abil);
        morph_as_player_ = was;
        if (!ok) std::fprintf(stderr, "effect morph %s refused for unit %u\n", abil.c_str(), unit);
        return ok;
    });
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
    world_.enable_target_priority(true);

    models_ = models;
    clear_model_caches();
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
    ai_own_period_.clear();
    ai_own_counter_.clear();
    for (size_t p = 0; p < layout_.players.size(); ++p) {
        ai::MeleeConfig cfg = layout_.players[p].config;
        if (int(p) != cfg_.human_player) {
            cfg.smart_combat = true;
            cfg.num_scouts = 1;
            const int rung = p < cfg_.slot_difficulty.size() && cfg_.slot_difficulty[p] > 0 ? cfg_.slot_difficulty[p]
                                                                                            : cfg_.ai_difficulty;
            if (rung > 0 && rung != cfg_.ai_difficulty) {
                int own = cfg_.ai_period_ticks;
                ai::apply_difficulty(cfg, ai::Difficulty(rung), own);
                if (ai_own_period_.size() <= p) ai_own_period_.resize(p + 1, 0);
                ai_own_period_[p] = own > 0 ? own : 1;
            } else if (rung > 0) {
                ai::apply_difficulty(cfg, ai::Difficulty(rung), cfg_.ai_period_ticks);
            }
        }
        auto ai = std::make_unique<ai::MeleeAI>(int(p), cfg);
        ai->set_base(layout_.players[p].base);
        if (map_w_ > 0.0f && map_h_ > 0.0f)
            ai->set_map_bounds(sim::Bounds{sim::Fixed(), sim::Fixed(), sim::Fixed::from_double(map_w_),
                                           sim::Fixed::from_double(map_h_)});
        ais_.push_back(std::move(ai));
    }

    camctl_.yaw = -1.5707963f;
    camctl_.mirror_x = false;
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
    train_tap_.out = &train_events_;
    world_.set_train_sink(&train_tap_);
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
        if (research_) research_->apply_completed(upgrade_fold_new_);
        trace.hashes.push_back(world_.state_hash());
    }
    return trace;
}

void MeleeGame::enable_research(const data::Catalog& cat, const data::UpgradeCatalog& up,
                                std::vector<MeleeResearchDriver::ProtoNames> proto_names) {
    research_ = std::make_unique<MeleeResearchDriver>(cat, up);
    research_->set_world(&world_);
    cat_ = &cat;
    world_.set_unit_refold([this](sim::Unit& u) {
        if (!upgrade_on_create_ || !upgrade_fold_new_ || !research_ || !cat_ || !research_->upgrades()) return;
        fold_unit_lagging(world_, *cat_, *research_->upgrades(), u);
    });
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
            er.target = r.target;
            out.push_back(std::move(er));
        }
        return out;
    };
    driver_.unit_rules = [&cat, convert](const std::string& id) {
        static std::unordered_map<std::string, std::vector<render::ActorEventRule>> cache;
        auto it = cache.find(id);
        if (it == cache.end()) {
            std::vector<render::ActorEventRule> rules = convert(cat.actor_rules(id));
            if (rules.empty() && !std::getenv("OMC_NO_BIRTH_ACTOR_RULES"))
                if (const std::string aid = data::unit_actor_id(cat, id); !aid.empty() && aid != id) rules = convert(cat.actor_rules(aid));
            if (!std::getenv("OMC_NO_PROGRESS_ACTORS")) {
                std::vector<render::ActorEventRule> extra;
                for (render::ActorEventRule& r : rules) {
                    if (r.verb != "Create" || r.args.size() != 1 || !cat.find("CActorProgress", r.args[0])) continue;
                    const std::string pid = r.args[0];
                    std::vector<std::string> stages;
                    for (const core::XmlNode& n : cat.resolved("CActorProgress", pid))
                        if (n.name == "StageArray")
                            if (const std::string* ap = n.attr("AnimProps"); ap && !ap->empty()) stages.push_back(*ap);
                    if (stages.size() != 3) continue;
                    r.verb = "AnimBracketStart";
                    r.args = {pid, stages[0], stages[1], stages[2]};
                    for (const render::ActorEventRule& pr : convert(cat.actor_rules(pid, "CActorProgress"))) {
                        if (pr.verb != "Destroy" || pr.sub == "Start") continue;
                        render::ActorEventRule stop = pr;
                        stop.verb = "AnimBracketStop";
                        stop.args = {pid};
                        extra.push_back(std::move(stop));
                    }
                }
                rules.insert(rules.end(), extra.begin(), extra.end());
            }
            it = cache.emplace(id, std::move(rules)).first;
        }
        return it->second;
    };
    if (!std::getenv("OMC_MORPH_START_AT_FINISH"))
        driver_.morph_target = [this](std::uint32_t id) { return world_.morph_target_id(id); };
    if (!std::getenv("OMC_NO_VALIDATE_PLAYER"))
        driver_.validate_player = [this, &cat](int player, const std::string& vid) -> int {
            const data::Catalog::Entry* e = cat.find("CValidator", vid);
            if (!e || player < 0 || e->tag != "CValidatorPlayerRequirement") return -1;
            const std::string req = cat.scalar("CValidator", vid, "Value").value_or("");
            if (req.empty()) return -1;
            const bool find = cat.scalar("CValidator", vid, "Find").value_or("0") != "0";
            const bool met = data::eval_requirement(cat, req, "Use", tech_state_of(player));
            return met == find ? 1 : 0;
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

void MeleeGame::begin_research(int player, const std::string& id, sim::Fixed delay, std::uint32_t producer,
                               int minerals, int gas) {
    if (research_) research_->begin(player, id, delay, producer, minerals, gas);
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
    std::uint32_t tech_body = sim::kNoTarget;
    for (const sim::Unit& u : world_.units())
        if (u.alive && u.player == player && !u.constructing && u.catalog_id == plan.tech_id) {
            tech_stands = true;
            tech_body = u.id;
            break;
        }
    if (!tech_stands) return;
    const data::Catalog::ResearchInfo info = cat_->research_info(next);
    if (!info.found || info.time <= 0) return;
    sim::Player& pp = world_.player(player);
    if (pp.minerals < info.minerals || pp.gas < info.gas) return;
    pp.minerals -= info.minerals;
    pp.gas -= info.gas;
    research_->begin(player, next, sim::Fixed::from_double(info.time),
                     std::getenv("OMC_RESEARCH_OUTLIVES_HOST") ? sim::kNoTarget : tech_body, info.minerals, info.gas);
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
        static const bool kHangarOld = std::getenv("OMC_HANGAR_QUEUE_OLD") != nullptr;
        if (!kHangarOld && spec->queue_size > 0)
            world_.register_interceptor_hangar(id, spec->queue_size, spec->minerals, spec->gas, spec->auto_build,
                                               spec->auto_build_on, spec->show_status);
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
    wire_cast_folds();
    for (const auto& [unit_id, abil_id] : regs)
        cast_policy_->register_offensive(world_, unit_id, abil_id);
}

int MeleeGame::enable_ai_casting_from_roster(const data::Catalog& cat) {
    cast_policy_ = std::make_unique<CastPolicy>(cat);
    cat_ = &cat;
    wire_cast_folds();
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

void MeleeGame::run_ai_inject(int player) {
    if (!cast_policy_ || !cat_) return;
    const std::size_t U = world_.units().size();
    for (std::uint32_t q = 0; q < U; ++q) {
        const sim::Unit& qu = world_.units()[q];
        if (!qu.alive || qu.player != player || qu.constructing || qu.loaded) continue;
        if (!qu.orders.empty() && qu.orders.front().type != sim::OrderType::Move) continue;
        auto it = inject_abil_.find(qu.catalog_id);
        if (it == inject_abil_.end()) {
            const data::Catalog::InjectInfo ii = cat_->inject_ability(qu.catalog_id);
            it = inject_abil_.emplace(qu.catalog_id, ii.found ? ii.ability : std::string()).first;
        }
        if (it->second.empty()) continue;
        std::uint32_t best = sim::kNoTarget;
        sim::Fixed bd;
        for (const sim::Unit& h : world_.units()) {
            if (!h.alive || h.player != player || h.spawner_proto < 0 || h.constructing || h.speed.raw() != 0) continue;
            if (world_.inject_pending(h.id)) continue;
            const sim::Fixed dx = h.x - qu.x, dy = h.y - qu.y, d2 = dx * dx + dy * dy;
            if (d2 > sim::Fixed::from_int(12 * 12)) continue;
            if (best == sim::kNoTarget || d2 < bd) { best = h.id; bd = d2; }
        }
        if (best == sim::kNoTarget) continue;
        if (cast_policy_->cast_now(world_, q, it->second, best)) ++ai_injects_;
    }
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

float MeleeGame::card_cooldown_shown(const std::string& abil_id) const {
    if (!cast_policy_ || !cat_) return 0.0f;
    static const bool kCdMaxOld = std::getenv("OMC_CARD_COOLDOWN_MAX_OLD") != nullptr;
    float soonest = -1.0f;
    if (!kCdMaxOld)
        for (std::uint32_t sid : selection_) {
            if (sid >= world_.units().size()) continue;
            const sim::Unit& su = world_.units()[sid];
            if (!su.alive || su.catalog_id.empty()) continue;
            const std::vector<std::string> abils = cat_->array_links("CUnit", su.catalog_id, "AbilArray");
            if (std::find(abils.begin(), abils.end(), abil_id) == abils.end()) continue;
            const float f = cast_policy_->cooldown_fraction(world_, sid, abil_id);
            if (soonest < 0.0f || f < soonest) soonest = f;
        }
    if (soonest >= 0.0f) return soonest;
    float worst = 0.0f;
    for (std::uint32_t sid : selection_) {
        const float f = cast_policy_->cooldown_fraction(world_, sid, abil_id);
        if (f > worst) worst = f;
    }
    return worst;
}

bool MeleeGame::autocast_state(std::uint32_t unit, const std::string& abil_id) {
    if (!cast_policy_) return false;
    return cast_policy_->autocast_state(world_, unit, abil_id);
}

bool MeleeGame::human_cast(const std::string& abil_id, std::uint32_t target_id) {
    if (!cast_policy_) return false;
    const std::uint32_t caster = human_lead_caster();
    if (caster == sim::kNoTarget) return false;
    if (!cast_policy_->cast_now(world_, caster, abil_id, target_id, cast_queue_now(abil_id))) return false;
    static const bool kSprayUnitOld = std::getenv("OMC_SPRAY_UNIT_OLD") != nullptr;
    if (!kSprayUnitOld && target_id < world_.units().size())
        spawn_effect_doodads(abil_id, world_.units()[target_id].x, world_.units()[target_id].y,
                             world_.units()[caster].player);
    return true;
}

bool MeleeGame::ability_best_unit(const std::string& abil_id) const {
    if (!cat_) return false;
    int own = -1;
    for (const core::XmlNode& n : cat_->resolved("CAbil", abil_id))
        if ((n.name == "Flags" || n.name == "FlagArray") && n.attr_or("index", "") == "BestUnit")
            own = n.attr_or("value", "") == "1";
    if (own >= 0) return own == 1;
    const data::Catalog::Entry* e = cat_->find("CAbil", abil_id);
    if (!e) return false;
    if (const std::vector<core::XmlNode>* d = cat_->tag_default_indexed_nodes(e->tag))
        for (const core::XmlNode& n : *d)
            if ((n.name == "Flags" || n.name == "FlagArray") && n.attr_or("index", "") == "BestUnit")
                own = n.attr_or("value", "") == "1";
    return own == 1;
}

std::vector<std::uint32_t> MeleeGame::best_unit_target_casters(const std::string& abil_id, sim::Fixed tx,
                                                               sim::Fixed ty) const {
    std::vector<std::uint32_t> out;
    const std::uint32_t lead = human_lead_caster();
    if (lead == sim::kNoTarget) return out;
    out.push_back(lead);
    if (!best_unit_target_pick_ || !cat_ || !ability_best_unit(abil_id)) return out;
    for (std::uint32_t id : selection_) {
        if (id == lead || id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.catalog_id.empty()) continue;
        const std::vector<std::string> abils = cat_->array_links("CUnit", u.catalog_id, "AbilArray");
        if (std::find(abils.begin(), abils.end(), abil_id) == abils.end()) continue;
        if (std::find(out.begin(), out.end(), id) != out.end()) continue;
        out.push_back(id);
    }
    auto dist2 = [&](std::uint32_t id) {
        const sim::Unit& u = world_.units()[id];
        const std::int64_t dx = std::int64_t(u.x.raw()) - std::int64_t(tx.raw());
        const std::int64_t dy = std::int64_t(u.y.raw()) - std::int64_t(ty.raw());
        return dx * dx + dy * dy;
    };
    std::stable_sort(out.begin(), out.end(), [&](std::uint32_t a, std::uint32_t b) { return dist2(a) < dist2(b); });
    if (!sim::AbilitySystem::cost_at_order())
        std::stable_partition(out.begin(), out.end(),
                              [&](std::uint32_t id) { return !CastPolicy::pending_cast(world_, id); });
    return out;
}

std::vector<std::uint32_t> MeleeGame::every_unit_target_casters(const std::string& abil_id) const {
    std::vector<std::uint32_t> out;
    if (!every_unit_target_cast_ || !cat_ || ability_best_unit(abil_id)) return out;
    const std::uint32_t lead = human_lead_caster();
    if (lead == sim::kNoTarget) return out;
    out.push_back(lead);
    for (std::uint32_t id : selection_) {
        if (id == lead || id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.catalog_id.empty()) continue;
        const std::vector<std::string> abils = cat_->array_links("CUnit", u.catalog_id, "AbilArray");
        if (std::find(abils.begin(), abils.end(), abil_id) == abils.end()) continue;
        if (std::find(out.begin(), out.end(), id) != out.end()) continue;
        out.push_back(id);
    }
    return out;
}

bool MeleeGame::human_cast_target(const std::string& abil_id, std::uint32_t target_id) {
    if (!cast_policy_ || target_id >= world_.units().size()) return human_cast(abil_id, target_id);
    const std::vector<std::uint32_t> every = every_unit_target_casters(abil_id);
    if (!every.empty()) {
        static const bool kSprayUnitOld = std::getenv("OMC_SPRAY_UNIT_OLD") != nullptr;
        int cast = 0;
        for (std::uint32_t caster : every) {
            if (caster == target_id) continue;
            if (!cast_policy_->cast_now(world_, caster, abil_id, target_id, cast_queue_now(abil_id))) continue;
            ++cast;
            if (!kSprayUnitOld)
                spawn_effect_doodads(abil_id, world_.units()[target_id].x, world_.units()[target_id].y,
                                     world_.units()[caster].player);
        }
        return cast > 0;
    }
    const sim::Unit& t = world_.units()[target_id];
    for (std::uint32_t caster : best_unit_target_casters(abil_id, t.x, t.y)) {
        if (!cast_policy_->cast_now(world_, caster, abil_id, target_id, cast_queue_now(abil_id))) continue;
        static const bool kSprayUnitOld = std::getenv("OMC_SPRAY_UNIT_OLD") != nullptr;
        if (!kSprayUnitOld)
            spawn_effect_doodads(abil_id, world_.units()[target_id].x, world_.units()[target_id].y,
                                 world_.units()[caster].player);
        return true;
    }
    return false;
}

int MeleeGame::human_cast_instant(const std::string& abil_id) {
    const std::uint32_t lead = human_lead_caster();
    if (lead == sim::kNoTarget) return 0;
    const bool best_unit = ability_best_unit(abil_id);
    if (group_self_cast_ && cat_ && cast_policy_ && group_cast_cell_) {
        std::vector<std::pair<std::uint32_t, std::string>> cell = group_cell_casters(abil_id);
        if (cell.empty()) return human_cast(abil_id, lead) ? 1 : 0;
        if (best_unit && best_unit_pick_) {
            std::stable_sort(cell.begin(), cell.end(), [&](const auto& a, const auto& b) {
                return world_.units()[a.first].energy > world_.units()[b.first].energy;
            });
            for (const auto& c : cell)
                if (human_cast_by(c.second, c.first)) return 1;
            return 0;
        }
        if (best_unit) return human_cast(abil_id, lead) ? 1 : 0;
        int cast = 0;
        for (const auto& c : cell)
            if (human_cast_by(c.second, c.first)) ++cast;
        return cast;
    }
    if (!group_self_cast_ || !cat_ || !cast_policy_ || best_unit) return human_cast(abil_id, lead) ? 1 : 0;
    std::vector<std::uint32_t> casters{lead};
    for (std::uint32_t id : selection_) {
        if (id == lead || id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.catalog_id.empty()) continue;
        const std::vector<std::string> abils = cat_->array_links("CUnit", u.catalog_id, "AbilArray");
        if (std::find(abils.begin(), abils.end(), abil_id) == abils.end()) continue;
        casters.push_back(id);
    }
    int cast = 0;
    for (std::uint32_t id : casters)
        if (human_cast_by(abil_id, id)) ++cast;
    return cast;
}

std::vector<std::pair<std::uint32_t, std::string>> MeleeGame::group_cell_casters(const std::string& abil_id) const {
    std::vector<std::pair<std::uint32_t, std::string>> out;
    const std::uint32_t lead = human_lead_caster();
    if (lead == sim::kNoTarget || !cat_) return out;
    const AbilityCardEntry* key = nullptr;
    const std::vector<AbilityCardEntry> lead_entries = selection_ability_entries();
    for (const AbilityCardEntry& e : lead_entries)
        if (e.ability == abil_id && e.card == card_page_ && e.slot >= 0) { key = &e; break; }
    if (!key) return out;
    out.push_back({lead, abil_id});
    std::unordered_map<std::string, std::string> by_type;
    for (std::uint32_t id : selection_) {
        if (id == lead || id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.catalog_id.empty()) continue;
        const std::string type = card_unit_id(u);
        if (type.empty()) continue;
        auto it = by_type.find(type);
        if (it == by_type.end()) {
            std::string pick;
            for (const AbilityCardEntry& e : build_ability_card_entries(*cat_, type, human_tech_state(id))) {
                if (e.slot != key->slot || e.card != key->card || !e.requirement_met) continue;
                if (e.face != key->face && (key->hotkey.empty() || e.hotkey != key->hotkey)) continue;
                if (ability_target_mode(*cat_, e.ability) != AbilTargetMode::Instant) continue;
                pick = e.ability;
                break;
            }
            it = by_type.emplace(type, pick).first;
        }
        if (!it->second.empty()) out.push_back({id, it->second});
    }
    return out;
}

bool MeleeGame::human_cast_by(const std::string& abil_id, std::uint32_t caster) {
    if (!cast_policy_ || caster >= world_.units().size()) return false;
    if (!cast_policy_->cast_now(world_, caster, abil_id, caster, cast_queue_now(abil_id))) return false;
    static const bool kSprayUnitOld = std::getenv("OMC_SPRAY_UNIT_OLD") != nullptr;
    if (!kSprayUnitOld)
        spawn_effect_doodads(abil_id, world_.units()[caster].x, world_.units()[caster].y, world_.units()[caster].player);
    return true;
}

bool MeleeGame::human_cast_point(const std::string& abil_id, sim::Fixed px, sim::Fixed py) {
    if (!cast_policy_) return false;
    const std::vector<std::uint32_t> every = every_unit_target_casters(abil_id);
    if (!every.empty()) {
        int cast = 0;
        for (std::uint32_t caster : every) {
            if (!cast_policy_->cast_now_point(world_, caster, abil_id, px, py, cast_queue_now(abil_id))) continue;
            ++cast;
            spawn_effect_doodads(abil_id, px, py, world_.units()[caster].player);
        }
        return cast > 0;
    }
    for (std::uint32_t caster : best_unit_target_casters(abil_id, px, py)) {
        if (!cast_policy_->cast_now_point(world_, caster, abil_id, px, py, cast_queue_now(abil_id))) continue;
        spawn_effect_doodads(abil_id, px, py, world_.units()[caster].player);
        return true;
    }
    return false;
}

void MeleeGame::spawn_effect_doodads(const std::string& abil_id, sim::Fixed px, sim::Fixed py, int player) {
    static const bool off = std::getenv("OMC_NO_EFFECT_DOODAD") != nullptr;
    if (off || !cat_ || !models_ || !ui_store_) return;
    std::string eff = cat_->scalar("CAbil", abil_id, "Effect").value_or("");
    if (eff.empty()) eff = abil_id;
    std::string actor = eff;
    bool creates = false;
    if (std::getenv("OMC_NO_EFFECT_ACTORS") == nullptr) {
        for (const data::Catalog::ActorSubscription& s : cat_->actors_on("Effect", eff, "Start"))
            if (s.family == "CActorDoodad" && s.filters.empty()) {
                actor = s.actor;
                creates = true;
                break;
            }
    }
    const std::vector<data::Catalog::ActorRule> rules = cat_->actor_rules(actor, "CActorDoodad");
    if (std::getenv("OMC_NO_EFFECT_ACTORS") != nullptr)
        for (const data::Catalog::ActorRule& r : rules)
            if (r.head == "Effect" && r.subject == eff && r.sub == "Start" && r.verb == "Create" && r.args.empty() && r.target.empty())
                creates = true;
    static const bool no_sound = std::getenv("OMC_NO_EFFECT_SOUND") != nullptr;
    if (!no_sound)
        for (const data::Catalog::ActorRule& r : rules) {
            if (r.head != "Effect" || r.subject != eff || r.sub != "Start" || r.verb != "Create" || r.args.size() != 1) continue;
            std::string sound;
            for (const core::XmlNode& n : cat_->resolved("CActorSound", r.args[0]))
                if (n.name == "Sound") sound = n.attr_or("value", "");
            if (sound.empty()) continue;
            last_effect_sound_ = sound;
            if (audio_) audio_->play_sound(sound, 1.0f);
        }
    if (!creates) return;
    std::string model_id;
    for (const data::Catalog::ActorRule& r : rules) {
        if (r.head != "ActorCreation" || r.verb != "ModelSwap" || r.args.empty()) continue;
        bool ok = true;
        for (const auto& f : r.filters) {
            if (f.first == "!PlayerHasReward") continue;
            ok = false;
        }
        if (ok) { model_id = r.args[0]; break; }
    }
    if (model_id.empty()) return;
    const formats::Model* m = models_->cmodel(*ui_store_, model_id);
    if (!m || m->sequences.empty()) return;
    EffectDoodad d;
    d.model = m;
    d.model_id = model_id;
    const char* names[3] = {"Birth", "Stand", "Death"};
    for (int k = 0; k < 3; ++k) d.seq[k] = render::select_sequence(*m, {names[k]}, 0);
    d.phase = d.seq[0] >= 0 ? 0 : 1;
    d.x = float(px.to_double());
    d.y = float(py.to_double());
    d.z = ground_z_at(d.x, d.y);
    d.yaw = 1.5707963f;
    d.player = player;
    int standing = 0;
    for (const EffectDoodad& o : effect_doodads_)
        if (o.player == player && o.model_id == model_id && o.phase < 2) ++standing;
    if (standing >= 7)
        for (EffectDoodad& o : effect_doodads_)
            if (o.player == player && o.model_id == model_id && o.phase < 2) {
                o.phase = 2;
                o.time_ms = 0.0f;
                break;
            }
    effect_doodads_.push_back(d);
}

void MeleeGame::set_placement_ghost(const std::string& unit_id, sim::Fixed x, sim::Fixed y, bool rules_ok) {
    ghost_.active = !unit_id.empty();
    ghost_.unit_id = unit_id;
    ghost_.x = float(x.to_double());
    ghost_.y = float(y.to_double());
    ghost_.ok = rules_ok;
    ghost_.cells.clear();
    ghost_.cell_ok.clear();
    if (!cat_ || unit_id.empty()) return;
    const bool gas = unit_id == build_proto_.catalog_id && build_proto_.requires_vespene;
    const int ox = int(std::floor(ghost_.x)), oy = int(std::floor(ghost_.y));
    for (const std::pair<int, int>& c : cat_->unit_pathing_footprint_offsets(unit_id)) {
        const int cx = ox + c.first, cy = oy + c.second;
        bool ok = rules_ok && cx >= 0 && cy >= 0 && float(cx) < map_w_ && float(cy) < map_h_;
        if (ok && world_.placement().configured()) {
            int gx = 0, gy = 0;
            world_.placement().to_cell(sim::Fixed::from_double(cx + 0.5), sim::Fixed::from_double(cy + 0.5), gx, gy);
            ok = world_.placement().can_place(gx, gy, 1, 1,  gas);
        }
        if (ok)
            for (const Unit& u : world_.units()) {
                if (!u.alive || u.flying || (gas && u.is_gas && u.resource_left > 0)) continue;
                const double ux = u.x.to_double(), uy = u.y.to_double(), r = u.radius.to_double();
                const double nx = std::clamp(ux, double(cx), double(cx + 1)), ny = std::clamp(uy, double(cy), double(cy + 1));
                if ((ux - nx) * (ux - nx) + (uy - ny) * (uy - ny) < r * r) {
                    ok = false;
                    break;
                }
            }
        ghost_.cells.emplace_back(cx, cy);
        ghost_.cell_ok.push_back(ok ? 1 : 0);
    }
}

std::string MeleeGame::card_unit_id(const sim::Unit& u) const {
    static const bool kOld = std::getenv("OMC_LAND_CARD_OLD") != nullptr;
    if (!kOld && cat_ && u.flying && !u.catalog_id.empty()) {
        const std::string air = cat_->liftoff_target(u.catalog_id);
        if (!air.empty() && cat_->find("CUnit", air)) return air;
    }
    static const bool kBurrowOld = std::getenv("OMC_BURROW_CARD_OLD") != nullptr;
    if (!kBurrowOld && cat_ && u.burrowed && !u.catalog_id.empty()) {
        const std::string var = cat_->burrowed_variant(u.catalog_id);
        if (!var.empty() && var != u.catalog_id && cat_->find("CUnit", var)) return var;
    }
    return u.catalog_id;
}

bool MeleeGame::land_site(std::uint32_t id, sim::Fixed& x, sim::Fixed& y) const {
    if (id >= world_.units().size()) return false;
    const sim::Unit& u = world_.units()[id];
    if (!u.alive || !u.flying) return false;
    double fx = 0.5, fy = 0.5;
    if (cat_ && cat_->unit_placement_grid_phase(u.catalog_id, fx, fy)) {
        auto snap = [](sim::Fixed v, double phase) {
            return sim::Fixed::from_double(std::floor(v.to_double() - phase + 0.5) + phase);
        };
        x = snap(x, fx);
        y = snap(y, fy);
    }
    const bool in_bounds = x.to_double() >= 0.0 && y.to_double() >= 0.0 && x.to_double() <= double(map_w_) &&
                           y.to_double() <= double(map_h_);
    return in_bounds && u.lift_lock.raw() <= 0 && world_.can_land(id, x, y);
}

bool MeleeGame::morph_blocks_queue(std::uint32_t id) const {
    static const bool kOld = std::getenv("OMC_MORPH_QUEUE_OPEN") != nullptr;
    if (kOld || !cat_ || !world_.is_morphing(id)) return false;
    const auto it = morph_abil_.find(id);
    return it != morph_abil_.end() && cat_->ability_disables_class(it->second, "CAbilQueue");
}

bool MeleeGame::morph_start_blocked(std::uint32_t id, const std::string& morph_abil) const {
    if (std::getenv("OMC_MORPH_QUEUE_OPEN") || !cat_ || id >= world_.units().size()) return false;
    sim::Fixed f;
    return world_.pending_progress(id, f) && cat_->ability_disables_class(morph_abil, "CAbilQueue");
}

bool MeleeGame::effect_cancel_live(std::uint32_t id) const {
    if (id >= world_.units().size()) return false;
    const sim::Unit& u = world_.units()[id];
    if (!u.alive) return false;
    if (world_.is_channelling(id) || world_.is_healing(id)) return true;
    return !u.orders.empty() && u.orders.front().type == sim::OrderType::Cast;
}

bool MeleeGame::human_cancel_effect(const std::string& abil) {
    if (!cat_ || cfg_.human_player < 0) return false;
    const data::Catalog::Entry* ce = cat_->find("CAbil", abil);
    if (!ce || ce->tag.rfind("CAbilEffect", 0) != 0) return false;
    bool any = false;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || !effect_cancel_live(id)) continue;
        world_.cancel_channel(id);
        world_.stop_heal(id);
        if (!world_.units()[id].orders.empty() && world_.units()[id].orders.front().type == sim::OrderType::Cast)
            world_.stop(id);
        any = true;
    }
    return any;
}

bool MeleeGame::human_cancel_morph(const std::string& morph_abil) {
    if (!cat_ || cfg_.human_player < 0) return false;
    bool any = false;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size() || world_.unit(id).player != cfg_.human_player) continue;
        if (const auto rw = root_walks_.find(id); rw != root_walks_.end() && rw->second.abil == morph_abil) {
            root_walks_.erase(rw);
            world_.stop(id);
            any = true;
            continue;
        }
        const auto it = morph_abil_.find(id);
        if (it == morph_abil_.end() || it->second != morph_abil || !world_.is_morphing(id)) continue;
        if (!world_.cancel_morph(id)) continue;
        const auto paid = morph_paid_.count(id) ? morph_paid_[id] : std::pair<std::int32_t, std::int32_t>{0, 0};
        const auto rf = cat_->refund_fraction(morph_abil, "Minerals");
        const auto rg = cat_->refund_fraction(morph_abil, "Vespene");
        sim::Player& hp = world_.player(cfg_.human_player);
        hp.minerals += std::int32_t(double(paid.first) * (rf ? *rf : 0.75));
        hp.gas += std::int32_t(double(paid.second) * (rg ? *rg : 0.75));
        morph_abil_.erase(id);
        morph_paid_.erase(id);
        any = true;
    }
    return any;
}

std::vector<MeleeGame::PaneQueueSlot> MeleeGame::pane_queue(std::uint32_t sel) const {
    std::vector<PaneQueueSlot> out;
    if (sel >= world_.units().size()) return out;
    const sim::Unit& u = world_.units()[sel];
    static const bool kMorphOld = std::getenv("OMC_MORPH_PANE_OLD") != nullptr;
    if (!kMorphOld && cat_ && world_.is_morphing(sel)) {
        const auto it = morph_abil_.find(sel);
        bool show = false;
        if (it != morph_abil_.end())
            for (const core::XmlNode& n : cat_->resolved("CAbil", it->second))
                if (n.name == "Flags" && n.attr_or("index", "") == "ShowProgress") show = n.attr_or("value", "0") != "0";
        if (show) {
            PaneQueueSlot s;
            s.subject = world_.morph_target_id(sel);
            if (u.morph_total.raw() > 0) {
                const double f = 1.0 - u.morph_timer.to_double() / u.morph_total.to_double();
                s.progress = float(f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f));
            }
            s.activity = cat_->scalar("CAbil", it->second, "Activity").value_or("");
            if (s.activity.empty()) s.activity = cat_->tag_default_scalar("CAbilMorph", "Activity").value_or("UI/Morphing");
            out.push_back(std::move(s));
        }
    }
    for (const sim::World::QueueSlotView& q : world_.queue_view(sel)) {
        PaneQueueSlot s;
        s.subject = q.catalog_id;
        s.progress = q.progress;
        s.activity = "UI/Training";
        out.push_back(std::move(s));
    }
    for (const sim::World::ResearchSlotView& r : world_.research_at(sel)) {
        PaneQueueSlot s;
        s.subject = r.id;
        s.progress = r.progress;
        s.activity = cat_ ? cat_->tag_default_scalar("CAbilResearch", "Activity").value_or("UI/Researching") : "UI/Researching";
        if (s.activity.empty()) s.activity = "UI/Researching";
        out.push_back(std::move(s));
    }
    return out;
}

const formats::Model* MeleeGame::placement_ghost_model() const {
    if (!ghost_.active || !cat_ || !models_ || !ui_store_) return nullptr;
    std::string actor = data::unit_actor_id(*cat_, ghost_.unit_id);
    if (actor.empty()) actor = ghost_.unit_id;
    std::string pm = cat_->scalar("CActorUnit", actor, "PlacementModel").value_or("");
    for (std::size_t at; (at = pm.find("##unitName##")) != std::string::npos;)
        pm.replace(at, 12, cat_->actor_unit_name(actor, "CActorUnit"));
    const formats::Model* m = pm.empty() ? nullptr : models_->cmodel(*ui_store_, pm);
    if (!m) m = models_->cmodel(*ui_store_, ghost_.unit_id);
    return m;
}

bool MeleeGame::draws_authored_warp_in(const std::string& catalog_id, const formats::Model* drawn) {
    static const bool kOld = std::getenv("OMC_WARPIN_PRESENT_OLD") != nullptr;
    if (kOld || !drawn || !models_ || !ui_store_ || catalog_id.empty()) return false;
    auto it = warp_build_model_.find(catalog_id);
    if (it == warp_build_model_.end())
        it = warp_build_model_.emplace(catalog_id, models_->ensure_build(*ui_store_, catalog_id)).first;
    return it->second != nullptr && it->second == drawn;
}

const formats::Model* MeleeGame::warp_ghost_model() const {
    if (!warp_ghost_.active || !pending_warp_ || warp_ghost_.unit_id.empty() || !cat_ || !models_ || !ui_store_) return nullptr;
    std::string actor = data::unit_actor_id(*cat_, warp_ghost_.unit_id);
    if (actor.empty()) actor = warp_ghost_.unit_id;
    std::string pm = cat_->scalar("CActorUnit", actor, "PlacementModel").value_or("");
    for (std::size_t at; (at = pm.find("##unitName##")) != std::string::npos;)
        pm.replace(at, 12, cat_->actor_unit_name(actor, "CActorUnit"));
    const formats::Model* m = pm.empty() ? nullptr : models_->cmodel(*ui_store_, pm);
    if (!m) m = models_->cmodel(*ui_store_, warp_ghost_.unit_id);
    return m;
}

std::vector<AbilityCardEntry> MeleeGame::selection_ability_entries() const {
    if (!cat_) return {};
    const std::uint32_t lead = human_lead_caster();
    if (lead == sim::kNoTarget) return {};
    const std::string type = card_unit_id(world_.units()[lead]);
    if (type.empty()) return {};
    std::vector<AbilityCardEntry> out =
        build_ability_card_entries(*cat_, type, human_tech_state(lead));
    if (lead < world_.units().size()) {
        const std::vector<std::uint32_t> hold = world_.passengers(lead);
        auto passenger_can_use = [&](const std::string& abil) {
            const auto target = cat_->ability_redirect_target(abil);
            if (!target) return false;
            for (std::uint32_t pid : hold) {
                if (pid >= world_.units().size() || !world_.units()[pid].alive) continue;
                for (const auto& ac : cat_->unit_ability_commands(world_.units()[pid].catalog_id))
                    if (ac.first == *target) return true;
            }
            return false;
        };
        static const bool kSetCellOld = std::getenv("OMC_ABILSET_CELL_OLD") != nullptr;
        auto set_key = [&](const std::string& set, int slot) {
            return kSetCellOld ? set : set + "@" + std::to_string(slot);
        };
        std::vector<std::string> settled;
        std::vector<AbilityCardEntry> kept;
        kept.reserve(out.size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto set = cat_->ability_set_id(out[i].ability);
            if (!set) { kept.push_back(out[i]); continue; }
            const std::string key = set_key(*set, out[i].slot);
            if (std::find(settled.begin(), settled.end(), key) != settled.end()) continue;
            std::size_t pick = i;
            for (std::size_t j = i; j < out.size(); ++j) {
                const auto other = cat_->ability_set_id(out[j].ability);
                if (!other || set_key(*other, out[j].slot) != key) continue;
                if (passenger_can_use(out[j].ability)) { pick = j; break; }
            }
            settled.push_back(key);
            kept.push_back(out[pick]);
        }
        out = std::move(kept);
    }
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const AbilityCardEntry& e) {
                                 return unpowered_offer_blocked(lead, e.ability);
                             }),
              out.end());
    for (AbilityCardEntry& e : out)
        if (e.command != "Cancel" && morph_start_blocked(lead, e.ability)) e.requirement_met = false;
    static const bool kMergeSingleOld = std::getenv("OMC_ARCHON_WARP_SINGLE_OLD") != nullptr;
    if (!kMergeSingleOld) {
        const std::string merge_ab = cat_->merge_ability(world_.units()[lead].catalog_id);
        if (!merge_ab.empty() && selected_merge_templar().size() < 2)
            for (AbilityCardEntry& e : out)
                if (e.ability == merge_ab) e.requirement_met = false;
    }
    static const bool return_always = [] { const char* e = std::getenv("OMC_RETURN_CARGO_ALWAYS"); return e && e[0] == '1'; }();
    if (!return_always) {
        bool any_carry = false;
        for (std::uint32_t id : selection_)
            if (id < world_.units().size() && world_.units()[id].alive && world_.units()[id].carrying > 0) any_carry = true;
        if (!any_carry)
            out.erase(std::remove_if(out.begin(), out.end(),
                                     [&](const AbilityCardEntry& e) {
                                         return e.command == "Return" && cat_->is_harvest_ability(e.ability);
                                     }),
                      out.end());
    }
    static const bool no_badge = [] { const char* e = std::getenv("OMC_NO_CHARGE_BADGE"); return e && e[0] == '1'; }();
    if (!no_badge) {
        auto ticks = [](double s) { return int(s * double(sim::World::kTicksPerSecond) + 0.5); };
        const int32_t owner = lead < world_.units().size() ? world_.units()[lead].player : -1;
        for (AbilityCardEntry& e : out) {
            const data::ChargeSpec c = data::ability_cost_charge(*cat_, e.ability);
            if (!c.found || c.count_max <= 1.0) continue;
            const int32_t start = c.count_start > 0.0 ? int32_t(c.count_start) : int32_t(c.count_max);
            e.charges = world_.charge_pool_count(owner, c.link.empty() ? e.ability : c.link, start, int32_t(c.count_max),
                                                 ticks(c.time_start), ticks(c.time_use));
        }
    }
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
        btn.ability = 0;
        btn.cooldown = e.cooldown;
        btn.autocast = e.autocast;
        btn.charges = e.charges;
        btn.state = resolve_button_state(e.requirement_met, e.cooldown);
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
    const int viewer = cfg_.human_player >= 0 ? cfg_.human_player : 0;
    if (layout_.players.size() > 2 && player != viewer && !std::getenv("OMC_SURRENDER_DECIDES_MATCH")) {
        if (player >= int(layout_.players.size())) return;
        world_.player(player).defeated = true;
        return;
    }
    if (layout_.players.size() > 2 && player < int(layout_.players.size())) world_.player(player).defeated = true;
    surrendered_player_ = player;
    outcome_ = player == viewer ? MatchResult::Outcome::Defeat : MatchResult::Outcome::Victory;
}

bool MeleeGame::viewer_defeated() const {
    if (outcome_ != MatchResult::Outcome::Ongoing) return false;
    return MatchResult::viewer_defeated(world_, cfg_.human_player >= 0 ? cfg_.human_player : 0);
}

static bool card_hotkey_first_char_old() {
    static const bool v = std::getenv("OMC_CARD_HOTKEY_FIRST_CHAR_OLD") != nullptr;
    return v;
}

static std::string hotkey_key_name(std::string name) {
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(name.begin());
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r')) name.pop_back();
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') name[0] = char(name[0] - 'a' + 'A');
    if (name == "Esc") name = "Escape";
    return name;
}

bool hotkey_names_match(const std::string& spec, const std::vector<std::string>& keys) {
    std::size_t start = 0;
    while (start <= spec.size()) {
        std::size_t comma = spec.find(',', start);
        if (comma == std::string::npos) comma = spec.size();
        const std::string name = hotkey_key_name(spec.substr(start, comma - start));
        start = comma + 1;
        if (name.empty() || name.find('+') != std::string::npos) continue;
        for (const std::string& k : keys)
            if (hotkey_key_name(k) == name) return true;
    }
    return false;
}

int card_slot_for_keys(const CommandCard& card, const std::vector<std::string>& keys) {
    if (keys.empty()) return -1;
    for (int i = 0; i < CommandCard::kSlots; ++i) {
        const CommandButton& b = card.slots[i];
        if (b.state == ButtonState::Empty || b.hotkey.empty()) continue;
        if (hotkey_names_match(b.hotkey, keys)) return i;
    }
    return -1;
}

int card_slot_for_key(const CommandCard& card, char key) {
    if (!key) return -1;
    const char up = (key >= 'a' && key <= 'z') ? char(key - 'a' + 'A') : key;
    if (!card_hotkey_first_char_old()) return card_slot_for_keys(card, {std::string(1, up)});
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
        const auto cards = cat_->unit_command_cards(card_unit_id(u));
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
                btn.hotkey = e.hotkey;
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

CommandCard MeleeGame::compose_command_card_cached(std::vector<BuildCardEntry>& builds, std::vector<AbilityCardEntry>& abils,
                                                   std::vector<TrainCardEntry>& trains, std::vector<SubmenuCardEntry>& submenus) {
    if (!card_cache_on_) return compose_command_card(builds, abils, trains, submenus);
    const std::uint64_t h = world_.state_hash() ^ (std::uint64_t(int(outcome_)) * 0x9E3779B97F4A7C15ull);
    CardCache& c = card_cache_;
    if (c.valid && c.hash == h && c.selection == selection_ && c.page == card_page_ && c.producer == human_producer_ &&
        c.units == world_.units().size() && c.active == sel_groups_.active && c.groups == sel_groups_.subgroups.size()) {
        render::frame_prof_count("card.reused", 1.0);
        ++card_cache_hits_;
        builds = c.builds;
        abils = c.abils;
        trains = c.trains;
        submenus = c.submenus;
        return c.card;
    }
    c.valid = false;
    c.hash = h;
    c.selection = selection_;
    c.page = card_page_;
    c.producer = human_producer_;
    c.units = world_.units().size();
    c.active = sel_groups_.active;
    c.groups = sel_groups_.subgroups.size();
    c.card = compose_command_card(builds, abils, trains, submenus);
    c.builds = builds;
    c.abils = abils;
    c.trains = trains;
    c.submenus = submenus;
    c.valid = true;
    return c.card;
}

CommandCard MeleeGame::compose_command_card(std::vector<BuildCardEntry>& builds,
                                            std::vector<AbilityCardEntry>& abils,
                                            std::vector<TrainCardEntry>& trains,
                                            std::vector<SubmenuCardEntry>& submenus) {
    static const bool kActiveSubgroupOld = std::getenv("OMC_CARD_ACTIVE_SUBGROUP_OLD") != nullptr;
    if (!kActiveSubgroupOld && sel_groups_.subgroups.size() > 1) {
        const std::size_t a = sel_groups_.active < sel_groups_.subgroups.size() ? sel_groups_.active : 0;
        std::vector<std::uint32_t> members;
        for (std::uint32_t id : sel_groups_.subgroups[a].units)
            if (std::find(selection_.begin(), selection_.end(), id) != selection_.end()) members.push_back(id);
        if (!members.empty() && members.size() != selection_.size()) {
            selection_.swap(members);
            card_full_selection_ = &members;
            const CommandCard active_card = compose_subgroup_card(builds, abils, trains, submenus);
            card_full_selection_ = nullptr;
            selection_.swap(members);
            return active_card;
        }
    }
    return compose_subgroup_card(builds, abils, trains, submenus);
}

CommandCard MeleeGame::compose_subgroup_card(std::vector<BuildCardEntry>& builds,
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
    } else if (card_page_ == 0 && !std::getenv("OMC_IDLE_CANCEL_CELL")) {
        bool morphing = false;
        for (std::uint32_t id : selection_)
            if ((id < world_.units().size() && world_.is_morphing(id)) || root_walks_.count(id)) morphing = true;
        static const bool kNoChannelCancel = std::getenv("OMC_NO_CHANNEL_CANCEL") != nullptr;
        if (!kNoChannelCancel)
            for (std::uint32_t id : selection_)
                if (id < world_.units().size() && world_.is_channelling(id)) morphing = true;
        if (!morphing)
            for (int i = 0; i < CommandCard::kSlots; ++i)
                if (card.slots[i].is_cancel) card.slots[i] = CommandButton{};
    }
    if (cat_ && !std::getenv("OMC_DISABLED_CARD_OLD") && !selection_.empty() && selection_[0] < world_.units().size()) {
        const std::uint32_t lead = selection_[0];
        const sim::Unit& lu = world_.unit(lead);
        bool keep_cancel = false, clear = false;
        if (lu.alive && lu.player == cfg_.human_player) {
            if (lu.salvage_timer.raw() > 0) clear = true;
            const auto it = morph_abil_.find(lead);
            if (world_.is_morphing(lead) && it != morph_abil_.end())
                for (const core::XmlNode& n : cat_->resolved("CAbil", it->second))
                    if (n.name == "Flags" && n.attr_or("index", "") == "DisableAbils" && n.attr_or("value", "0") != "0")
                        clear = keep_cancel = true;
        }
        static const bool kCancelOnly = std::getenv("OMC_MORPH_CARD_CANCEL_ONLY") != nullptr;
        if (clear && keep_cancel && !kCancelOnly && lu.salvage_timer.raw() <= 0) {
            static const bool kMorphBehaviorOnOld = std::getenv("OMC_MORPH_BEHAVIOR_ON_CARD_OLD") != nullptr;
            std::vector<std::string> link_off;
            if (const auto mit = morph_abil_.find(lead); !kMorphBehaviorOnOld && mit != morph_abil_.end())
                for (const core::XmlNode& n : cat_->resolved("CAbil", mit->second)) {
                    if (n.name != "BehaviorOn") continue;
                    const std::string beh = n.attr_or("value", "");
                    if (beh.empty()) continue;
                    if (const auto bb = cat_->behavior_buff(beh))
                        link_off.insert(link_off.end(), bb->disabled_abils.begin(), bb->disabled_abils.end());
                }
            std::vector<int> live;
            for (const AbilityCardEntry& e : abils)
                if (const data::Catalog::Entry* ce = cat_->find("CAbil", e.ability); ce && ce->tag == "CAbilRally" &&
                    std::find(link_off.begin(), link_off.end(), e.ability) == link_off.end())
                    live.push_back(e.slot);
            auto is_live = [&](int s) { return std::find(live.begin(), live.end(), s) != live.end(); };
            for (int i = 0; i < CommandCard::kSlots; ++i)
                if (card.slots[i].state != ButtonState::Empty && !card.slots[i].is_cancel && !is_live(i))
                    card.slots[i].state = ButtonState::Disabled;
            auto grey = [&](int s) { return !is_live(s); };
            builds.clear();
            trains.clear();
            submenus.clear();
            abils.erase(std::remove_if(abils.begin(), abils.end(), [&](const AbilityCardEntry& e) { return grey(e.slot); }),
                        abils.end());
            clear = false;
        }
        if (clear) {
            for (int i = 0; i < CommandCard::kSlots; ++i)
                if (!(keep_cancel && card.slots[i].is_cancel)) card.slots[i] = CommandButton{};
            auto gone = [&](int s) { return s < 0 || s >= CommandCard::kSlots || card.slots[s].state == ButtonState::Empty; };
            builds.erase(std::remove_if(builds.begin(), builds.end(), [&](const BuildCardEntry& e) { return gone(e.slot); }), builds.end());
            abils.erase(std::remove_if(abils.begin(), abils.end(), [&](const AbilityCardEntry& e) { return gone(e.slot); }), abils.end());
            trains.erase(std::remove_if(trains.begin(), trains.end(), [&](const TrainCardEntry& e) { return gone(e.slot); }), trains.end());
            submenus.erase(std::remove_if(submenus.begin(), submenus.end(), [&](const SubmenuCardEntry& e) { return gone(e.slot); }),
                           submenus.end());
        }
    }
    if (cat_ && !selection_.empty() && selection_[0] < world_.units().size()) {
        static const bool kHaltOld = std::getenv("OMC_HALT_IDLE_OLD") != nullptr;
        static const bool kRallyOld = std::getenv("OMC_RALLY_MOBILE_OLD") != nullptr;
        static const bool kToggleOld = std::getenv("OMC_TOGGLE_PAIR_OLD") != nullptr;
        static const bool kRedirectOld = std::getenv("OMC_REDIRECT_EMPTY_OLD") != nullptr;
        static const bool kEffectCancelOld = std::getenv("OMC_EFFECT_CANCEL_IDLE_OLD") != nullptr;
        static const bool kAbilArrayOld = std::getenv("OMC_CARD_ABILARRAY_OLD") != nullptr;
        const std::uint32_t lead_id = selection_[0];
        const sim::Unit& lu = world_.unit(lead_id);
        const bool building = lu.constructing || lu.warp_forming;
        const bool mobile = lu.speed.raw() != 0 || lu.flying;
        std::vector<std::string> carried;
        if (!kAbilArrayOld) carried = cat_->array_links("CUnit", card_unit_id(lu), "AbilArray");
        std::vector<int> drop;
        for (const AbilityCardEntry& e : abils) {
            if (e.slot < 0 || e.slot >= CommandCard::kSlots || card.slots[e.slot].is_cancel) continue;
            const data::Catalog::Entry* ce = cat_->find("CAbil", e.ability);
            if (!ce) continue;
            if (!kAbilArrayOld && !carried.empty() && ce->tag == "CAbilRally" &&
                std::find(carried.begin(), carried.end(), e.ability) == carried.end())
                drop.push_back(e.slot);
            if (!kRedirectOld && (ce->tag == "CAbilRedirectInstant" || ce->tag == "CAbilRedirectTarget") &&
                world_.cargo_used(lead_id) <= 0)
                drop.push_back(e.slot);
            if (!kEffectCancelOld && e.command == "Cancel" && ce->tag.rfind("CAbilEffect", 0) == 0 &&
                !effect_cancel_live(lead_id))
                drop.push_back(e.slot);
            if (!kHaltOld && (ce->tag == "CAbilBuildable" || ce->tag == "CAbilBuildInProgress") && !building)
                drop.push_back(e.slot);
            if (!kRallyOld && ce->tag == "CAbilRally" && mobile && !building) drop.push_back(e.slot);
            if (!kToggleOld && ce->tag == "CAbilMorph") {
                const auto ms = cat_->morph_spec(e.ability);
                if (ms && !ms->target.empty() && ms->target == card_unit_id(lu)) drop.push_back(e.slot);
            }
            if (!kToggleOld && ce->tag == "CAbilTransport" && e.command.rfind("Unload", 0) == 0 &&
                world_.cargo_used(selection_[0]) <= 0)
                drop.push_back(e.slot);
            if (!kToggleOld && ce->tag == "CAbilBehavior" && (e.command == "On" || e.command == "Off")) {
                bool active = false;
                for (const std::string& beh : cat_->array_links("CAbil", e.ability, "BehaviorArray")) {
                    const std::uint32_t h = behavior_id_hash(beh);
                    for (const sim::BehaviorInstance& bi : lu.behaviors)
                        if (bi.src == h || bi.present_src == h) active = true;
                    if (!cat_->behavior_weapon_enable(beh).empty() && lu.toggle_wpn_on) active = true;
                }
                if (lu.has_toggle_creep && lu.creep_gen_on) active = true;
                if (lu.cloaked && lu.cloak_drain.raw() > 0) active = true;
                if ((e.command == "Off") != active) drop.push_back(e.slot);
            }
        }
        for (int s : drop) card.slots[s] = CommandButton{};
        if (!drop.empty())
            abils.erase(std::remove_if(abils.begin(), abils.end(),
                                       [&](const AbilityCardEntry& e) {
                                           return std::find(drop.begin(), drop.end(), e.slot) != drop.end();
                                       }),
                        abils.end());
    }
    return card;
}

std::optional<TooltipInfo> MeleeGame::hovered_card_tooltip(std::uint32_t view_w, std::uint32_t view_h,
                                                           float mx, float my,
                                                           const ConsolePanel* console) {
    if (cfg_.human_player < 0 || !view_w || !view_h) return std::nullopt;
    static const bool kComposeAlways = std::getenv("OMC_CARD_TOOLTIP_COMPOSE_ALWAYS") != nullptr;
    if (!kComposeAlways) {
        bool over_cell = false;
        for (int i = 0; i < CommandCard::kSlots && !over_cell; ++i) {
            const HudRect c = hud_command_cell(view_w, view_h, i, console);
            over_cell = c.w() > 0 && mx >= float(c.x0) && mx < float(c.x1) && my >= float(c.y0) && my < float(c.y1);
        }
        if (!over_cell) return std::nullopt;
    }
    std::vector<BuildCardEntry> builds;
    std::vector<AbilityCardEntry> abils;
    std::vector<TrainCardEntry> trains;
    std::vector<SubmenuCardEntry> submenus;
    const CommandCard card = compose_command_card_cached(builds, abils, trains, submenus);
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
                if (cat_)
                    if (auto d = cat_->button_tooltip(e.unit)) t.description = std::move(*d);
                break;
            }
        for (const BuildCardEntry& e : builds)
            if (e.slot == i) {
                t.minerals = e.minerals;
                t.gas = e.gas;
                t.build_time = float(e.time.to_double());
                if (cat_)
                    if (auto d = cat_->button_tooltip(e.unit)) t.description = std::move(*d);
                break;
            }
        for (const AbilityCardEntry& e : abils)
            if (e.slot == i) {
                if (cat_ && !e.face.empty())
                    if (auto d = cat_->button_tooltip(e.face)) t.description = std::move(*d);
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
        world_.pending_at_producer(producer, st.at_unit_queued, st.at_unit);
        for (const sim::BehaviorInstance& b : p.behaviors) {
            if (b.src == 0) continue;
            const std::string id = behavior_id_of_tag(b.src);
            if (id.empty()) continue;
            ++st.at_unit[id];
            ++st.behaviors[id];
        }
        static const bool kUnitBehReqOld = std::getenv("OMC_UNIT_BEHAVIOR_REQ_OLD") != nullptr;
        if (!kUnitBehReqOld && p.alive && !p.catalog_id.empty())
            for (const std::string& b : cat_->array_links("CUnit", p.catalog_id, "BehaviorArray")) {
                if (b.empty() || st.behaviors.count(b) || !cat_->find("CBehavior", b)) continue;
                if (!cat_->array_links("CBehavior", b, "RemoveValidatorArray").empty()) continue;
                bool enabled = true;
                for (const std::string& v : cat_->array_links("CBehavior", b, "DisableValidatorArray")) {
                    const data::Catalog::Entry* ve = v.empty() ? nullptr : cat_->find("CValidator", v);
                    std::string where = "SourceUnit";
                    if (ve)
                        for (const core::XmlNode& n : cat_->resolved("CValidator", v))
                            if (n.name == "WhichLocation") where = n.attr_or("Value", n.attr_or("value", where));
                    if (!ve || ve->tag != "CValidatorLocationCreep" || where != "SourceUnit") {
                        enabled = false;
                        break;
                    }
                    const bool want = cat_->scalar("CValidator", v, "Find").value_or("1") != "0";
                    if (world_.creep().is_creep(p.x, p.y) != want) enabled = false;
                }
                if (!enabled) continue;
                ++st.at_unit[b];
                ++st.behaviors[b];
            }
    }
    return st;
}

void MeleeGame::wire_addon_bodies() {
    if (cat_) world_.set_addon_body_factory(addon_body_factory(*cat_));
}

void MeleeGame::enable_ai_tech_gate(const data::Catalog& cat) {
    cat_ = &cat;
    world_.set_requirement_gate([this](std::int32_t player, const std::string& req_id) {
        if (!cat_ || player < 0) return true;
        return data::eval_requirement(*cat_, req_id, "Use", tech_state_of(player));
    });
    world_.set_train_gate([this](std::uint32_t producer, const std::string& unit_id, bool at_site) {
        if (!cat_ || producer >= world_.units().size()) return true;
        const sim::Unit& p = world_.units()[producer];
        if (p.catalog_id.empty()) return true;
        if (!train_gate_strict_) {
            if (p.player == cfg_.human_player) return true;
            for (const data::Catalog::TrainProduct& tp : cat_->unit_train_products(p.catalog_id))
                if (tp.unit == unit_id)
                    return production_.can_produce(*cat_, tp.ability, unit_id, tech_state_of(p.player, producer));
            return true;
        }
        const std::string ability = train_ability_listed(producer, unit_id, at_site);
        if (ability.empty()) return false;
        return production_.can_produce(*cat_, ability, unit_id, tech_state_of(p.player, producer));
    });
}

std::string MeleeGame::train_ability_listed(std::uint32_t producer, const std::string& unit_id, bool at_site) const {
    if (!cat_ || producer >= world_.units().size()) return {};
    const sim::Unit& p = world_.units()[producer];
    auto in = [&](const std::vector<data::Catalog::TrainProduct>& ps) -> std::string {
        for (const data::Catalog::TrainProduct& tp : ps)
            if (tp.unit == unit_id) return tp.ability;
        return {};
    };
    std::string a = in(cat_->unit_train_products(p.catalog_id));
    if (a.empty()) a = in(cat_->unit_train_products(card_unit_id(p)));
    if (a.empty()) a = in(cat_->unit_warp_products(p.catalog_id));
    if (a.empty())
        if (const auto sb = cat_->unit_spawn_behavior(p.catalog_id))
            if (!sb->unit.empty()) a = in(cat_->unit_train_products(sb->unit));
    if (a.empty() && at_site && !hall_needs_builder_) {
        std::set<std::string> seen;
        for (const sim::Unit& w : world_.units()) {
            if (!w.alive || w.player != p.player || !w.is_worker || w.catalog_id.empty()) continue;
            if (!seen.insert(w.catalog_id).second) continue;
            for (const data::Catalog::BuildProduct& bp : cat_->unit_build_products(w.catalog_id))
                if (bp.unit == unit_id) return bp.ability;
        }
    }
    return a;
}

const std::vector<std::pair<std::string, std::string>>& MeleeGame::upgrade_model_swaps(const std::string& unit_id) const {
    auto it = upgrade_model_swaps_.find(unit_id);
    if (it != upgrade_model_swaps_.end()) return it->second;
    std::vector<std::pair<std::string, std::string>> out;
    if (actor_cat_) {
        if (const data::CActorUnit* a = actor_cat_->actor_for_unit(unit_id)) {
            for (const data::ActorRule& r : a->on) {
                if (r.verb != "ModelSwap" || r.args.empty() || r.event_head != "Upgrade") continue;
                const std::size_t dot = r.event.find('.');
                if (dot == std::string::npos) continue;
                const std::size_t end = r.event.rfind('.');
                if (end <= dot || r.event.compare(end, std::string::npos, ".Add") != 0) continue;
                out.emplace_back(r.event.substr(dot + 1, end - dot - 1), r.args[0]);
            }
        }
    }
    return upgrade_model_swaps_.emplace(unit_id, std::move(out)).first->second;
}

static const std::string& option_model_swap(const data::ActorCatalog* ac, const std::string& unit_id) {
    static std::map<std::string, std::string> cache;
    auto it = cache.find(unit_id);
    if (it != cache.end()) return it->second;
    static const std::string level = std::getenv("OMC_SHADER_OPTION") ? std::getenv("OMC_SHADER_OPTION") : "Extreme";
    std::string out;
    if (ac)
        if (const data::CActorUnit* a = ac->actor_for_unit(unit_id))
            for (const data::ActorRule& r : a->on) {
                if (r.verb != "ModelSwap" || r.args.empty() || r.event_head != "ActorCreation") continue;
                for (const std::string& f : r.filters) {
                    const std::size_t b = f.find_first_not_of(' '), e = f.find_last_not_of(' ');
                    if (b != std::string::npos && f.substr(b, e - b + 1) == "Option Shader " + level) out = r.args[0];
                }
                if (std::getenv("OMC_SHADER_OPTION_TRACE"))
                    std::fprintf(stderr, "[optswap] %s rule %s | %s -> %s\n", unit_id.c_str(), r.terms.c_str(), r.send.c_str(), out.c_str());
            }
    if (std::getenv("OMC_SHADER_OPTION_TRACE"))
        std::fprintf(stderr, "[optswap] %s actor %s -> '%s'\n", unit_id.c_str(), (ac && ac->actor_for_unit(unit_id)) ? "found" : "none", out.c_str());
    return cache.emplace(unit_id, std::move(out)).first->second;
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
    bool queue_closed = false;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing || u.catalog_id.empty()) continue;
        if (morph_blocks_queue(id)) {
            if (std::getenv("OMC_MORPH_CARD_CANCEL_ONLY")) break;
            queue_closed = true;
        }
        std::vector<data::Catalog::TrainProduct> ps = cat_->unit_train_products(card_unit_id(u));
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
        data::UnitCost c = data::unit_cost(*cat_, tp.unit);
        if (!std::getenv("OMC_TRAIN_PAIR_SINGLE"))
            if (const auto row = cat_->ability_info_row(tp.ability, tp.command); row && row->unit_count > 1) {
                c.minerals *= row->unit_count;
                c.gas *= row->unit_count;
            }
        e.minerals = c.minerals;
        e.gas = c.gas;
        const double t = folded_train_time(tp.ability, tp.command, tp.time);
        e.time = t > 0 ? sim::Fixed::from_double(t) : sim::Fixed();
        const bool affordable = hp.minerals >= c.minerals && hp.gas >= c.gas;
        const bool req_ok = production_.can_produce(*cat_, tp.ability, tp.unit, st);
        e.tech_met = req_ok;
        e.requirement_met = affordable && e.tech_met && !queue_closed;
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
    if (morph_blocks_queue(producer)) return false;
    sim::Fixed time;
    bool trains_it = false;
    std::string train_ability;
    int train_count = 1;
    for (const data::Catalog::TrainProduct& tp : cat_->unit_train_products(p.catalog_id))
        if (tp.unit == unit_id) {
            const double ft = folded_train_time(tp.ability, tp.command, tp.time);
            time = ft > 0 ? sim::Fixed::from_double(ft) : sim::Fixed();
            train_ability = tp.ability;
            if (!std::getenv("OMC_TRAIN_PAIR_SINGLE"))
                if (const auto row = cat_->ability_info_row(tp.ability, tp.command)) train_count = row->unit_count;
            trains_it = true;
            break;
        }
    if (!trains_it) return false;
    if (unpowered_offer_blocked(producer, train_ability)) return false;
    if (!production_.can_produce(*cat_, train_ability, unit_id, human_tech_state(producer)))
        return false;
    const data::UnitCost c = data::unit_cost(*cat_, unit_id);
    sim::Unit proto = make_unit(*cat_, unit_id, cfg_.human_player);
    return MeleeCommands::train(world_, producer, proto, c.minerals, c.gas, time, MeleeCommands::kPendingCap,
                                train_count);
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
        for (data::Catalog::BuildProduct& bp : cat_->unit_build_products(card_unit_id(u))) {
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
        if (auto hk = cat_->button_hotkey(bp.face)) e.hotkey = std::move(*hk);
        if (bp.row >= 0 && bp.col >= 0) e.slot = bp.row * 5 + bp.col;
        const data::UnitCost c = data::build_charge(*cat_, bp.unit, world_.units()[worker].catalog_id, bp.ability);
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
        for (const auto& c : cat_->unit_command_cards(card_unit_id(u)))
            if (int(c.index) == page) return true;
        return false;
    }
    return false;
}

static int timed_ladder_stage(const formats::Model& model, const std::vector<std::vector<std::string>>& ladder, double p,
                              double total_ms, int fallback) {
    static const bool kEqualStages = std::getenv("OMC_PROGRESS_EQUAL_STAGES") != nullptr;
    if (kEqualStages || !(total_ms > 0.0) || ladder.empty()) return fallback;
    std::vector<double> len(ladder.size(), -1.0);
    double fixed = 0.0;
    int stands = 0;
    for (std::size_t si = 0; si < ladder.size(); ++si) {
        const std::vector<std::string>& sp = ladder[si];
        const bool stand = std::find(sp.begin(), sp.end(), "Stand") != sp.end();
        const int sq = stand ? -1 : render::select_sequence(model, sp, 0);
        if (!stand && sq >= 0 && std::size_t(sq) < model.sequences.size()) {
            len[si] = double(model.sequences[std::size_t(sq)].length_ms());
            fixed += len[si];
        } else {
            ++stands;
        }
    }
    if (stands <= 0 || fixed >= total_ms) return fallback;
    const double each = (total_ms - fixed) / double(stands);
    double at = 0.0;
    const double now = p * total_ms;
    for (std::size_t si = 0; si < ladder.size(); ++si) {
        at += len[si] >= 0.0 ? len[si] : each;
        if (now < at) return int(si);
    }
    return int(ladder.size()) - 1;
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
        const auto cards = cat_->unit_command_cards(card_unit_id(u));
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
    const double rate = cat_->warp_progress_rate(unit_id, world_.power_level_at(tx, ty, cfg_.human_player));
    const sim::Fixed forming =
        found->time > 0 ? sim::Fixed::from_double(found->time / (rate > 0 ? rate : 1.0)) : sim::Fixed();
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
    static const bool kRemoteOld = std::getenv("OMC_NYDUS_REMOTE_OLD") != nullptr;
    const bool remote = !kRemoteOld && !instant && !w.is_worker && w.build_charges < 0 && w.speed.raw() == 0;
    const bool charged = !instant && !w.is_worker && (w.build_charges >= 0 || remote);
    if (!instant && !w.is_worker && !charged) return false;
    if (charged && !remote && !world_.build_charge_ready(worker)) return false;
    if (!production_.can_produce(*cat_, build_ability, structure_unit_id, human_tech_state(worker)))
        return false;
    pending_attack_ = pending_patrol_ = pending_move_ = false;
    cancel_ability_cast();
    if (instant) {
        const std::int32_t kind = addon_kind_of(*cat_, structure_unit_id);
        if (kind == sim::World::kAddonNone) return false;
        if (w.flying && !std::getenv("OMC_LAND_CARD_OLD")) {
            if (std::getenv("OMC_FLY_ADDON_REFUSED") || w.lift_lock.raw() > 0) return false;
            const data::UnitCost fc = data::unit_cost(*cat_, structure_unit_id);
            const sim::Player& fp = world_.player(cfg_.human_player);
            if (fp.minerals < fc.minerals || fp.gas < fc.gas) return false;
            pending_land_ = true;
            pending_land_unit_ = worker;
            pending_land_addon_ = structure_unit_id;
            pending_defender_ = false;
            return true;
        }
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
        {
            const data::UnitCost bc = data::build_charge(*cat_, structure_unit_id, w.catalog_id, build_ability);
            build_cost_ = bc.minerals;
            build_gas_cost_ = bc.gas;
        }
        build_time_ = time;
        return true;
    }
    pending_build_ = true;
    build_caster_ = charged ? worker : sim::kNoTarget;
    build_proto_ = make_unit(*cat_, structure_unit_id, cfg_.human_player);
    build_proto_.speed = sim::Fixed();
    {
        const data::UnitCost bc = data::build_charge(*cat_, structure_unit_id, w.catalog_id, build_ability);
        build_cost_ = bc.minerals;
        build_gas_cost_ = bc.gas;
    }
    build_time_ = time;
    return true;
}

bool MeleeGame::human_land_addon(std::uint32_t host, sim::Fixed x, sim::Fixed y, const std::string& addon) {
    if (!cat_ || host >= world_.units().size()) return false;
    const sim::Unit& u = world_.units()[host];
    if (!u.alive || !u.flying || u.player != cfg_.human_player) return false;
    const std::int32_t kind = addon_kind_of(*cat_, addon);
    if (kind == sim::World::kAddonNone) return false;
    sim::Fixed time;
    bool found = false;
    for (const data::Catalog::BuildProduct& bp : cat_->unit_build_products(u.catalog_id))
        if (bp.unit == addon && bp.instant_placement) {
            time = bp.time > 0 ? sim::Fixed::from_double(bp.time) : sim::Fixed();
            found = true;
            break;
        }
    if (!found) return false;
    if (!land_site(host, x, y)) return false;
    const data::UnitCost c = data::unit_cost(*cat_, addon);
    const sim::Player& hp = world_.player(cfg_.human_player);
    if (hp.minerals < c.minerals || hp.gas < c.gas) return false;
    return world_.order_land_addon(host, x, y, kind, time, c.minerals, c.gas);
}

std::optional<data::Catalog::MorphSpec> MeleeGame::player_morph_spec(std::int32_t player, const std::string& abil) const {
    if (!cat_) return std::nullopt;
    const data::UpgradeCatalog* up = research_ ? research_->upgrades() : nullptr;
    if (!morph_upgrade_fold_ || !up || player < 0) return cat_->morph_spec(abil);
    const std::vector<std::string>& done = world_.researched(player);
    if (done.empty()) return cat_->morph_spec(abil);
    return cat_->morph_spec(abil, [&](const std::string& field, double base) {
        return data::fold_upgrade_whole_field(*up, done, "Abil", abil, field, base);
    });
}

sim::Unit MeleeGame::research_body(const std::string& unit_id, std::int32_t player) const {
    const data::UpgradeCatalog* up = research_ ? research_->upgrades() : nullptr;
    if (!upgrade_fold_new_ || !up || player < 0) return make_unit(*cat_, unit_id, player);
    const std::vector<std::string>& done = world_.researched(player);
    sim::Unit u = make_unit_upgraded(*cat_, *up, unit_id, player, done);
    u.upgrades_folded = std::uint32_t(done.size());
    return u;
}

bool MeleeGame::human_burrow(std::uint32_t id, bool down) {
    if (!cat_ || id >= world_.units().size()) return false;
    const sim::Unit& u = world_.units()[id];
    if (!u.alive || u.burrowed == down || world_.is_burrowing(id)) return false;
    if (!timed_burrow_) {
        sim::set_burrowed(world_, id, down);
        return true;
    }
    std::string abil = cat_->burrow_ability(u.catalog_id);
    if (!down) {
        const std::string variant = cat_->burrowed_variant(u.catalog_id);
        abil = variant.empty() ? std::string() : cat_->unburrow_ability(variant);
    }
    const auto ms = abil.empty() ? std::nullopt : player_morph_spec(u.player, abil);
    if (!ms || ms->transient) {
        sim::set_burrowed(world_, id, down);
        return true;
    }
    return world_.begin_burrow(id, down, sim::Fixed::from_double(ms->stats_delay),
                               sim::Fixed::from_double(ms->order_time));
}

bool MeleeGame::human_depot(std::uint32_t id, bool down) {
    if (!cat_ || id >= world_.units().size()) return false;
    const sim::Unit& u = world_.units()[id];
    if (!u.alive || u.lowered == down || world_.is_depot_morphing(id)) return false;
    if (!timed_depot_) return down ? world_.lower(id) : world_.raise(id);
    const std::string lower = cat_->depot_lower_ability(u.catalog_id);
    std::string abil = lower;
    if (!down && !lower.empty())
        if (const auto ms = cat_->morph_spec(lower)) abil = cat_->morph_button_ability(ms->target, "Raise");
    const auto ms = abil.empty() ? std::nullopt : player_morph_spec(u.player, abil);
    if (!ms) return down ? world_.lower(id) : world_.raise(id);
    return world_.begin_depot_morph(id, down, sim::Fixed::from_double(ms->collide_delay),
                                    sim::Fixed::from_double(ms->order_time));
}

bool MeleeGame::human_morph(std::uint32_t caster, const std::string& morph_abil) {
    if (!cat_ || morph_abil.empty() || caster >= world_.units().size()) return false;
    const sim::Unit& u = world_.units()[caster];
    const std::int32_t mp = morph_as_player_ >= 0 ? morph_as_player_ : cfg_.human_player;
    if (!u.alive || u.player != mp) return false;
    const auto ms = player_morph_spec(mp, morph_abil);
    if (!ms || ms->target.empty()) return false;
    auto delta = morph_charge(*cat_, u.catalog_id, morph_abil);
    sim::Player& hp = world_.player(mp);
    if (hp.minerals < delta.first || hp.gas < delta.second) return false;
    if (morph_start_blocked(caster, morph_abil)) return false;
    const std::int32_t proto = world_.add_spawn_proto(research_body(ms->target, mp));
    world_.clear_zone(caster);
    const double lock_s = morph_lock_seconds(*ms, morph_lock_order_);
    const sim::Fixed t = lock_s > 0 ? sim::Fixed::from_double(lock_s) : sim::Fixed();
    const std::string fin = cat_->morph_finish_behavior(morph_abil);
    const std::uint32_t fin_tag =
        (!fin.empty() && cat_->behavior_is_permanent(fin)) ? behavior_id_hash(fin) : 0u;
    std::vector<sim::MorphStep> steps;
    if (morph_swap_at_data_) {
        std::string last_unit;
        std::int32_t last_proto = -1;
        for (const MorphStepPlan& sp : morph_step_plan(*ms, lock_s)) {
            if (sp.unit != last_unit) {
                last_unit = sp.unit;
                last_proto = sp.unit == ms->target ? proto : world_.add_spawn_proto(research_body(sp.unit, mp));
            }
            steps.push_back({sp.at, last_proto, sp.type, sp.vitals});
        }
    }
    world_.begin_morph(caster, proto, t, fin_tag, std::move(steps),
                       morph_extras(*cat_, *ms, morph_start_delay_, morph_behaviors_sim_));
    if (!world_.is_morphing(caster)) return false;
    morph_abil_[caster] = morph_abil;
    morph_paid_[caster] = delta;
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

int MeleeGame::run_morph_autocast() {
    static const bool kOld = std::getenv("OMC_MORPH_AUTOCAST_OLD") != nullptr;
    if (kOld || !cat_) return 0;
    auto key = [](std::uint32_t id, const std::string& a) { return std::to_string(id) + "|" + a; };
    auto set_off = [&](std::uint32_t id, const std::string& a) {
        if (cast_policy_) cast_policy_->set_autocast(id, a, false);
        else morph_autocast_off_[key(id, a)] = true;
    };
    auto is_on = [&](std::uint32_t id, const std::string& a) {
        if (cast_policy_) return cast_policy_->autocast_state(world_, id, a);
        return !morph_autocast_off_.count(key(id, a)) && cat_->ability_autocast(a).on_by_default;
    };
    int started = 0;
    const std::size_t n = world_.units().size();
    for (std::uint32_t id = 0; id < n; ++id) {
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player < 0 || u.catalog_id.empty()) {
            morph_autocast_was_morphing_.erase(id);
            continue;
        }
        if (world_.is_morphing(id)) {
            morph_autocast_was_morphing_[id] = true;
            continue;
        }
        if (const auto w = morph_autocast_was_morphing_.find(id); w != morph_autocast_was_morphing_.end()) {
            morph_autocast_was_morphing_.erase(w);
            if (const auto m = morph_abil_.find(id); m != morph_abil_.end())
                for (const std::string& a : cat_->morph_finish_autocast_off(m->second)) set_off(id, a);
        }
        if (u.constructing || u.loaded || !u.orders.empty()) continue;
        auto it = morph_autocast_cache_.find(u.catalog_id);
        if (it == morph_autocast_cache_.end()) {
            std::vector<MorphAutocast> v;
            for (const std::string& a : cat_->array_links("CUnit", u.catalog_id, "AbilArray")) {
                const data::Catalog::Entry* e = cat_->find("CAbil", a);
                if (!e || e->tag != "CAbilMorph" || !cat_->ability_autocast(a).offered) continue;
                MorphAutocast mc;
                mc.abil = a;
                mc.requirement = cat_->button_requirement(a, "Execute");
                AbilityBuild b;
                parse_ability_autocast_validators(*cat_, a, b);
                mc.not_behaviors = b.autocast_not_behaviors;
                v.push_back(std::move(mc));
            }
            it = morph_autocast_cache_.emplace(u.catalog_id, std::move(v)).first;
        }
        if (it->second.empty()) continue;
        sim::Fixed f;
        if (world_.pending_progress(id, f) || world_.queued_count(id) > 0) continue;
        for (const MorphAutocast& mc : it->second) {
            if (!is_on(id, mc.abil)) continue;
            bool blocked = false;
            for (std::uint32_t tag : mc.not_behaviors)
                for (const sim::BehaviorInstance& b : u.behaviors)
                    if (b.src == tag) blocked = true;
            if (blocked) continue;
            if (!mc.requirement.empty() &&
                !data::eval_requirement(*cat_, mc.requirement, "Use", tech_state_of(u.player, id)))
                continue;
            morph_as_player_ = u.player;
            const bool ok = human_morph(id, mc.abil);
            morph_as_player_ = -1;
            if (ok) {
                morph_autocast_was_morphing_[id] = true;
                ++started;
                break;
            }
        }
    }
    return started;
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
    begin_research(cfg_.human_player, upgrade, sim::Fixed::from_double(info.time),
                   std::getenv("OMC_RESEARCH_PANE_OLD") ? sim::kNoTarget : producer, info.minerals, info.gas);
    return true;
}

double MeleeGame::player_abil_field(std::int32_t player, const std::string& abil, const std::string& field,
                                   double base) const {
    const data::UpgradeCatalog* up = research_ ? research_->upgrades() : nullptr;
    if (!abil_upgrade_fold_ || !up || player < 0) return base;
    const std::vector<std::string>& done = world_.researched(player);
    if (done.empty()) return base;
    return data::fold_upgrade_whole_field(*up, done, "Abil", abil, field, base);
}

void MeleeGame::wire_cast_folds() {
    if (!cast_policy_) return;
    cast_policy_->set_button_gate([this](std::uint32_t caster, const std::string& req) {
        if (!cat_ || caster >= world_.units().size()) return true;
        const data::TechState st = tech_state_of(world_.units()[caster].player, caster);
        return data::eval_requirement(*cat_, req, "Use", st) && !data::requirement_hidden(*cat_, req, st);
    });
    cast_policy_->set_cooldown_fold([this](std::int32_t player, const std::string& abil, sim::Fixed base) {
        const double b = base.to_double();
        double v = player_abil_field(player, abil, "Cost[0].Cooldown.TimeUse", b);
        v = player_abil_field(player, abil, "Cost.Cooldown.TimeUse", v);
        return v == b ? base : sim::Fixed::from_double(v);
    });
    world_.set_cast_cooldown_fold([this](const sim::Unit& u, std::int32_t ability, sim::Fixed base) {
        if (!cast_policy_) return base;
        const std::string& abil = cast_policy_->abil_of_effect(ability);
        return abil.empty() ? base : cast_policy_->player_cooldown(world_, u.id, abil, base);
    });
}

void MeleeGame::step_defender_approach() {
    for (auto it = defender_approach_.begin(); it != defender_approach_.end();) {
        const std::uint32_t id = it->first;
        const DefenderApproach a = it->second;
        if (id >= world_.units().size() || !world_.units()[id].alive) {
            it = defender_approach_.erase(it);
            continue;
        }
        const sim::Unit& u = world_.units()[id];
        const sim::Fixed dx = a.x - u.x, dy = a.y - u.y;
        const sim::Fixed reach = a.range + u.radius;
        if (dx * dx + dy * dy <= reach * reach) {
            it = defender_approach_.erase(it);
            world_.unit(id).orders.clear();
            siege_defender_now(id, a.x, a.y);
            continue;
        }
        if (u.orders.empty() || u.orders.front().type != sim::OrderType::Move || u.orders.front().tx != a.ox ||
            u.orders.front().ty != a.oy) {
            it = defender_approach_.erase(it);
            continue;
        }
        ++it;
    }
}

bool MeleeGame::human_siege_defender(std::uint32_t caster, sim::Fixed cx, sim::Fixed cy) {
    defender_approach_.erase(caster);
    if (defender_range_ && cat_ && caster < world_.units().size()) {
        const sim::Unit& u = world_.units()[caster];
        const std::int32_t mp = morph_as_player_ >= 0 ? morph_as_player_ : cfg_.human_player;
        if (!u.alive || u.player != mp) return false;
        const char* const target_abil = "LiberatorAGTarget";
        const double base = cat_->scalar_num("CAbil", target_abil, "Range").value_or(0.0);
        const double r = player_abil_field(u.player, target_abil, "Range[0]", base);
        if (r > 0.0) {
            const sim::Fixed range = sim::Fixed::from_double(r);
            const sim::Fixed dx = cx - u.x, dy = cy - u.y;
            const sim::Fixed reach = range + u.radius;
            if (dx * dx + dy * dy > reach * reach) {
                world_.issue(caster, sim::Order{sim::OrderType::Move, cx, cy, sim::kNoTarget});
                if (world_.units()[caster].orders.empty()) return false;
                const sim::Order& o = world_.units()[caster].orders.front();
                defender_approach_[caster] = DefenderApproach{cx, cy, range, o.tx, o.ty};
                return true;
            }
        }
    }
    return siege_defender_now(caster, cx, cy);
}

bool MeleeGame::siege_defender_now(std::uint32_t caster, sim::Fixed cx, sim::Fixed cy) {
    const std::string from = caster < world_.units().size() ? world_.units()[caster].catalog_id : std::string();
    if (!human_morph(caster, "LiberatorMorphtoAG")) return false;
    world_.set_zone(caster, cx, cy, sim::Fixed::from_int(5));
    if (zone_markers_) zone_marker_[caster] = {zone_marker_of(from), world_.tick()};
    if (std::getenv("OMC_MORPH_TRACE"))
        std::fprintf(stderr, "[morph] zone of %s at (%.2f, %.2f): marker '%s'\n", from.c_str(), cx.to_double(), cy.to_double(),
                     zone_markers_ ? zone_marker_[caster].first.c_str() : "(off)");
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

int MeleeGame::select_rank(const sim::Unit& u) const {
    if (!cat_ || u.catalog_id.empty()) return 0;
    const auto p = cat_->unit_subgroup_priority(u.catalog_id);
    return p ? *p : 0;
}

std::uint32_t MeleeGame::click_select_at(const Control& ctrl, const ScreenPoint& p) const {
    const std::uint32_t hit = ctrl.click_pick(world_, p);
    if (hit != sim::kNoTarget) return hit;
    auto hidden = [&](std::uint32_t id) {
        const sim::Unit& u = world_.units()[id];
        return u.player != cfg_.human_player && (u.cloaked || u.burrowed) && !world_.revealed_to(cfg_.human_player, id);
    };
    std::vector<std::uint32_t> box = ctrl.box_select(world_, ScreenRect{p.x - 6.0f, p.y - 6.0f, p.x + 6.0f, p.y + 6.0f}, false);
    box.erase(std::remove_if(box.begin(), box.end(), [&](std::uint32_t id) { return ctrl.body_has_volumes(id) || hidden(id); }),
              box.end());
    if (!box.empty()) {
        std::uint32_t best = box[0];
        sim::Fixed wx, wy;
        if (box.size() > 1 && ctrl.ground_point(world_, p, wx, wy)) {
            sim::Fixed best_d2;
            bool first = true;
            for (std::uint32_t id : box) {
                const sim::Unit& u = world_.units()[id];
                const sim::Fixed dx = u.x - wx, dy = u.y - wy, d2 = dx * dx + dy * dy;
                if (first || d2 < best_d2) { best = id; best_d2 = d2; first = false; }
            }
        }
        return best;
    }
    const std::uint32_t enemy = ctrl.pick_target(world_, p);
    if (enemy != sim::kNoTarget && !ctrl.body_has_volumes(enemy) && !hidden(enemy)) return enemy;
    return sim::kNoTarget;
}

bool MeleeGame::PickVolumesOf::volumes(std::uint32_t id, std::vector<PickVolume>& out) const {
    if (id >= game->pick_pose_.size()) return false;
    const PickPose& p = game->pick_pose_[id];
    if (!p.model || p.frame != game->pick_frame_) return false;
    const formats::Model& m = *p.model;
    if (!m.has_tight_hit && m.hit_fuzzy.empty()) return false;
    std::vector<render::Mat4> bones;
    if (p.seq >= 0 && std::size_t(p.seq) < m.sequences.size()) bones = render::pose_world(m, std::uint32_t(p.seq), p.time_ms);
    auto add = [&](const formats::M3HitShape& s, bool tight) {
        render::Mat4 local;
        for (int i = 0; i < 16; ++i) local.m[std::size_t(i)] = s.m[i];
        render::Mat4 bone = render::Mat4::identity();
        if (s.bone >= 0 && std::size_t(s.bone) < bones.size()) {
            bone = bones[std::size_t(s.bone)];
        } else if (s.bone >= 0 && m.inverse_bind.size() >= (std::size_t(s.bone) + 1) * 16) {
            render::Mat4 ib;
            for (int i = 0; i < 16; ++i) ib.m[std::size_t(i)] = m.inverse_bind[std::size_t(s.bone) * 16 + std::size_t(i)];
            if (!invert_mat4(ib, bone)) bone = render::Mat4::identity();
        }
        PickVolume v;
        v.shape = s.shape;
        v.tight = tight;
        for (int i = 0; i < 3; ++i) v.size[i] = s.size[i];
        if (!invert_mat4(p.world * bone * local, v.inv)) return;
        out.push_back(v);
    };
    if (m.has_tight_hit) add(m.hit_tight, true);
    for (const formats::M3HitShape& s : m.hit_fuzzy) add(s, false);
    return true;
}

bool MeleeGame::PickVolumesOf::may_hit(std::uint32_t id, const float* o, const float* d) const {
    if (!broadphase || id >= game->pick_pose_.size()) return true;
    const PickPose& p = game->pick_pose_[id];
    if (!p.model || p.frame != game->pick_frame_) return true;
    const formats::Model& m = *p.model;
    if (!m.has_tight_hit && m.hit_fuzzy.empty()) return true;
    const int seq = (p.seq >= 0 && std::size_t(p.seq) < m.sequences.size()) ? p.seq : -1;
    auto it = reach.find({p.model, seq});
    auto col_scale = [](const render::Mat4& t) {
        float s = 0.0f;
        for (int c = 0; c < 3; ++c)
            s = std::max(s, std::sqrt(t.m[std::size_t(c * 4)] * t.m[std::size_t(c * 4)] + t.m[std::size_t(c * 4 + 1)] * t.m[std::size_t(c * 4 + 1)] +
                                      t.m[std::size_t(c * 4 + 2)] * t.m[std::size_t(c * 4 + 2)]));
        return s;
    };
    if (it == reach.end()) {
        float r = 0.0f;
        auto measure = [&](const std::vector<render::Mat4>& bones) {
            auto one = [&](const formats::M3HitShape& s) {
                render::Mat4 local;
                for (int i = 0; i < 16; ++i) local.m[std::size_t(i)] = s.m[i];
                render::Mat4 bone = render::Mat4::identity();
                if (s.bone >= 0 && std::size_t(s.bone) < bones.size()) {
                    bone = bones[std::size_t(s.bone)];
                } else if (s.bone >= 0 && m.inverse_bind.size() >= (std::size_t(s.bone) + 1) * 16) {
                    render::Mat4 ib;
                    for (int i = 0; i < 16; ++i) ib.m[std::size_t(i)] = m.inverse_bind[std::size_t(s.bone) * 16 + std::size_t(i)];
                    if (!invert_mat4(ib, bone)) bone = render::Mat4::identity();
                }
                const render::Mat4 t = bone * local;
                const float lr = s.shape == 0 ? std::sqrt(s.size[0] * s.size[0] + s.size[1] * s.size[1] + s.size[2] * s.size[2])
                                 : s.shape == 1 ? s.size[0] : s.size[0] + 0.5f * std::fabs(s.size[1]);
                const float c = std::sqrt(t.m[12] * t.m[12] + t.m[13] * t.m[13] + t.m[14] * t.m[14]);
                r = std::max(r, c + lr * col_scale(t));
            };
            if (m.has_tight_hit) one(m.hit_tight);
            for (const formats::M3HitShape& s : m.hit_fuzzy) one(s);
        };
        measure({});
        if (seq >= 0) {
            const float len = float(m.sequences[std::size_t(seq)].length_ms());
            for (int k = 0; k <= 16; ++k) measure(render::pose_world(m, std::uint32_t(seq), len * float(k) / 16.0f));
        }
        it = reach.emplace(std::make_pair(p.model, seq), r * 1.5f + 0.5f).first;
    }
    const float R = it->second * col_scale(p.world);
    const float c[3] = {p.world.m[12], p.world.m[13], p.world.m[14]};
    const float q[3] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    float t = dd > 0.0f ? (q[0] * d[0] + q[1] * d[1] + q[2] * d[2]) / dd : 0.0f;
    if (t < 0.0f) t = 0.0f;
    const float e[3] = {o[0] + d[0] * t - c[0], o[1] + d[1] * t - c[1], o[2] + d[2] * t - c[2]};
    return e[0] * e[0] + e[1] * e[1] + e[2] * e[2] <= R * R;
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
            hud_covers_point(in.view_w, in.view_h, &console, in.mouse_x, in.mouse_y) || hud_buttons_.at(in.mouse_x, in.mouse_y);
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
    if (cam_right_ > cam_left_ && !OMC_ENV_ONCE("OMC_NO_CAMERA_MARGIN")) {
        static const float kMargin = std::getenv("OMC_CAMERA_MARGIN") ? float(std::atof(std::getenv("OMC_CAMERA_MARGIN"))) : 12.0f;
        const float lo = cam_left_ + kMargin, hi = cam_right_ - kMargin;
        if (hi > lo) camctl_.focus.x = std::clamp(camctl_.focus.x, lo, hi);
    }
    static const bool kCameraSouthOld = std::getenv("OMC_CAMERA_SOUTH_OLD") != nullptr;
    if (!kCameraSouthOld && cam_bottom_ > 0.0f) {
        constexpr float kSouthMargin = 4.0f;
        camctl_.focus.y = std::max(camctl_.focus.y, cam_bottom_ + kSouthMargin);
    }
    if (map_w_ > 0.0f && map_h_ > 0.0f) {
        float sum = 0.0f;
        int n = 0;
        if (!air_layer_.empty()) {
            camctl_.focus.z = air_z_at(camctl_.focus.x, camctl_.focus.y);
        } else {
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    sum += ground_z_at(camctl_.focus.x + float(dx), camctl_.focus.y + float(dy));
                    ++n;
                }
            camctl_.focus.z = sum / float(n);
        }
        if (const char* cf = OMC_ENV_ONCE("OMC_CAM_FOCUS")) {
            const float gz = ground_z_at(camctl_.focus.x, camctl_.focus.y);
            if (cf[0] == 't') {
                static float last_x = -1.0f, last_y = -1.0f;
                if (last_x != camctl_.focus.x || last_y != camctl_.focus.y) {
                    std::fprintf(stderr, "camfocus %.3f %.3f layer %.4f ground %.4f\n", camctl_.focus.x, camctl_.focus.y, camctl_.focus.z, gz);
                    last_x = camctl_.focus.x;
                    last_y = camctl_.focus.y;
                }
            } else if (cf[0] == 'a' && cf[1] == 'd' && cf[2] == 'd' && cf[3] == ':') {
                camctl_.focus.z += float(std::atof(cf + 4));
            } else {
                camctl_.focus.z = gz;
            }
        }
    }
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
        if (models_ && ui_store_ && cat_ && u.player >= 0 && u.speed.raw() == 0 && (u.attributes & sim::kAttrStructure) != 0 &&
            !std::getenv("OMC_NO_PLAYER_DECAL")) {
            if (const MeleeModels::GroundDecal* gd = models_->unit_ground_decal(*ui_store_, u.catalog_id)) {
                const float fl = std::sqrt(gd->forward[0] * gd->forward[0] + gd->forward[1] * gd->forward[1]);
                if (fl > 1e-6f) {
                    const float ux = float(u.x.to_double()), uy = float(u.y.to_double());
                    formats::PlacedDoodad dd{};
                    dd.rotation = std::atan2(gd->forward[1], gd->forward[0]) + 1.5707963f;
                    const float cr = std::cos(dd.rotation), sr = std::sin(dd.rotation);
                    dd.x = cr * gd->offset[0] - sr * gd->offset[1];
                    dd.y = sr * gd->offset[0] + cr * gd->offset[1];
                    dd.sx = dd.sy = dd.sz = gd->scale;
                    formats::Model only = build_projector_items(std::vector<ProjectorItem>{{&gd->model, dd}}, 0.0f,
                                                                [&](float x, float y) { return ground_z_at(ux + x, uy + y); });
                    if (only.vertex_count) return &splat_quads_.emplace(u.id, std::move(only)).first->second;
                }
            }
        }
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
    if (models_->unit_splat_from_actor(u.catalog_id) && !splat->projectors.empty() && !std::getenv("OMC_ACTOR_SPLAT_QUAD")) {
        const float ux = float(u.x.to_double()), uy = float(u.y.to_double());
        formats::PlacedDoodad d{};
        d.rotation = float(u.facing.to_double());
        d.sx = d.sy = d.sz = 1.0f;
        formats::Model bed = build_projector_items(std::vector<ProjectorItem>{{splat, d}}, 100.0f,
                                                   [&](float x, float y) { return ground_z_at(ux + x, uy + y); });
        if (bed.vertex_count) return &splat_quads_.emplace(u.id, std::move(bed)).first->second;
        splat_none_.insert(u.id);
        return nullptr;
    }
    static const bool kPadFootprint = std::getenv("OMC_SPLAT_PAD_FOOTPRINT") != nullptr;
    const bool burrow_ring = (u.id & 0x80000000u) != 0 && !std::getenv("OMC_BURROW_RING_FOOTPRINT");
    if (!kPadFootprint && cat_ && u.player >= 0 && ((u.speed.raw() == 0 && (u.attributes & sim::kAttrStructure) != 0) || burrow_ring) && !splat->projectors.empty()) {
        const formats::M3Projector& pj = splat->projectors.front();
        if (pj.x_right - pj.x_left > 0.05f && pj.y_back - pj.y_front > 0.05f) {
            const float ux = float(u.x.to_double()), uy = float(u.y.to_double());
            static const float kRotAdd = std::getenv("OMC_SPLAT_PAD_ROT_ADD") ? float(std::atof(std::getenv("OMC_SPLAT_PAD_ROT_ADD"))) * 0.01745329252f : 0.0f;
            formats::PlacedDoodad d{};
            d.rotation = float(cat_->unit_facing_radians(u.catalog_id)) + 1.5707963f + kRotAdd;
            d.sx = d.sy = d.sz = models_->unit_splat_scale(u.catalog_id);
            std::vector<ProjectorItem> items{{splat, d}};
            static const bool kNoDecal = std::getenv("OMC_NO_PLAYER_DECAL") != nullptr;
            if (const MeleeModels::GroundDecal* gd = kNoDecal ? nullptr : models_->unit_ground_decal(*ui_store_, u.catalog_id)) {
                const float fl = std::sqrt(gd->forward[0] * gd->forward[0] + gd->forward[1] * gd->forward[1]);
                if (fl > 1e-6f) {
                    formats::PlacedDoodad dd{};
                    dd.rotation = std::atan2(gd->forward[1], gd->forward[0]) + 1.5707963f;
                    const float cr = std::cos(dd.rotation), sr = std::sin(dd.rotation);
                    dd.x = cr * gd->offset[0] - sr * gd->offset[1];
                    dd.y = sr * gd->offset[0] + cr * gd->offset[1];
                    dd.sx = dd.sy = dd.sz = gd->scale;
                    items.push_back({&gd->model, dd});
                }
            }
            formats::Model pad = build_projector_items(items, 0.0f,
                                                       [&](float x, float y) { return ground_z_at(ux + x, uy + y); });
            if (std::getenv("OMC_SPLAT_TRACE"))
                for (std::size_t ri = 0; ri < pad.regions.size(); ++ri) {
                    const formats::M3Region& rg = pad.regions[ri];
                    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                    for (std::uint32_t v = rg.first_vertex; v < rg.first_vertex + rg.num_vertices; ++v)
                        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], pad.positions[v * 3 + a]); hi[a] = std::max(hi[a], pad.positions[v * 3 + a]); }
                    std::uint32_t imax = 0;
                    for (std::uint32_t k = rg.first_index; k < rg.first_index + rg.num_indices; ++k) imax = std::max<std::uint32_t>(imax, pad.indices[k]);
                    std::fprintf(stderr, "[splatreg] %s region %zu material '%s' verts %u+%u indices %u+%u (max index %u) x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f]\n", u.catalog_id.c_str(), ri,
                                 pad.region_material[ri] >= 0 ? pad.materials[std::size_t(pad.region_material[ri])].name.c_str() : "-", rg.first_vertex, rg.num_vertices, rg.first_index, rg.num_indices, imax,
                                 double(lo[0]), double(hi[0]), double(lo[1]), double(hi[1]), double(lo[2]), double(hi[2]));
                    if (pad.region_material[ri] >= 0) {
                        const formats::M3Material& mt = pad.materials[std::size_t(pad.region_material[ri])];
                        std::fprintf(stderr, "[splatreg]   flags 0x%08x blend %u\n", mt.flags, unsigned(mt.blend_mode));
                        for (const formats::M3Layer& L : mt.layers)
                            std::fprintf(stderr, "[splatreg]   layer %s '%s' w36 0x%x w40 0x%x w44 %u mult %.3f (id %u) add %.3f (id %u) uvoff %.3f,%.3f (id %u) uvang %.3f,%.3f,%.3f (id %u) tile %.3f,%.3f (id %u)\n", L.usage.c_str(),
                                         L.image_path.c_str(), L.word36, L.word40, L.word44, double(L.anim.bright_mult), L.anim.bright_mult_id, double(L.anim.midtone_offset), L.anim.midtone_id,
                                         double(L.anim.uv_offset[0]), double(L.anim.uv_offset[1]), L.anim.uv_offset_id, double(L.anim.uv_angle[0]), double(L.anim.uv_angle[1]), double(L.anim.uv_angle[2]),
                                         L.anim.uv_angle_id, double(L.anim.uv_tiling[0]), double(L.anim.uv_tiling[1]), L.anim.uv_tiling_id);
                    }
                }
            if (pad.vertex_count) return &splat_quads_.emplace(u.id, std::move(pad)).first->second;
        }
    }
    if (!splat->projectors.empty()) {
        const formats::M3Projector& pj = splat->projectors.front();
        const float pw = pj.x_right - pj.x_left, pl = pj.y_back - pj.y_front;
        if (std::getenv("OMC_SPLAT_TRACE"))
            std::fprintf(stderr, "[splat] %s projector type %u x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f] footprint extent %.2f\n", u.catalog_id.c_str(), pj.type,
                         double(pj.x_left), double(pj.x_right), double(pj.y_front), double(pj.y_back), double(pj.z_bottom), double(pj.z_top), double(extent));
        if (pw > 0.05f && pl > 0.05f && (u.player < 0 || std::getenv("OMC_SPLAT_PROJ_BOX")) && !std::getenv("OMC_SPLAT_FOOTPRINT_BOX")) extent = std::max(pw, pl);
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

std::string MeleeGame::power_visual_actor(const std::string& unit_id) const {
    static const bool kOld = std::getenv("OMC_WARPIN_PRESENT_OLD") != nullptr;
    if (kOld || !cat_ || unit_id.empty()) return "PowerVisual";
    if (auto it = power_visual_cache_.find(unit_id); it != power_visual_cache_.end()) return it->second;
    std::string found;
    for (const std::string& beh : cat_->array_links("CUnit", unit_id, "BehaviorArray")) {
        if (beh.empty() || !found.empty()) continue;
        const data::Catalog::Entry* e = cat_->find("CBehavior", beh);
        if (!e || e->tag != "CBehaviorPowerSource") continue;
        const std::string create = "Behavior." + beh + ".Create";
        for (const std::string& pid : cat_->ids("CActorPower")) {
            bool hit = false;
            std::string visual;
            for (const core::XmlNode& n : cat_->resolved("CActorPower", pid)) {
                if (n.name == "On") {
                    const std::string terms = n.attr_or("Terms", "");
                    if (terms.compare(0, create.size(), create) == 0 &&
                        (terms.find("Option Model Low") == std::string::npos || terms.find("!Option Model Low") != std::string::npos))
                        hit = true;
                } else if (n.name == "VisualArray") {
                    const std::string v = n.attr_or("value", "");
                    if (!v.empty()) visual = v;
                }
            }
            if (hit && !visual.empty()) { found = visual; break; }
        }
    }
    if (found.empty()) found = "PowerVisual";
    power_visual_cache_.emplace(unit_id, found);
    return found;
}

const formats::Model* MeleeGame::power_quad_for(const sim::Unit& u) {
    static const bool kOld = std::getenv("OMC_WARPIN_PRESENT_OLD") != nullptr;
    const std::array<float, 2> at = {float(u.x.to_double()), float(u.y.to_double())};
    if (auto it = power_quads_.find(u.id); it != power_quads_.end()) {
        auto pa = power_quad_at_.find(u.id);
        if (kOld || pa == power_quad_at_.end() || pa->second == at) return &it->second;
        power_quads_.erase(it);
    }
    if (power_quad_none_.count(u.id)) return nullptr;
    const std::string visual = power_visual_actor(u.catalog_id);
    const formats::Model* splat = (models_ && ui_store_) ? models_->actor_splat(*ui_store_, visual) : nullptr;
    if (!splat || splat->materials.empty() || u.power_radius.raw() <= 0) {
        power_quad_none_.insert(u.id);
        return nullptr;
    }
    power_quad_at_[u.id] = at;
    float scale = 1.0f;
    if (cat_)
        if (auto sm = cat_->scalar("CModel", cat_->scalar("CActorSplat", visual, "Model").value_or(visual), "ScaleMax"); sm && !sm->empty()) {
            const double v = std::strtod(sm->c_str(), nullptr);
            if (v > 0.0) scale = float(v);
        }
    const float extent = 2.0f * float(u.power_radius.to_double()) * scale;
    const std::uint32_t divs = 8;
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
    quad.material_refs.assign(1, formats::M3MaterialRef{1, 0});
    quad.region_material.assign(quad.regions.size(), 0);
    return &power_quads_.emplace(u.id, std::move(quad)).first->second;
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

std::vector<EquipmentEntry> MeleeGame::equipment_for(int player, const sim::Unit* body) const {
    std::vector<EquipmentEntry> out;
    if (player < 0 || !cat_) return out;
    const data::UpgradeCatalog* ups = research_ ? research_->upgrades() : nullptr;
    auto line_covers_body = [body, ups](const std::string& up_id) {
        if (!body || !ups) return true;
        for (const data::UpgradeEffect& e : ups->resolve(up_id)) {
            if ((e.catalog == "Unit" || e.catalog == "Actor") && e.entry == body->catalog_id) return true;
            if (e.catalog == "Weapon" && !e.entry.empty() &&
                (e.entry == body->wpn_id || e.entry == body->wpn2_id))
                return true;
        }
        return false;
    };
    auto base_cell = [&](const std::string& key, const std::optional<std::string>& icon) {
        if (!icon || icon->empty()) return;
        EquipmentEntry e;
        e.line = key;
        e.level = 0;
        e.icon = *icon;
        out.push_back(e);
    };
    if (body) {
        base_cell("LifeArmor", cat_->scalar("CActorUnit", body->catalog_id, "LifeArmorIcon"));
        if (body->shields_max.raw() > 0) base_cell("ShieldArmor", cat_->scalar("CActorUnit", body->catalog_id, "ShieldArmorIcon"));
        if (!body->wpn_id.empty()) base_cell("Weapon:" + body->wpn_id, cat_->scalar("CWeapon", body->wpn_id, "Icon"));
        if (!body->wpn2_id.empty() && body->wpn2_id != body->wpn_id)
            base_cell("Weapon:" + body->wpn2_id, cat_->scalar("CWeapon", body->wpn2_id, "Icon"));
    }
    const std::size_t base_count = out.size();
    for (const std::string& id : world_.researched(player)) {
        if (!line_covers_body(id)) continue;
        std::size_t end = id.size();
        while (end > 0 && id[end - 1] >= '0' && id[end - 1] <= '9') --end;
        int level = 0;
        for (std::size_t i = end; i < id.size(); ++i) level = level * 10 + (id[i] - '0');
        if (level <= 0) level = 1;
        if (body && ups && base_count) {
            bool raised = false;
            for (const data::UpgradeEffect& e : ups->resolve(id)) {
                std::string key;
                if (e.catalog == "Actor" && e.entry == body->catalog_id && e.field == "LifeArmorIcon") key = "LifeArmor";
                else if (e.catalog == "Actor" && e.entry == body->catalog_id && e.field == "ShieldArmorIcon") key = "ShieldArmor";
                else if (e.catalog == "Weapon" && e.field == "Icon" && (e.entry == body->wpn_id || e.entry == body->wpn2_id)) key = "Weapon:" + e.entry;
                if (key.empty()) continue;
                for (std::size_t q = 0; q < base_count; ++q)
                    if (out[q].line == key) {
                        if (level > out[q].level) {
                            out[q].level = level;
                            if (!e.value.empty()) out[q].icon = e.value;
                        }
                        raised = true;
                    }
            }
            if (raised) continue;
        }
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
    std::sort(out.begin() + std::ptrdiff_t(base_count), out.end(), [](const EquipmentEntry& a, const EquipmentEntry& b) {
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

std::vector<std::uint32_t> MeleeGame::army_units() const {
    std::vector<std::uint32_t> army;
    if (cfg_.human_player < 0 || !cat_) return army;
    for (const Unit& u : world_.units())
        if (u.alive && u.player == cfg_.human_player && !u.catalog_id.empty() &&
            cat_->unit_flag(u.catalog_id, "ArmySelect"))
            army.push_back(u.id);
    return army;
}

std::vector<std::uint32_t> MeleeGame::ready_warp_gates() const {
    std::vector<std::uint32_t> gates;
    if (cfg_.human_player < 0 || !cat_) return gates;
    for (const Unit& u : world_.units()) {
        if (!u.alive || u.constructing || u.player != cfg_.human_player || u.catalog_id.empty()) continue;
        if (cat_->unit_warp_products(u.catalog_id).empty()) continue;
        if (!world_.warp_ready(u.id)) continue;
        gates.push_back(u.id);
    }
    return gates;
}

int MeleeGame::idle_worker_count() const {
    if (cfg_.human_player < 0) return 0;
    int n = 0;
    for (const Unit& u : world_.units())
        if (worker_is_idle(u, cfg_.human_player)) ++n;
    return n;
}

std::vector<std::uint32_t> MeleeGame::idle_workers() const {
    std::vector<std::uint32_t> ids;
    if (cfg_.human_player < 0) return ids;
    for (const Unit& u : world_.units())
        if (worker_is_idle(u, cfg_.human_player)) ids.push_back(u.id);
    return ids;
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
        case sim::CmdError::CantBuildTooCloseToCreepSource:
        case sim::CmdError::CantBuildTooFarFromBuildPowerSource:
        case sim::CmdError::TechRequirementsNotMet: base = "BuildError"; break;
        case sim::CmdError::MustBuildRefineryFirst:
        case sim::CmdError::None: break;
    }
    if (!base) return {};
    const int race = human_race();
    const std::string id = std::string("Alert_") + (race == 1 ? "Protoss" : race == 2 ? "Zerg" : "Terran") + base;
    return cat_->find("CSound", id) ? id : std::string();
}

std::string MeleeGame::alert_sound_id(AlertKind kind, const std::string& alert_override) const {
    if (!cat_) return {};
    const char* base = nullptr;
    switch (kind) {
        case AlertKind::AttackUnit: base = "AttackUnit"; break;
        case AlertKind::AttackTown: base = "AttackTown"; break;
        case AlertKind::UpgradeComplete: base = "UpgradeComplete"; break;
        case AlertKind::BuildComplete: base = "BuildComplete"; break;
        case AlertKind::ResourceExhausted:
            base = alert_override.empty() ? "ResourceExhausted_Mineral" : alert_override.c_str();
            break;
    }
    if (!base) return {};
    const int race = human_race();
    const char* suffix = race == 1 ? "_Prot" : race == 2 ? "_Zerg" : "_Terr";
    if (auto s = cat_->scalar("CAlert", std::string(base) + suffix, "Sound"))
        if (!s->empty()) return *s;
    return {};
}

bool MeleeGame::advance_corpse(Corpse& c, float dt_ms, bool dead_phase) {
    c.time_ms += dt_ms;
    if (c.time_ms < c.length_ms) return true;
    if (c.dead) {
        if (c.hold || c.length_ms <= 0.0f) c.time_ms = c.length_ms > 0.0f ? c.length_ms : 0.0f;
        else c.time_ms = std::fmod(c.time_ms, c.length_ms);
        return true;
    }
    if (!dead_phase || !c.model) return false;
    const int dead = render::select_sequence(*c.model, {"Dead"}, 0);
    if (dead < 0 || std::size_t(dead) >= c.model->sequences.size()) return false;
    const formats::M3Sequence& s = c.model->sequences[std::size_t(dead)];
    c.dead = true;
    c.seq = dead;
    c.time_ms = 0.0f;
    c.length_ms = float(s.length_ms());
    c.hold = (s.flags & 0x1u) != 0;
    return true;
}

void MeleeGame::spawn_corpse(const sim::Unit& u) {
    const formats::Model* model = nullptr;
    std::vector<std::string> props = {"Death"};
    if (actor_cat_ && !u.catalog_id.empty()) {
        if (const data::CActorUnit* actor = actor_cat_->actor_for_unit(u.catalog_id)) {
            const data::ActorDeath* row = nullptr;
            const std::string want = sim::death_name_of(u.death_code);
            static const bool kUnderConstructionOld = std::getenv("OMC_DEATH_UNDER_CONSTRUCTION_OLD") != nullptr;
            const bool building = u.constructing || (u.id < constructing_seen_.size() && constructing_seen_[u.id] != 0);
            if (!kUnderConstructionOld && building)
                for (const data::ActorDeath& d : actor->death_array)
                    if (d.index == "UnderConstruction") { row = &d; break; }
            if (!row && u.death_code != 0)
                for (const data::ActorDeath& d : actor->death_array)
                    if (d.index == want) { row = &d; break; }
            if (!row)
                for (const data::ActorDeath& d : actor->death_array)
                    if (d.index == "Normal" || d.index.empty()) { row = &d; break; }
            if (!row && !actor->death_array.empty()) row = &actor->death_array.front();
            static const bool kCustomOld = std::getenv("OMC_DEATH_CUSTOM_OLD") != nullptr;
            data::ActorDeath empty_row;
            if (!kCustomOld && u.cargo_space > 0 && world_.cargo_used(u.id) == 0)
                for (const data::ActorDeath& d : actor->death_customs)
                    if (d.index == "Empty" && !d.model_link.empty()) {
                        empty_row = d;
                        if (row) empty_row.anim_props = row->anim_props;
                        row = &empty_row;
                        break;
                    }
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
                if (audio_ && !row->sound_link.empty()) audio_->play_sound(row->sound_link, 1.0f);
            }
        }
    }
    const bool own_model = !model;
    if (!model && u.id < driver_.actors.size()) model = driver_.actors[u.id].model;
    int seq = model ? render::select_sequence(*model, props, u.id) : -1;
    if (model && seq < 0 && props != std::vector<std::string>{"Death"}) seq = render::select_sequence(*model, {"Death"}, u.id);
    if (OMC_ENV_ONCE("OMC_CORPSE_TRACE"))
        std::fprintf(stderr, "[corpse] tick %d at (%.1f, %.1f) %s death '%s' -> %s model (%u verts, %zu emitters), clip %d '%s'\n", world_.tick(), u.x.to_double(), u.y.to_double(), u.catalog_id.c_str(), sim::death_name_of(u.death_code),
                     model ? (own_model ? "its own" : "the death row's") : "NO", model ? model->vertex_count : 0u, model ? model->particles.size() : std::size_t(0), seq,
                     model && seq >= 0 && std::size_t(seq) < model->sequences.size() ? model->sequences[std::size_t(seq)].name.c_str() : "-");
    if (!model) return;
    if (seq < 0 || std::size_t(seq) >= model->sequences.size()) return;
    Corpse c;
    c.model = model;
    c.seq = seq;
    c.length_ms = float(model->sequences[std::size_t(seq)].length_ms());
    if (c.length_ms <= 0.0f) return;
    c.x = float(u.x.to_double());
    c.y = float(u.y.to_double());
    c.z = cat_ ? cat_->unit_display_height(u.catalog_id) : 0.0f;
    static const bool kCorpseAtZero = std::getenv("OMC_CORPSE_AT_ZERO") != nullptr;
    if (!kCorpseAtZero) c.z += ground_z_at(c.x, c.y);
    c.scale = unit_draw_scale(cat_, u.catalog_id);
    c.player = u.player;
    c.fog_visibility = u.fog_visibility;
    static const bool kUnturned = std::getenv("OMC_CORPSE_UNTURNED") != nullptr;
    if (!kUnturned && u.id < driver_.actors.size()) c.orient = driver_.actors[u.id].orient();
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

bool MeleeGame::card_smart_click(const std::string& tag, const Control& ctrl, const ScreenPoint& p, bool queued) {
    if (!cat_ || cfg_.human_player < 0) return false;
    sim::Fixed gx, gy;
    if (!ctrl.ground_point(world_, p, gx, gy)) return false;
    auto own = [&](std::uint32_t id) {
        return id < world_.units().size() && world_.unit(id).alive && world_.unit(id).player == cfg_.human_player;
    };
    if (tag == "CAbilHarvest") {
        std::uint32_t node = ctrl.pick_resource(world_, p);
        if (node == sim::kNoTarget)
            for (const Unit& u : world_.units()) {
                if (!u.alive || !u.is_gas || u.player != cfg_.human_player) continue;
                const sim::Fixed r = u.radius + sim::Fixed::from_int(1), dx = u.x - gx, dy = u.y - gy;
                if (dx * dx + dy * dy <= r * r) { node = u.id; break; }
            }
        if (node == sim::kNoTarget || (world_.unit(node).resource_raw && world_.gas_harvest_ordered())) return false;
        const std::uint32_t depot = size_t(cfg_.human_player) < layout_.players.size()
                                        ? layout_.players[size_t(cfg_.human_player)].base.depot
                                        : sim::kNoTarget;
        bool any = false;
        for (std::uint32_t id : selection_) {
            if (!own(id) || !world_.unit(id).is_worker) continue;
            Unit& w = world_.unit(id);
            if (depot != sim::kNoTarget) w.home_depot = depot;
            w.gather_target = node;
            w.gathers_gas = world_.unit(node).is_gas;
            world_.issue(id, sim::Order{sim::OrderType::Gather, sim::Fixed(), sim::Fixed(), sim::kNoTarget}, queued);
            any = true;
        }
        return any;
    }
    if (tag == "CAbilRepair") {
        std::uint32_t t = ctrl.pick_own(world_, p);
        if (t == sim::kNoTarget) t = ctrl.pick_ally(world_, p);
        if (t == sim::kNoTarget) return false;
        const Unit& tu = world_.unit(t);
        if (tu.repair_time.raw() <= 0 || tu.hp >= tu.hp_max) return false;
        bool any = false;
        for (std::uint32_t id : selection_) {
            if (!own(id) || id == t || !world_.unit(id).is_worker) continue;
            sim::Order o;
            o.type = sim::OrderType::Repair;
            o.target = t;
            world_.issue(id, o, queued);
            any = true;
        }
        return any;
    }
    if (tag == "CAbilRally") {
        bool any = false;
        for (std::uint32_t id : selection_) {
            if (!own(id)) continue;
            const Unit& u = world_.unit(id);
            const std::string props = u.flying ? cat_->liftoff_target(u.catalog_id) : u.catalog_id;
            if (props.empty() || cat_->unit_rally_ability(props).empty()) continue;
            world_.set_rally(id, gx, gy);
            any = true;
        }
        return any;
    }
    if (tag == "CAbilTransport") {
        const std::uint32_t transport = human_lead_caster();
        const std::uint32_t t = ctrl.pick_own(world_, p);
        if (transport == sim::kNoTarget || t == sim::kNoTarget || t == transport) return false;
        const Unit& pu = world_.unit(t);
        if (pu.cargo_size <= 0 || pu.cargo_space > 0) return false;
        world_.issue(t, sim::Order{sim::OrderType::Load, sim::Fixed(), sim::Fixed(), transport}, queued);
        return true;
    }
    return false;
}

bool MeleeGame::human_load_all(const std::string& abil_id) {
    static const bool kOld = std::getenv("OMC_LOADALL_OLD") != nullptr;
    if (kOld || !cat_ || cfg_.human_player < 0) return false;
    const data::Catalog::Entry* ce = cat_->find("CAbil", abil_id);
    if (!ce || ce->tag != "CAbilTransport") return false;
    const double radius = std::atof(cat_->scalar("CAbil", abil_id, "SearchRadius").value_or("0").c_str());
    if (radius <= 0.0) return false;
    bool any = false;
    for (std::uint32_t tid : selection_) {
        if (tid >= world_.units().size()) continue;
        const Unit& t = world_.unit(tid);
        if (!t.alive || t.player != cfg_.human_player || t.cargo_space <= 0) continue;
        int room = t.cargo_space - world_.cargo_used(tid);
        for (const Unit& u : world_.units()) {
            if (room <= 0) break;
            if (!u.alive || u.player != cfg_.human_player || u.id == tid || u.loaded || u.cargo_size <= 0 ||
                u.cargo_space > 0 || u.flying)
                continue;
            if (t.load_require_race >= 0 && u.race_index != t.load_require_race) continue;
            if (!t.load_require_types.empty() &&
                std::find(t.load_require_types.begin(), t.load_require_types.end(), u.catalog_id) ==
                    t.load_require_types.end())
                continue;
            if (std::find(t.load_exclude_types.begin(), t.load_exclude_types.end(), u.catalog_id) !=
                t.load_exclude_types.end())
                continue;
            const double dx = u.x.to_double() - t.x.to_double(), dy = u.y.to_double() - t.y.to_double();
            const double reach = radius + t.radius.to_double() + u.radius.to_double();
            if (dx * dx + dy * dy > reach * reach) continue;
            world_.issue(u.id, sim::Order{sim::OrderType::Load, sim::Fixed(), sim::Fixed(), tid});
            room -= u.cargo_size;
            any = true;
        }
    }
    (void)any;
    return true;
}

bool MeleeGame::consume_modal_target(sim::Fixed wx, sim::Fixed wy, bool queued) {
    if (!(pending_attack_ || pending_patrol_ || pending_move_) || selection_.empty()) return false;
    static const bool kModalQueueOld = std::getenv("OMC_MODAL_QUEUE_OLD") != nullptr;
    if (kModalQueueOld) queued = false;
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
            if (queued)
                for (auto it = u.orders.rbegin(); it != u.orders.rend(); ++it)
                    if (it->tx.raw() != 0 || it->ty.raw() != 0) {
                        o.px = it->tx;
                        o.py = it->ty;
                        break;
                    }
        }
        world_.issue(id, o, queued);
    }
    cue_confirm_point(float(wx.to_double()), float(wy.to_double()), pending_attack_);
    audio_ui_event(human_lead_caster(), pending_attack_ ? data::UnitEvent::AttackOrder : data::UnitEvent::Confirm,
                   kUiCueTrim);
    if (queued) {
        modal_shift_kept_ = true;
        return true;
    }
    pending_attack_ = pending_patrol_ = pending_move_ = false;
    modal_shift_kept_ = false;
    return true;
}

void MeleeGame::handle_commands(const render::InputState& in, float aspect) {
    clear_placement_ghost();
    if (cues_) cue_cursor_point(false, 0.0f, 0.0f);
    if (modal_shift_kept_ && !in.shift) {
        modal_shift_kept_ = false;
        pending_attack_ = pending_patrol_ = pending_move_ = false;
    }
    if (cast_shift_kept_ && !in.shift) {
        cast_shift_kept_ = false;
        cancel_ability_cast();
    }
    if (cfg_.human_player < 0 || !in.view_w || !in.view_h) return;
    render::Mat4 mvp = camctl_.view_proj(aspect);
    Control ctrl = make_control(mvp, in.view_w, in.view_h, cfg_.human_player);
    update_cursor(in, ctrl);

    ensure_console_texture(in.view_w, in.view_h);
    refresh_hud_layout(in.view_w, in.view_h);
    const ConsolePanel console = console_view();
    refresh_hud_buttons(in.view_w, in.view_h);

    const HudRect mm = hud_minimap_rect(in.view_w, in.view_h, console_tex_.ok() ? &console : nullptr);
    const bool over_minimap = in.mouse_x >= float(mm.x0) && in.mouse_x < float(mm.x1) &&
                              in.mouse_y >= float(mm.y0) && in.mouse_y < float(mm.y1);
    const bool over_bar =
        hud_covers_point(in.view_w, in.view_h, &console, in.mouse_x, in.mouse_y) || hud_buttons_.at(in.mouse_x, in.mouse_y);
    auto minimap_world = [&](float& wx, float& wy) {
        hud_minimap_pixel_to_world(mm, minimap_extent(), in.mouse_x, in.mouse_y, wx, wy);
    };

    int hot_slot = -1;
    if (card_hotkey_first_char_old()) {
        hot_slot = (in.key_char && !in.key_ctrl && !in.key_alt && !selection_.empty())
                       ? card_slot_for_key(command_card_layout().card, in.key_char)
                       : -1;
    } else if (!in.key_ctrl && !in.key_alt && !selection_.empty() &&
               (in.key_char || !in.key_tokens.empty() || esc_card_)) {
        std::vector<std::string> keys;
        if (in.key_char) keys.emplace_back(1, in.key_char);
        for (const std::string& t : in.key_tokens) keys.push_back(t);
        if (esc_card_) keys.emplace_back("Escape");
        hot_slot = card_slot_for_keys(command_card_layout().card, keys);
    }

    static const bool kFixedOrderKeysOld = std::getenv("OMC_FIXED_ORDER_KEYS_OLD") != nullptr;
    if (kFixedOrderKeysOld && !selection_.empty() && hot_slot < 0) {
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
            snap_build_site(wx, wy);
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
            if (clear && !build_site_on_grid(wx, wy)) clear = false;
            build_valid_ = clear &&
                           world_.player(cfg_.human_player).minerals >= build_cost_ &&
                           world_.player(cfg_.human_player).gas >= build_gas_cost_;
            static const bool kNoGhost = std::getenv("OMC_NO_PLACEMENT_GHOST") != nullptr;
            if (!kNoGhost) {
                bool site = world_.player(cfg_.human_player).minerals >= build_cost_ &&
                            world_.player(cfg_.human_player).gas >= build_gas_cost_ &&
                            world_.creep_placement_ok(build_proto_, wx, wy) && world_.resource_halo_ok(build_proto_, wx, wy) &&
                            (!build_proto_.requires_vespene || on_live_gas_node(wx, wy)) &&
                            (!build_proto_.needs_power || world_.powered_at(wx, wy, cfg_.human_player));
                set_placement_ghost(build_proto_.catalog_id, wx, wy, site);
            }
        } else {
            build_valid_ = false;
        }
    }
    {
        static const bool kWarpPresentOld = std::getenv("OMC_WARPIN_PRESENT_OLD") != nullptr;
        warp_ghost_.active = false;
        sim::Fixed wx, wy;
        if (pending_warp_ && !kWarpPresentOld && cfg_.human_player >= 0 && !over_bar &&
            ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
            warp_ghost_.active = true;
            warp_ghost_.unit_id = pending_warp_unit_;
            warp_ghost_.x = float(wx.to_double());
            warp_ghost_.y = float(wy.to_double());
            warp_ghost_.ok = world_.powered_at(wx, wy, cfg_.human_player) && world_.warp_ready(pending_warp_gate_);
        }
    }
    if (pending_land_ && !std::getenv("OMC_LAND_CARD_OLD") && !std::getenv("OMC_NO_PLACEMENT_GHOST") &&
        pending_land_unit_ < world_.units().size()) {
        sim::Fixed wx, wy;
        if (!over_bar && ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
            const bool ok = land_site(pending_land_unit_, wx, wy);
            set_placement_ghost(world_.unit(pending_land_unit_).catalog_id, wx, wy, ok);
        }
    }
    if (pending_root_ && cat_) {
        sim::Fixed wx, wy;
        const std::uint32_t rid = human_lead_caster();
        const auto rms = cat_->morph_spec(pending_root_abil_);
        if (rid != sim::kNoTarget && rms && !over_bar && ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
            const bool ok = root_site(pending_root_abil_, rid, wx, wy);
            set_placement_ghost(rms->target, wx, wy, ok);
        }
    }
    if (has_pending_ability_cast()) {
        sim::Fixed cwx, cwy;
        const bool cpt = !over_bar && !over_minimap && ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, cwx, cwy);
        cue_cursor_point(cpt, float(cwx.to_double()), float(cwy.to_double()));
    } else if (cues_) {
        cue_cursor_point(false, 0.0f, 0.0f);
    }

    if (minimap_drag_ && (!in.left_down || map_w_ <= 0.0f)) minimap_drag_ = false;
    if (minimap_drag_ && !in.left_pressed) {
        float wx, wy; minimap_world(wx, wy);
        camctl_.focus = {wx, wy, ground_z_at(wx, wy)};
        user_scrolled_ = true;
        dragging_ = false;
        return;
    }
    if (const std::optional<HudPress> hp = hud_buttons_.route(in)) {
        on_hud_button(*hp);
        dragging_ = false;
        return;
    }
    if ((in.left_pressed || in.right_pressed) && (hud_ui_.minimap.ping_armed || in.key_alt)) {
        float pwx = 0.0f, pwy = 0.0f;
        bool have_point = false;
        if (over_minimap && map_w_ > 0.0f && map_h_ > 0.0f) {
            minimap_world(pwx, pwy);
            have_point = true;
        } else if (!over_bar) {
            sim::Fixed fx, fy;
            if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, fx, fy)) {
                pwx = float(fx.to_double());
                pwy = float(fy.to_double());
                have_point = true;
            }
        }
        if (hud_minimap_take_click(in, have_point, pwx, pwy)) {
            dragging_ = false;
            return;
        }
    }
    if (over_minimap && map_w_ > 0.0f && map_h_ > 0.0f) {
        static const bool kMinimapCastOld = std::getenv("OMC_MINIMAP_CAST_OLD") != nullptr;
        if (in.right_pressed && !kMinimapCastOld && modal_armed()) {
            float wx, wy; minimap_world(wx, wy);
            camctl_.focus = {wx, wy, ground_z_at(wx, wy)};
            user_scrolled_ = true;
            return;
        }
        if (in.left_pressed) {
            float wx, wy; minimap_world(wx, wy);
            const sim::Fixed mfx = sim::Fixed::from_ratio(int(std::lround(double(wx) * 4096.0)), 4096);
            const sim::Fixed mfy = sim::Fixed::from_ratio(int(std::lround(double(wy) * 4096.0)), 4096);
            const bool spent = (!kMinimapCastOld && minimap_place_armed(mfx, mfy, in.shift)) ||
                               consume_modal_target(mfx, mfy, in.shift);
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
            cue_confirm_point(wx, wy, false);
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
                if (toggle_hangar_auto_build(ab.ability)) break;
                if (!cat_->ability_autocast(ab.ability).offered) break;
                static const bool kToggleEachOld = std::getenv("OMC_AUTOCAST_TOGGLE_EACH_OLD") != nullptr;
                std::uint32_t lead = sim::kNoTarget;
                if (!kToggleEachOld)
                    for (std::uint32_t id : selection_) {
                        if (id >= world_.units().size()) continue;
                        const sim::Unit& su = world_.units()[id];
                        if (!su.alive || su.player != cfg_.human_player) continue;
                        if (lead == sim::kNoTarget) lead = id;
                        const std::vector<std::string> abils = cat_->array_links("CUnit", su.catalog_id, "AbilArray");
                        if (std::find(abils.begin(), abils.end(), ab.ability) != abils.end()) { lead = id; break; }
                    }
                const bool turn_on = lead != sim::kNoTarget && !cast_policy_->autocast_state(world_, lead, ab.ability);
                for (std::uint32_t id : selection_) {
                    if (id >= world_.units().size()) continue;
                    const sim::Unit& su = world_.units()[id];
                    if (!su.alive || su.player != cfg_.human_player) continue;
                    if (lead != sim::kNoTarget) cast_policy_->set_autocast(id, ab.ability, turn_on);
                    else cast_policy_->toggle_autocast(world_, id, ab.ability);
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
                    if (card.slots[i].is_cancel && cancel_building == sim::kNoTarget && cancel_producer == sim::kNoTarget &&
                        !std::getenv("OMC_NO_CHANNEL_CANCEL")) {
                        bool channel = false;
                        for (std::uint32_t id : selection_)
                            if (id < world_.units().size() && world_.is_channelling(id)) channel = true;
                        if (channel) {
                            cancel_ability_cast();
                            pending_attack_ = pending_patrol_ = pending_move_ = false;
                            for (std::uint32_t id : selection_)
                                if (id < world_.units().size()) world_.cancel_channel(id);
                            break;
                        }
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
                        const data::UnitCost bc = data::build_charge(*cat_, b.catalog_id);
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
                    if (card.slots[i].is_cancel && cat_ && !std::getenv("OMC_ROOT_PLACEMENT_OLD")) {
                        std::vector<std::string> rooting;
                        for (std::uint32_t id : selection_) {
                            const auto ma = morph_abil_.find(id);
                            if (ma == morph_abil_.end() || !world_.is_morphing(id)) continue;
                            const data::Catalog::Entry* me = cat_->find("CAbil", ma->second);
                            if (me && me->tag == "CAbilMorphPlacement") rooting.push_back(ma->second);
                        }
                        bool cancelled = false;
                        for (const std::string& abil : rooting) cancelled = human_cancel_morph(abil) || cancelled;
                        if (cancelled) {
                            cancel_ability_cast();
                            pending_attack_ = pending_patrol_ = pending_move_ = false;
                            break;
                        }
                    }
                    bool routed = false;
                    for (const AbilityCardEntry& ab : card_abils)
                        if (ab.slot == i) {
                            if (ab.requirement_met && cat_) {
                                pending_attack_ = pending_patrol_ = pending_move_ = false;
                                cancel_ability_cast();
                                static const bool kLibOld = std::getenv("OMC_LIBERATOR_OPENER_OLD") != nullptr;
                                if (!kLibOld && ab.ability == "LiberatorAGTarget" && selected_liberator() != sim::kNoTarget) {
                                    pending_liberator_ = selected_liberator();
                                    pending_defender_ = true;
                                } else if (!kLibOld && ab.ability == "LiberatorAATarget" &&
                                           selected_liberator_ag() != sim::kNoTarget) {
                                    human_morph(selected_liberator_ag(), "LiberatorMorphtoAA");
                                } else if (ab.command == "LoadAll" && human_load_all(ab.ability)) {
                                } else if (human_toggle_cloak(ab.ability, ab.command)) {
                                } else if (human_toggle_weapon(ab.ability, ab.command)) {
                                } else if (human_toggle_creep(ab.ability, ab.command)) {
                                } else if (human_salvage(ab.ability)) {
                                } else if (human_arm_hangar(ab.ability)) {
                                } else if (const data::Catalog::Entry* re =
                                               cat_->find("CAbil", ab.ability);
                                           re && re->tag == "CAbilResearch") {
                                    human_research(ab.ability, ab.command, human_lead_caster());
                                } else if (ab.command == "Return" &&
                                           cat_->is_harvest_ability(ab.ability)) {
                                    order_return_cargo(*cat_, world_, human_lead_caster());
                                } else if (ab.command == "Cancel" && human_cancel_morph(ab.ability)) {
                                } else if (ab.command == "Cancel" && !std::getenv("OMC_EFFECT_CANCEL_IDLE_OLD") &&
                                           human_cancel_effect(ab.ability)) {
                                } else if (const data::Catalog::Entry* rp = cat_->find("CAbil", ab.ability);
                                           rp && rp->tag == "CAbilMorphPlacement" &&
                                           !std::getenv("OMC_ROOT_PLACEMENT_OLD")) {
                                    pending_attack_ = pending_patrol_ = pending_move_ = false;
                                    pending_root_ = true;
                                    pending_root_abil_ = ab.ability;
                                } else if (const auto ms = cat_->morph_spec(ab.ability);
                                           ms && !ms->target.empty()) {
                                    human_morph_selection(ab.ability);
                                } else {
                                    const AbilTargetMode m = ability_target_mode(*cat_, ab.ability);
                                    if (m == AbilTargetMode::Instant) {
                                        cast_queued_ = in.shift && !cast_queue_old();
                                        human_cast_instant(ab.ability);
                                        cast_queued_ = false;
                                    } else {
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
                                    if (burrow_command_allowed(id)) human_burrow(id, true);
                            break;
                        case kMeleeAbilUnburrow:
                            if (cat_)
                                for (std::uint32_t id : selected_burrowers(true))
                                    human_burrow(id, false);
                            break;
                        case kMeleeAbilLiftOff:
                            if (cat_)
                                if (const std::uint32_t id = selected_liftoff_structure(false);
                                    id != sim::kNoTarget && !liftoff_busy(id)) {
                                    const auto ls = cat_->liftoff_spec(cat_->liftoff_ability(world_.units()[id].catalog_id));
                                    world_.lift_off(id, ls ? sim::Fixed::from_double(ls->lock) : sim::Fixed(),
                                                    ls ? sim::Fixed::from_double(ls->rise_delay) : sim::Fixed(),
                                                    ls ? sim::Fixed::from_double(ls->rise) : sim::Fixed());
                                }
                            break;
                        case kMeleeAbilLowerDepot:
                            if (cat_)
                                if (const std::uint32_t id = selected_depot(false); id != sim::kNoTarget)
                                    human_depot(id, true);
                            break;
                        case kMeleeAbilRaiseDepot:
                            if (cat_)
                                if (const std::uint32_t id = selected_depot(true); id != sim::kNoTarget)
                                    human_depot(id, false);
                            break;
                        case kMeleeAbilLand:
                            pending_land_unit_ = selected_liftoff_structure(true);
                            if (pending_land_unit_ != sim::kNoTarget) pending_land_ = true;
                            pending_land_addon_.clear();
                            pending_defender_ = false;
                            pending_attack_ = pending_patrol_ = pending_move_ = false;
                            break;
                        case kMeleeAbilUnloadAll:
                            pending_unload_unit_ = selected_loaded_transport();
                            static const bool kUnloadAllTargeted = std::getenv("OMC_UNLOAD_ALL_TARGETED") != nullptr;
                            if (pending_unload_unit_ != sim::kNoTarget && cat_ && !kUnloadAllTargeted &&
                                !cat_->transport_unload_targeted(world_.unit(pending_unload_unit_).catalog_id)) {
                                const sim::Unit& tu = world_.unit(pending_unload_unit_);
                                world_.issue(pending_unload_unit_, sim::Order{sim::OrderType::Unload, tu.x, tu.y,
                                                                              sim::kNoTarget});
                                pending_unload_unit_ = sim::kNoTarget;
                            }
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
                static const bool kBuildClickGridOld = std::getenv("OMC_BUILD_CLICK_GRID_OLD") != nullptr;
                if (!kBuildClickGridOld) snap_build_site(wx, wy);
                bool clear = wx.to_double() >= 0.0 && wy.to_double() >= 0.0 &&
                             wx.to_double() <= double(map_w_) && wy.to_double() <= double(map_h_);
                if (clear && !kBuildClickGridOld && !build_site_on_grid(wx, wy)) clear = false;
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
                    const int cost = data::build_charge(*cat_, gas, world_.unit(bw).catalog_id).minerals;
                    if (world_.player(cfg_.human_player).minerals >= cost) {
                        Unit& gy = world_.unit(geyser);
                        sim::Unit proto = make_unit(*cat_, gas, cfg_.human_player);
                        proto.speed = sim::Fixed();
                        proto.is_gas = true;
                        proto.resource_left = gy.resource_left;
                        const sim::Fixed gpx = gy.x, gpy = gy.y;
                        if (world_.issue_worker_build(bw, gpx, gpy, proto, cost, 0,
                                                      sim::Fixed::from_int(30),
                                                      sim::Fixed::from_int(2), build_verb_for(bw))) {
                            world_.unit(geyser).resource_left = 0;
                            cue_build_issued(bw, gpx, gpy, gas);
                        }
                    }
                }
            }
            pending_refinery_ = false;
        } else if (pending_root_) {
            dragging_ = false;
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) root_order(wx, wy);
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
                if (!std::getenv("OMC_LAND_CARD_OLD")) land_site(pending_land_unit_, wx, wy);
                const bool in_bounds = wx.to_double() >= 0.0 && wy.to_double() >= 0.0 &&
                                       wx.to_double() <= double(map_w_) && wy.to_double() <= double(map_h_);
                const bool ordered = !pending_land_addon_.empty()
                                         ? in_bounds && human_land_addon(pending_land_unit_, wx, wy, pending_land_addon_)
                                         : in_bounds && world_.order_land(pending_land_unit_, wx, wy);
                if (ordered) {
                    pending_land_ = false;
                    pending_land_unit_ = sim::kNoTarget;
                    pending_land_addon_.clear();
                }
            } else {
                pending_land_ = false;
                pending_land_unit_ = sim::kNoTarget;
                pending_land_addon_.clear();
            }
        } else if (pending_unload_) {
            dragging_ = false;
            sim::Fixed wx, wy;
            if (pending_unload_unit_ < world_.units().size() && world_.unit(pending_unload_unit_).alive &&
                ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                const bool mobile = world_.unit(pending_unload_unit_).speed.raw() > 0;
                if (mobile) world_.issue(pending_unload_unit_, sim::Order{sim::OrderType::Move, wx, wy, sim::kNoTarget});
                world_.issue(pending_unload_unit_, sim::Order{sim::OrderType::Unload, wx, wy, sim::kNoTarget}, mobile);
            }
            pending_unload_ = false;
            pending_unload_unit_ = sim::kNoTarget;
        } else if ((pending_attack_ || pending_patrol_ || pending_move_) && !selection_.empty()) {
            dragging_ = false;
            sim::Fixed wx, wy;
            static const bool kAttackUnitOld = std::getenv("OMC_ATTACK_UNIT_CLICK_OLD") != nullptr;
            const std::uint32_t foe = pending_attack_ && !kAttackUnitOld
                                          ? ctrl.pick_target(world_, ScreenPoint{in.mouse_x, in.mouse_y})
                                          : sim::kNoTarget;
            if (foe != sim::kNoTarget) {
                for (std::uint32_t id : selection_) {
                    if (id >= world_.units().size() || id == foe) continue;
                    world_.issue(id, sim::Order{sim::OrderType::Attack, sim::Fixed(), sim::Fixed(), foe}, in.shift);
                }
                cue_confirm_point(float(world_.unit(foe).x.to_double()), float(world_.unit(foe).y.to_double()), true);
                audio_ui_event(human_lead_caster(), data::UnitEvent::AttackOrder, kUiCueTrim);
                if (in.shift) {
                    modal_shift_kept_ = true;
                } else {
                    pending_attack_ = pending_patrol_ = pending_move_ = false;
                    modal_shift_kept_ = false;
                }
            } else if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                consume_modal_target(wx, wy, in.shift);
            } else {
                audio_ui_event(human_lead_caster(), data::UnitEvent::Confirm, kUiCueTrim);
                pending_attack_ = pending_patrol_ = pending_move_ = false;
            }
        } else if (has_pending_ability_cast()) {
            dragging_ = false;
            static const bool kCardSmartOld = std::getenv("OMC_CARD_SMART_OLD") != nullptr;
            const data::Catalog::Entry* pce = cat_ ? cat_->find("CAbil", pending_cast_abil_) : nullptr;
            std::string ptag = pce ? pce->tag : std::string();
            if (ptag == "CAbilEffectTarget" && pending_cast_abil_.size() >= 6 &&
                pending_cast_abil_.compare(pending_cast_abil_.size() - 6, 6, "Repair") == 0)
                ptag = "CAbilRepair";
            if (!kCardSmartOld && (ptag == "CAbilHarvest" || ptag == "CAbilRepair" || ptag == "CAbilRally" ||
                                   ptag == "CAbilTransport")) {
                if (card_smart_click(ptag, ctrl, ScreenPoint{in.mouse_x, in.mouse_y}, in.shift)) {
                    cancel_ability_cast();
                    sim::Fixed cwx, cwy;
                    const bool cpt = ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, cwx, cwy);
                    cue_after_order(cpt, float(cwx.to_double()), float(cwy.to_double()), sim::kNoTarget);
                    audio_ui_event(human_lead_caster(), data::UnitEvent::Confirm, kUiCueTrim);
                }
            } else if (pending_cast_mode_ == AbilTargetMode::Point) {
                sim::Fixed wx, wy;
                if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                    const std::uint32_t caster = human_lead_caster();
                    if (caster != sim::kNoTarget && is_nova_cast(caster, pending_cast_abil_)) {
                        if (world_.launch_nova(caster, wx, wy)) cancel_ability_cast();
                    } else if (cast_pending_at_point(wx, wy, in.shift)) {
                        cue_confirm_point(float(wx.to_double()), float(wy.to_double()), false);
                    }
                }
            } else {
                std::uint32_t tgt = ctrl.pick_target(world_, ScreenPoint{in.mouse_x, in.mouse_y});
                if (tgt == sim::kNoTarget)
                    tgt = ctrl.pick_own(world_, ScreenPoint{in.mouse_x, in.mouse_y});
                if (tgt != sim::kNoTarget && cast_pending_at_target(tgt, in.shift)) cue_confirm_target(tgt);
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
        static const bool click_old = std::getenv("OMC_CLICK_PICK_OLD") != nullptr;
        if (!marquee && !click_old) {
            const std::uint32_t hit = click_select_at(ctrl, ScreenPoint{in.mouse_x, in.mouse_y});
            set_selection(hit == sim::kNoTarget ? std::vector<std::uint32_t>{} : std::vector<std::uint32_t>{hit});
        } else {
            set_selection(ctrl.box_select(world_, ScreenRect{x0, y0, x1, y1}, marquee));
        }
        const bool clicked_nothing = selection_.empty();
        select_click_ = !marquee;
        bool handled672 = false;
        if (select_rules_672_ && in.shift && !prev_selection.empty() && !clicked_nothing) {
            auto owned = [&](const std::vector<std::uint32_t>& v) {
                return std::any_of(v.begin(), v.end(), [&](std::uint32_t id) {
                    return id < world_.units().size() && world_.unit(id).player == cfg_.human_player;
                });
            };
            const bool prev_own = owned(prev_selection), hit_own = owned(selection_);
            if (!(prev_own && hit_own)) {
                handled672 = true;
                if (!prev_own && !marquee && !in.key_ctrl && selection_.size() == 1 && prev_selection.size() == 1 &&
                    selection_[0] == prev_selection[0])
                    set_selection({});
                else
                    set_selection(prev_selection);
                last_click_ms_ = -1e9;
            }
        }
        if (!handled672 && !marquee && selection_.size() > 1) {
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
        auto same_type_on_screen = [&](const Unit& picked) {
            std::vector<std::uint32_t> same;
            for (const Unit& u : world_.units()) {
                if (!u.alive || u.player != picked.player || u.loaded || u.in_gas_structure) continue;
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
            if (select_rules_672_ && same.size() > Control::kSelectionCap) same.resize(Control::kSelectionCap);
            return same;
        };
        auto cap672 = [&](std::vector<std::uint32_t> v) {
            if (select_rules_672_ && v.size() > Control::kSelectionCap) v.resize(Control::kSelectionCap);
            return v;
        };
        static const bool add_all_type_old = std::getenv("OMC_ADD_ALL_TYPE_OLD") != nullptr;
        if (!handled672 && !add_all_type_old && in.shift && in.key_ctrl && !marquee && selection_.size() == 1 &&
            selection_[0] < world_.units().size() && world_.unit(selection_[0]).player == cfg_.human_player) {
            std::vector<std::uint32_t> merged = prev_selection;
            for (std::uint32_t id : same_type_on_screen(world_.unit(selection_[0])))
                if (std::find(merged.begin(), merged.end(), id) == merged.end()) merged.push_back(id);
            set_selection(cap672(std::move(merged)));
            last_click_ms_ = -1e9;
        }
        const bool add_all_typed = !add_all_type_old && in.shift && in.key_ctrl && !marquee;
        const bool shift_merge = !handled672 && !add_all_typed && in.shift && !prev_selection.empty() && !selection_.empty() &&
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
            set_selection(cap672(std::move(merged)));
        }
        if (!handled672 && !add_all_typed && !shift_merge && !marquee && selection_.size() == 1 && selection_[0] < world_.units().size()) {
            const bool twice = in.key_ctrl || (clock_ms_ - last_click_ms_ <= kDoubleClickMs && std::fabs(in.mouse_x - last_click_x_) <= 6.0f &&
                                               std::fabs(in.mouse_y - last_click_y_) <= 6.0f);
            const Unit& picked = world_.unit(selection_[0]);
            if (twice && picked.player == cfg_.human_player) {
                std::vector<std::uint32_t> same = same_type_on_screen(picked);
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
        if (!handled672 && selection_.empty() && !marquee && click_old) {
            const std::uint32_t enemy = ctrl.pick_target(world_, ScreenPoint{in.mouse_x, in.mouse_y});
            if (enemy != sim::kNoTarget) set_selection({enemy});
        }
        if (!handled672 && selection_.empty()) {
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                for (const Unit& u : world_.units()) {
                    if (!u.alive || u.player >= 0) continue;
                    if (geyser_covered(u)) continue;
                    const sim::Fixed dx = u.x - wx, dy = u.y - wy;
                    const sim::Fixed rr = u.radius + sim::Fixed::from_ratio(1, 2);
                    if (dx * dx + dy * dy <= rr * rr) { set_selection({u.id}); break; }
                }
            }
        }
        static const bool empty_clears_old = std::getenv("OMC_EMPTY_CLICK_CLEARS_OLD") != nullptr;
        if (!marquee && clicked_nothing && selection_.empty() && !empty_clears_old) set_selection(prev_selection);
    }

    static const bool kRightCancelOld = std::getenv("OMC_RIGHT_CANCEL_OLD") != nullptr;
    const bool right_cancelled = in.right_pressed && !kRightCancelOld && !over_bar && modal_armed();
    if (right_cancelled) clear_armed_modals();
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
    if (in.right_pressed && !right_cancelled && !pending_build_ && !pending_refinery_ && selection_owned && !over_bar) {
        std::uint32_t node = sim::kNoTarget;
        std::uint32_t raw_node = sim::kNoTarget;
        sim::Fixed gx, gy;
        const bool have_pt = ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, gx, gy);
        if (have_pt) {
            sim::Fixed best_d2;
            for (const Unit& u : world_.units()) {
                const bool neutral_resource = u.alive && u.player < 0;
                const bool own_gas = u.alive && u.is_gas && u.player == cfg_.human_player;
                if (!neutral_resource && !own_gas) continue;
                if (u.resource_raw && world_.gas_harvest_ordered()) {
                    if (raw_node == sim::kNoTarget && !geyser_covered(u)) {
                        const sim::Fixed rr = u.radius + sim::Fixed::from_ratio(1, 2);
                        const sim::Fixed ex = u.x - gx, ey = u.y - gy;
                        if (ex * ex + ey * ey <= rr * rr) raw_node = u.id;
                    }
                    continue;
                }
                sim::Fixed r = u.radius + sim::Fixed::from_int(2);
                sim::Fixed dx = u.x - gx, dy = u.y - gy, d2 = dx * dx + dy * dy;
                if (d2 > r * r) continue;
                if (node == sim::kNoTarget || d2 < best_d2) { node = u.id; best_d2 = d2; }
            }
        }
        const bool enemy_under = ctrl.pick_target(world_, ScreenPoint{in.mouse_x, in.mouse_y}) != sim::kNoTarget;
        if (enemy_under) node = sim::kNoTarget;
        bool raw_refused = false;
        if (node == sim::kNoTarget && raw_node != sim::kNoTarget && !enemy_under) {
            std::vector<std::uint32_t> rest;
            for (std::uint32_t id : selection_) {
                if (id >= world_.units().size()) continue;
                if (world_.unit(id).is_worker && world_.unit(id).player == cfg_.human_player) raw_refused = true;
                else rest.push_back(id);
            }
            if (raw_refused) {
                world_.report_error(cfg_.human_player, sim::CmdError::MustBuildRefineryFirst);
                if (!rest.empty())
                    ctrl.issue_command(world_, rest, ScreenPoint{in.mouse_x, in.mouse_y},  false,
                                        in.shift);
            }
        }
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

        if (raw_refused) {
        } else if (resume != sim::kNoTarget) {
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
        if (std::getenv("OMC_SMART_TRACE")) {
            std::fprintf(stderr, "[smart] tick %d at (%.0f, %.0f) ground %d (%.3f, %.3f) node %u resume %u transport %u producers %zu raw_refused %d\n",
                         world_.tick(), double(in.mouse_x), double(in.mouse_y), int(have_pt), gx.to_double(), gy.to_double(), unsigned(node),
                         unsigned(resume), unsigned(transport), producers.size(), int(raw_refused));
            if (have_pt) {
                std::fprintf(stderr, "[smart]   ground z along y at x %.1f:", gx.to_double());
                for (int k = -6; k <= 6; ++k)
                    std::fprintf(stderr, " %.1f:%.2f", gy.to_double() + k, double(ground_z_at(float(gx.to_double()), float(gy.to_double()) + float(k))));
                std::fprintf(stderr, "\n");
            }
            for (std::uint32_t id : selection_) {
                if (id >= world_.units().size()) continue;
                const Unit& su = world_.units()[id];
                if (su.orders.empty()) std::fprintf(stderr, "[smart]   %u %s at (%.3f, %.3f): no order\n", unsigned(id), su.catalog_id.c_str(), su.x.to_double(), su.y.to_double());
                else std::fprintf(stderr, "[smart]   %u %s at (%.3f, %.3f): order %d to (%.3f, %.3f) target %u\n", unsigned(id), su.catalog_id.c_str(), su.x.to_double(),
                                  su.y.to_double(), int(su.orders.front().type), su.orders.front().tx.to_double(), su.orders.front().ty.to_double(), unsigned(su.orders.front().target));
            }
        }
        if (!raw_refused) {
            const std::uint32_t cue_tgt = enemy_under ? ctrl.pick_target(world_, ScreenPoint{in.mouse_x, in.mouse_y})
                                          : node != sim::kNoTarget ? node
                                          : resume != sim::kNoTarget ? resume
                                                                     : transport;
            cue_after_order(have_pt, float(gx.to_double()), float(gy.to_double()), cue_tgt);
        }
        audio_ui_event(human_lead_caster(), data::UnitEvent::Confirm, kUiCueTrim);
    }

    const std::uint32_t lead = human_lead_caster();
    if (lead != audio_sel_lead_) {
        if (lead != sim::kNoTarget) audio_ui_event(lead, data::UnitEvent::Select, 1.0f);
        audio_sel_lead_ = lead;
        audio_sel_repeat_ = 0;
    } else if (select_click_ && lead != sim::kNoTarget && selection_.size() == 1) {
        if (++audio_sel_repeat_ >= kPissedClicks) {
            audio_ui_event(lead, data::UnitEvent::Pissed, 1.0f);
            audio_sel_repeat_ = 0;
        }
    }
    select_click_ = false;
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

bool MeleeGame::debug_create(const std::string& unit_id_in, float x, float y, float facing) {
    const bool enemy = !unit_id_in.empty() && unit_id_in[0] == '!';
    const std::string unit_in = enemy ? unit_id_in.substr(1) : unit_id_in;
    const bool founding = !unit_in.empty() && unit_in[0] == '~';
    const std::string unit_id = founding ? unit_in.substr(1) : unit_in;
    if (!cat_ || cfg_.human_player < 0 || !cat_->find("CUnit", unit_id)) return false;
    int first_hostile = cfg_.human_player == 0 ? 1 : 0;
    for (int p = 0; p < int(layout_.players.size()); ++p)
        if (!world_.are_allied(p, cfg_.human_player)) {
            first_hostile = p;
            break;
        }
    const int owner = enemy ? first_hostile : cfg_.human_player;
    sim::Unit proto = make_unit(*cat_, unit_id, owner);
    if (facing > -100.0f) proto.facing = sim::Fixed::from_double(double(facing));
    proto.x = sim::Fixed::from_double(double(x));
    proto.y = sim::Fixed::from_double(double(y));
    if (founding) proto.speed = sim::Fixed();
    if (proto.needs_power && !std::getenv("OMC_DEBUG_CREATE_UNMANAGED_POWER")) proto.power_managed = true;
    const std::uint32_t id = world_.add_unit(proto);
    if (layout_.unit_type.size() <= id) layout_.unit_type.resize(id + 1);
    layout_.unit_type[id] = unit_id;
    if (facing > -100.0f && !std::getenv("OMC_DEBUG_CREATE_NO_SPAWN_FACING")) driver_.set_spawn_facing(id, facing);
    if (founding) {
        double secs = 0.0;
        for (const char* worker : {"Drone", "Probe", "SCV"})
            for (const auto& bp : cat_->unit_build_products(worker))
                if (bp.unit == unit_id && bp.time > 0) secs = bp.time;
        if (secs <= 0.0) return false;
        sim::Fixed start_hp = sim::Fixed::from_raw(world_.units()[id].hp_max.raw() / 10);
        if (start_hp.raw() <= 0) start_hp = sim::Fixed::from_int(1);
        world_.start_construction(id, sim::Fixed::from_double(secs), start_hp);
    }
    return true;
}

bool MeleeGame::debug_morph(const std::string& from_unit, const std::string& morph_abil_at) {
    if (!cat_ || cfg_.human_player < 0) return false;
    std::string morph_abil = morph_abil_at;
    float tx = 0.0f, ty = 0.0f;
    bool at = false;
    if (const std::size_t p = morph_abil_at.find('@'); p != std::string::npos) {
        morph_abil = morph_abil_at.substr(0, p);
        at = std::sscanf(morph_abil_at.c_str() + p + 1, "%f,%f", &tx, &ty) == 2;
    }
    world_.player(cfg_.human_player).minerals += 5000;
    world_.player(cfg_.human_player).gas += 5000;
    if (const auto ms = cat_->morph_spec(morph_abil)) {
        std::string chain;
        for (const std::string& c : ms->chain) chain += c + " ";
        std::fprintf(stderr, "[morph] spec %s: time %.2f target %s chain %s\n", morph_abil.c_str(), ms->morph_time, ms->target.c_str(), chain.c_str());
    }
    const std::string bdown = cat_->burrow_ability(from_unit);
    const std::string bvar = bdown.empty() ? std::string() : cat_->burrowed_variant(from_unit);
    const std::string bup = bvar.empty() ? std::string() : cat_->unburrow_ability(bvar);
    const bool burrow_down = (morph_abil.rfind("Burrow", 0) == 0 && morph_abil.size() > 4 && morph_abil.compare(morph_abil.size() - 4, 4, "Down") == 0) ||
                             (!bdown.empty() && morph_abil == bdown);
    const bool burrow_up = (morph_abil.rfind("Burrow", 0) == 0 && morph_abil.size() > 2 && morph_abil.compare(morph_abil.size() - 2, 2, "Up") == 0) ||
                           (!bup.empty() && morph_abil == bup);
    if (const auto mg = cat_->merge_spec(morph_abil)) {
        std::vector<std::uint32_t> two;
        for (const Unit& u : world_.units())
            if (u.alive && u.player == cfg_.human_player && u.catalog_id == from_unit && !world_.is_morphing(u.id)) two.push_back(u.id);
        if (two.size() < 2 || mg->target.empty()) return false;
        const std::int32_t proto = world_.add_spawn_proto(make_unit(*cat_, mg->target, cfg_.human_player));
        return world_.merge_units(two[0], two[1], proto, sim::Fixed::from_double(mg->time));
    }
    for (const Unit& u : world_.units())
        if (u.alive && u.player == cfg_.human_player && u.catalog_id == from_unit && !world_.is_morphing(u.id) &&
            !world_.is_burrowing(u.id)) {
            if (burrow_down || burrow_up) {
                if (u.burrowed == burrow_down) continue;
                return human_burrow(u.id, burrow_down);
            }
            if (const std::string dl = cat_->depot_lower_ability(from_unit); !dl.empty()) {
                const auto dms = cat_->morph_spec(dl);
                const bool to_down = morph_abil == dl;
                if (to_down || (dms && morph_abil == cat_->morph_button_ability(dms->target, "Raise"))) {
                    if (u.lowered == to_down || world_.is_depot_morphing(u.id)) continue;
                    return human_depot(u.id, to_down);
                }
            }
            if (at && morph_abil == "LiberatorMorphtoAG")
                return human_siege_defender(u.id, sim::Fixed::from_double(tx), sim::Fixed::from_double(ty));
            return human_morph(u.id, morph_abil);
        }
    return false;
}

void MeleeGame::snapshot_positions(std::vector<render::Vec3>& out) const {
    const std::vector<Unit>& units = world_.units();
    if (out.size() < units.size()) out.resize(units.size());
    for (const Unit& u : units)
        out[u.id] = {float(u.x.to_double()), float(u.y.to_double()), 0.0f};
}

static void emit_range_ring(std::vector<render::SceneInstance>& scene, float x, float y, float z, float radius) {
    if (radius <= 0.0f) return;
    const float band = 0.09f;
    const float inner = radius > band ? (radius - band) / radius : 0.0f;
    const int key = int(inner * 2000.0f + 0.5f);
    static std::map<int, formats::Model> ring_cache;
    auto it = ring_cache.find(key);
    if (it == ring_cache.end()) it = ring_cache.emplace(key, render::make_ring_model(inner, 128)).first;
    render::SceneInstance ring;
    ring.model = &it->second;
    ring.world = render::translation(x, y, z + 0.05f) * render::scaling(radius, radius, 1.0f);
    ring.team = {1.0f, 1.0f, 1.0f, 2.0f};
    ring.seq = -1;
    scene.push_back(ring);
}

static void emit_beam(std::vector<render::SceneInstance>& scene, const float a[3], const float b[3],
                      float width) {
    static formats::Model quad = render::make_beam_model();
    render::SceneInstance s;
    s.model = &quad;
    s.world = render::beam_transform(a[0], a[1], a[2], b[0], b[1], b[2], width);
    s.team = {1.0f, 1.0f, 1.0f, 2.0f};
    s.seq = -1;
    scene.push_back(s);
}

float MeleeGame::display_height(const std::string& id, bool flying_variant) {
    std::unordered_map<std::string, float>& cache = flying_variant ? flying_height_cache_ : display_h_cache_;
    auto hit = cache.find(id);
    if (hit == cache.end())
        hit = cache.emplace(id, cat_->unit_display_height(flying_variant ? id + "Flying" : id)).first;
    return hit->second;
}

bool MeleeGame::unit_attach_world(const sim::Unit& u, const char* first, const char* second, float out[3]) {
    out[0] = float(u.x.to_double());
    out[1] = float(u.y.to_double());
    out[2] = (u.flying ? air_z_at(out[0], out[1]) : ground_z_at(out[0], out[1]));
    if (cat_) {
        float h = display_height(u.catalog_id, false);
        if (u.flying && h <= 0.0f) h = display_height(u.catalog_id, true);
        if (h > 0.0f) out[2] = air_z_at(out[0], out[1]) + h;
    }
    if (u.id >= driver_.actors.size()) return false;
    render::ActorInstance& a = driver_.actors[u.id];
    const int seq = a.model ? a.current_sequence() : -1;
    if (seq < 0) return false;
    render::Mat4 att;
    if (!(first && render::attachment_transform(*a.model, first, std::uint32_t(seq), a.current_time(), att)) &&
        !(second && render::attachment_transform(*a.model, second, std::uint32_t(seq), a.current_time(), att)))
        return false;
    const float c = std::cos(a.facing + 1.5707963f), s = std::sin(a.facing + 1.5707963f);
    out[0] += att.m[12] * c - att.m[13] * s;
    out[1] += att.m[12] * s + att.m[13] * c;
    out[2] += att.m[14];
    return true;
}

void MeleeGame::beam_refresh(std::uint32_t attacker, std::uint32_t target, std::uint8_t slot, const std::string& key,
                             bool heal, float keep_ms) {
    if (!models_ || !ui_store_ || attacker >= world_.units().size()) return;
    for (BeamFx& b : beams_)
        if (b.attacker == attacker && b.slot == slot && b.heal == heal && b.phase < 2) {
            b.idle_ms = 0.0f;
            if (b.continuous) { b.target = target; return; }
            if (b.target == target) return;
        }
    const MeleeModels::BeamModel bm = heal ? models_->actor_beam_model(*ui_store_, key) : models_->beam_model(*ui_store_, key);
    if (!bm.model) return;
    BeamFx b;
    b.model = bm.model;
    b.attacker = attacker;
    b.target = target;
    b.slot = slot;
    b.heal = heal;
    b.continuous = bm.continuous;
    b.scale = bm.scale;
    b.keep_ms = keep_ms;
    b.seq[0] = render::select_sequence(*bm.model, {"Birth"}, attacker);
    b.seq[1] = render::select_sequence(*bm.model, {"Stand"}, attacker);
    b.seq[2] = render::select_sequence(*bm.model, {"Death"}, attacker);
    if (b.seq[1] < 0) b.seq[1] = b.seq[0] >= 0 ? b.seq[0] : (bm.model->sequences.empty() ? -1 : 0);
    b.phase = b.seq[0] >= 0 ? 0 : 1;
    for (std::size_t i = 0; i < bm.model->bones.size(); ++i)
        if (bm.model->bones[i].name == "Attacher") { b.beam_bone = int(i); break; }
    b.player = world_.units()[attacker].player;
    if (beams_.size() < 256) beams_.push_back(b);
}

void MeleeGame::emit_scene(std::vector<render::SceneInstance>& scene) {
    OMC_PROF_SCOPE("emit_scene");
    rebuild_subgroups();
    ++pick_frame_;
    const data::Catalog::SelectionLook sel_look = cat_ ? cat_->selection_look()
                                                       : data::Catalog::SelectionLook{};
    const bool env_sel_line_003 = OMC_ENV_ONCE("OMC_SELECTION_LINE_003") != nullptr;
    const bool env_sel_width_ratio = OMC_ENV_ONCE("OMC_SELECTION_WIDTH_RATIO") != nullptr;
    const bool env_sel_rim_uniform = OMC_ENV_ONCE("OMC_SELECTION_RIM_UNIFORM") != nullptr;
    const bool env_sel_band_flat = OMC_ENV_ONCE("OMC_SELECTION_BAND_FLAT") != nullptr;
    const bool env_sel_band_radial = OMC_ENV_ONCE("OMC_SELECTION_BAND_RADIAL") != nullptr;
    const bool pass_tags = render::gpu_prof_switch() || render::render_quality().shadows == render::ShadowQuality::Units;
    auto tag_pass = [&scene, pass_tags](std::size_t from, render::PassGroup g) {
        if (!pass_tags) return scene.size();
        for (std::size_t i = from; i < scene.size(); ++i) scene[i].pass_group = std::uint8_t(g);
        return scene.size();
    };
    scene.clear();
    if (ground_) {
        render::SceneInstance g;
        g.model = ground_;
        g.world = render::Mat4::identity();
        g.team = {0, 0, 0, -1.0f};
        g.seq = -1;
        scene.push_back(g);
    }
    if (roads_ && !roads_->regions.empty() && roads_->vertex_count) {
        render::SceneInstance r;
        r.model = roads_;
        r.world = render::Mat4::identity();
        r.team = {0.0f, 0.0f, 0.0f, -1.0f};
        r.seq = -1;
        scene.push_back(r);
    }
    if (water_ && !water_->regions.empty() && water_->vertex_count) {
        render::SceneInstance w;
        w.model = water_;
        w.world = render::Mat4::identity();
        w.team = {water_rgba_[0], water_rgba_[1], water_rgba_[2], 2.0f};
        w.seq = -1;
        if (ui_store_ && !water_look_.resolved) {
            water_look_.resolved = true;
            for (const auto& kv : water_type_materials(*ui_store_, map_water_xml_.empty() ? nullptr : &map_water_xml_)) {
                const WaterMaterial& wm = kv.second;
                if (!wm.has || std::fabs(wm.color[0] - water_rgba_[0]) > 1e-4f || std::fabs(wm.color[1] - water_rgba_[1]) > 1e-4f ||
                    std::fabs(wm.color[2] - water_rgba_[2]) > 1e-4f)
                    continue;
                water_look_.has = true;
                water_look_.color_falloff = wm.color_falloff;
                water_look_.specularity = wm.specularity;
                water_look_.specular_scaler = wm.specular_scaler;
                water_look_.reflectivity_power = wm.reflectivity_power;
                water_look_.min_reflection = wm.min_reflection;
                water_look_.reflection_distortion = wm.reflection_distortion;
                water_look_.refraction_distortion = wm.refraction_distortion;
                water_look_.uv_rotate = wm.uv_rotate;
                water_look_.receive_shadows = wm.receive_shadows;
                water_look_.caustics_falloff = wm.caustics_falloff;
                water_look_.caustics_tiling = wm.caustics_tiling[0];
                water_look_.caustics_fps = wm.caustics_fps;
                break;
            }
        }
        if (water_look_.has) {
            if (water_fx_source_ != water_) {
                water_fx_source_ = water_;
                water_fx_model_ = *water_;
                formats::M3Material mat;
                mat.name = "CWater";
                formats::M3Layer nl;
                nl.usage = "Normal";
                nl.image_path = "Assets/Textures/DefaultWater.dds";
                mat.layers.push_back(nl);
                water_fx_model_.materials.assign(1, mat);
                water_fx_model_.region_material.assign(water_fx_model_.regions.size(), 0);
            }
            w.model = &water_fx_model_;
            static const bool kCaustics = std::getenv("OMC_NO_WATER_CAUSTICS") == nullptr;
            if (kCaustics && OMC_ENV_ONCE("OMC_WATER_CAUSTICS_TRACE")) {
                static bool once = false;
                if (!once) {
                    once = true;
                    std::fprintf(stderr, "[caustics] falloff %.3f tiling %.3f fps %.1f has %d\n", double(water_look_.caustics_falloff),
                                 double(water_look_.caustics_tiling), double(water_look_.caustics_fps), int(water_look_.has));
                }
            }
            if (kCaustics && water_look_.caustics_falloff > 0.0f && water_look_.caustics_tiling > 0.0f) {
                if (water_caustics_models_.empty()) {
                    for (int k = 1; k <= 32; ++k) {
                        formats::Model fm = water_fx_model_;
                        formats::M3Layer dl;
                        static const bool caus_srgb = std::getenv("OMC_WATER_CAUSTICS_SRGB") != nullptr;
                        dl.usage = caus_srgb ? "Diffuse" : "AlphaMask";
                        char nm[96];
                        std::snprintf(nm, sizeof nm, "Assets/Textures/DefaultWaterCaustics_%03d.dds", k);
                        dl.image_path = nm;
                        fm.materials[0].layers.push_back(dl);
                        water_caustics_models_.push_back(std::move(fm));
                    }
                }
                const float fps = water_look_.caustics_fps > 0.0f ? water_look_.caustics_fps : 15.0f;
                const std::size_t frame = std::size_t(std::floor(clock_ms_ * 0.001 * fps)) % water_caustics_models_.size();
                w.model = &water_caustics_models_[frame];
                w.fx.emis_uv[0] = water_look_.caustics_tiling;
                w.fx.emis_uv[1] = water_look_.caustics_falloff;
                w.fx.emis_uv[2] = OMC_ENV_ONCE("OMC_WATER_CAUSTICS_SRGB") ? 0.0f : 1.0f;
            }
            static const bool shadow_always = std::getenv("OMC_WATER_SHADOW_ALWAYS") != nullptr;
            w.team = {(water_look_.receive_shadows || shadow_always) ? 1.0f : 0.0f, 0.0f, 0.0f, -1.0f};
            w.fx.kind = render::FxKind::Water;
            w.fx.t = float(clock_ms_ * 0.001);
            for (int k = 0; k < 4; ++k) w.fx.p0[k] = water_rgba_[std::size_t(k)];
            w.fx.p1[0] = water_look_.specularity;
            w.fx.p1[1] = water_look_.specular_scaler;
            w.fx.p1[2] = water_look_.reflectivity_power;
            w.fx.p1[3] = water_look_.min_reflection;
            w.fx.aux = water_look_.color_falloff;
            w.fx.uv_tiling[0] = water_look_.tiling[0];
            w.fx.uv_tiling[1] = water_look_.tiling[1];
            w.fx.ext[0] = water_look_.refraction_distortion;
            w.fx.ext[1] = water_look_.reflection_distortion;
            w.fx.uv_angle = water_look_.uv_rotate;
            for (int k = 0; k < 4; ++k) w.fx.team_emissive[k] = water_look_.scroll[k];
        }
        scene.push_back(w);
    }
    tag_pass(0, render::PassGroup::Terrain);
    const bool splat_fog_ready = cfg_.human_player >= 0 && vision_.width() > 0;
    static const bool kFogLeakOld = std::getenv("OMC_FOG_LEAK_OLD") != nullptr;
    static const bool kSnapshotOldEarly = std::getenv("OMC_FOG_SNAPSHOT_OLD") != nullptr;
    const bool fog_leak_guard = !kFogLeakOld && !kSnapshotOldEarly && !reveal_map_ && ground_fog_ && splat_fog_ready;
    auto fog_hides_enemy = [&](int player, float x, float y, int fv) {
        return fog_leak_guard && player >= 0 && player != cfg_.human_player && !world_.are_allied(player, cfg_.human_player) &&
               FogVisibility(fv) != FogVisibility::Visible &&
               vision_.state_at(sim::Fixed::from_double(x), sim::Fixed::from_double(y)) != sim::Visibility::Visible;
    };
    fog_splat_frame_.clear();
    if (models_ && ui_store_)
        for (const Unit& u : world_.units()) {
            static const bool kNeutralSplats = std::getenv("OMC_NO_ACTOR_SPLAT") == nullptr;
            static const bool kBurrowSplat = std::getenv("OMC_NO_BIRTH_SPLAT") == nullptr;
            const bool dug = kBurrowSplat && u.burrowed && cat_;
            if (!dug) splat_quads_.erase(u.id | 0x80000000u);
            if (!u.alive || (u.player < 0 && !kNeutralSplats) || (u.speed.raw() != 0 && !dug) || u.flying) continue;
            if (splat_fog_ready && u.player != cfg_.human_player &&
                !hud_fog_body_drawn(fog_body_state(u.player, int(vision_.state_at(u.x, u.y)), fog_hidden_black_), FogVisibility(u.fog_visibility)))
                continue;
            if (fog_hides_enemy(u.player, float(u.x.to_double()), float(u.y.to_double()), int(u.fog_visibility)) &&
                !(u.id < fog_seen_.size() && fog_seen_[u.id]))
                continue;
            const formats::Model* q = nullptr;
            if (dug) {
                sim::Unit v = u;
                v.catalog_id = cat_->burrowed_variant(u.catalog_id);
                v.id = u.id | 0x80000000u;
                if (!v.catalog_id.empty()) q = splat_quad_for(v);
            } else {
                q = splat_quad_for(u);
            }
            if (!q) continue;
            render::SceneInstance inst;
            inst.model = q;
            inst.world = render::translation(float(u.x.to_double()), float(u.y.to_double()), 0.03f);
            inst.team = {0.0f, 0.0f, 0.0f, -1.0f};
            static const bool kSplatTeam = std::getenv("OMC_SPLAT_NO_TEAM") == nullptr;
            if (kSplatTeam && u.player >= 0 && (u.attributes & sim::kAttrStructure) != 0) inst.team = team_colour_for(cat_, u.player, false);
            inst.seq = -1;
            scene.push_back(inst);
            if (fog_leak_guard && !dug && u.player >= 0 && u.player != cfg_.human_player && FogVisibility(u.fog_visibility) != FogVisibility::Visible)
                fog_splat_frame_[u.id] = inst;
        }
    if (fog_leak_guard)
        for (const auto& kv : fog_snaps_)
            if (kv.second.has_splat &&
                vision_.state_at(sim::Fixed::from_double(kv.second.x), sim::Fixed::from_double(kv.second.y)) != sim::Visibility::Visible)
                scene.push_back(kv.second.splat);

    if (models_ && ui_store_ && cfg_.human_player >= 0) {
        bool show = pending_build_ && cat_ && !build_proto_.catalog_id.empty() &&
                    cat_->unit_race_index(build_proto_.catalog_id) == 1;
        static const bool kWarpPresentOld = std::getenv("OMC_WARPIN_PRESENT_OLD") != nullptr;
        if (!show && !kWarpPresentOld && pending_warp_) show = true;
        if (!show)
            for (uint32_t s : selection_)
                if (s < world_.units().size() && world_.units()[s].power_radius.raw() > 0) { show = true; break; }
        if (show)
            for (const Unit& u : world_.units()) {
                if (!u.alive || u.constructing || u.player != cfg_.human_player || u.speed.raw() != 0 ||
                    u.power_radius.raw() <= 0)
                    continue;
                const formats::Model* q = power_quad_for(u);
                if (!q) continue;
                render::SceneInstance inst;
                inst.model = q;
                inst.world = render::translation(float(u.x.to_double()), float(u.y.to_double()), 0.04f);
                inst.team = {0.0f, 0.0f, 0.0f, -1.0f};
                inst.seq = -1;
                scene.push_back(inst);
            }
    }
    if (const formats::Model* wgm = warp_ghost_model()) {
        const float gx = warp_ghost_.x, gy = warp_ghost_.y;
        const std::array<float, 3> gs = data::resolve_unit_model_scale(*cat_, data::unit_actor_id(*cat_, warp_ghost_.unit_id));
        render::SceneInstance inst;
        inst.model = wgm;
        inst.world = render::translation(gx, gy, ground_z_at(gx, gy)) *
                     render::rotation_z(cat_->unit_facing_radians(warp_ghost_.unit_id) + 1.5707963f) *
                     render::scaling(gs[0], gs[1], gs[2]);
        inst.seq = render::select_sequence(*wgm, {"Stand"}, 0);
        if (inst.seq >= 0 && std::size_t(inst.seq) < wgm->sequences.size()) {
            const float len = float(wgm->sequences[std::size_t(inst.seq)].length_ms());
            inst.time_ms = len > 0.0f ? float(std::fmod(clock_ms_, double(len))) : 0.0f;
        }
        inst.team = team_colour_for(cat_, cfg_.human_player, false);
        inst.team_emissive = team_emissive_for(cat_, cfg_.human_player);
        inst.opacity = 0.5f;
        inst.tint = warp_ghost_.ok ? std::array<float, 3>{0.6f, 1.0f, 0.6f} : std::array<float, 3>{1.0f, 0.45f, 0.45f};
        scene.push_back(inst);
    }

    {
        static const float kCreepDrawLift = std::getenv("OMC_CREEP_LIFT") ? float(std::atof(std::getenv("OMC_CREEP_LIFT"))) : 0.01f;
        constexpr float kCreepUvTilesPerCell = 1.0f;
        refresh_creep_seen();
        const sim::CreepGrid& cg = creep_fogged_ ? creep_seen_ : world_.creep().grid();
        if (creep_fogged_ && creep_seen_dirty_) {
            ++creep_seen_gen_;
            creep_seen_dirty_ = false;
        }
        const std::uint64_t sum = creep_fogged_ ? (0x9E3779B97F4A7C15ull ^ creep_seen_gen_) : cg.checksum();
        if (!creep_model_built_ || sum != creep_drawn_checksum_) {
            static const bool kCreepPerCell = std::getenv("OMC_CREEP_UV_PER_CELL") != nullptr;
            static const float kCreepUvAngle = (std::getenv("OMC_CREEP_UV_ANGLE") ? float(std::atof(std::getenv("OMC_CREEP_UV_ANGLE"))) : -15.0f) * 0.01745329252f;
            static const float kCreepUvSpan = std::getenv("OMC_CREEP_UV_SPAN") ? float(std::atof(std::getenv("OMC_CREEP_UV_SPAN"))) : 91.5f;
            static const bool kCreepCentre = std::getenv("OMC_CREEP_CENTRE_HEIGHTS") != nullptr;
            creep_model_ = render::build_creep_model(
                cg, [this](float wx, float wy) { return kCreepCentre ? ground_z_at(wx, wy) : vertex_ground_at(wx, wy); }, kCreepDrawLift,
                kCreepPerCell ? kCreepUvTilesPerCell : 10.0f, creep_diffuse_,  1,
                kCreepPerCell ? 0.0f : kCreepUvSpan, kCreepPerCell ? 0.0f : kCreepUvSpan, kCreepUvAngle);
            if (!creep_model_.materials.empty() && cat_) {
                const auto slot = [&](const char* field, const char* usage) {
                    auto v = cat_->tag_default_scalar("CTerrain", field);
                    if (!v || v->empty()) return;
                    formats::M3Layer L;
                    L.usage = usage;
                    L.image_path = *v;
                    for (char& c : L.image_path)
                        if (c == '\\') c = '/';
                    creep_model_.materials[0].layers.push_back(L);
                };
                slot("CreepEdgeNormalMap", "Normal");
                slot("CreepBaseSpecularMap", "Specular");
                slot("CreepNoiseMap", "AlphaMask");
                slot("CreepHeightMap", "AlphaMask2");
            }
            creep_coverage_.w = std::uint32_t(std::max(0, cg.width()));
            creep_coverage_.h = std::uint32_t(std::max(0, cg.height()));
            creep_coverage_.world_w = float(cg.cell_size().to_double()) * float(cg.width());
            creep_coverage_.world_h = float(cg.cell_size().to_double()) * float(cg.height());
            creep_coverage_.alpha.assign(std::size_t(creep_coverage_.w) * creep_coverage_.h, 0);
            for (int cy = 0; cy < cg.height(); ++cy)
                for (int cx = 0; cx < cg.width(); ++cx)
                    if (cg.is_creep_cell(cx, cy)) creep_coverage_.alpha[std::size_t(cy) * creep_coverage_.w + std::size_t(cx)] = 255;
            if (std::getenv("OMC_CREEP_TRACE")) {
                std::string hex;
                static const char* kHex = "0123456789abcdef";
                std::uint32_t acc = 0, nb = 0, cells = 0;
                for (int cy = 0; cy < cg.height(); ++cy)
                    for (int cx = 0; cx < cg.width(); ++cx) {
                        const bool on = cg.is_creep_cell(cx, cy);
                        cells += on ? 1u : 0u;
                        acc = (acc << 1) | (on ? 1u : 0u);
                        if (++nb == 8) { hex.push_back(kHex[(acc >> 4) & 15]); hex.push_back(kHex[acc & 15]); acc = 0; nb = 0; }
                    }
                if (nb) { acc <<= (8 - nb); hex.push_back(kHex[(acc >> 4) & 15]); hex.push_back(kHex[acc & 15]); }
                if (!creep_model_.materials.empty())
                    for (const formats::M3Layer& L : creep_model_.materials[0].layers) std::fprintf(stderr, "[creeptex] %s = %s\n", L.usage.c_str(), L.image_path.c_str());
                std::fprintf(stderr, "[creep] tick %llu w %d h %d cells %u hex %s\n", static_cast<unsigned long long>(world_.tick()), cg.width(), cg.height(), cells, hex.c_str());
            }
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
            if (creep_coverage_.valid()) {
                c.fx.kind = render::FxKind::Creep;
                c.fx.p0[0] = float(cg.origin_x().to_double());
                c.fx.p0[1] = float(cg.origin_y().to_double());
                c.fx.p0[2] = creep_coverage_.world_w;
                c.fx.p0[3] = creep_coverage_.world_h;
                c.fx.p1[0] = 0.45f;
                static const bool kCreepStatic = std::getenv("OMC_CREEP_STATIC") != nullptr;
                static const float kCreepHeightStrength = std::getenv("OMC_CREEP_HEIGHT_STRENGTH") ? float(std::atof(std::getenv("OMC_CREEP_HEIGHT_STRENGTH"))) : 15.0f;
                static const float kCreepNoiseRate = std::getenv("OMC_CREEP_NOISE_RATE") ? float(std::atof(std::getenv("OMC_CREEP_NOISE_RATE"))) : 0.0255f;
                static const bool kCreepModelLambert = std::getenv("OMC_CREEP_MODEL_LAMBERT") != nullptr;
                c.fx.t = float(clock_ms_ * 0.001);
                c.fx.team_emissive[0] = kCreepNoiseRate;
                c.fx.team_emissive[1] = kCreepStatic ? 0.0f : kCreepHeightStrength;
                c.fx.team_emissive[2] = 0.45f;
                c.fx.team_emissive[3] = kCreepModelLambert ? 0.0f : 0.25f;
                static const bool kCreepNoiseRim = std::getenv("OMC_CREEP_NOISE_RIM") != nullptr;
                c.fx.p1[1] = (kCreepNoiseRim || OMC_ENV_ONCE("OMC_CREEP_UV_PER_CELL")) ? 0.0f : 1.0f;
                c.fx.p1[2] = 0.2f;
                c.fx.p1[3] = 0.0f;
                c.coverage = &creep_coverage_;
            }
            scene.push_back(c);
        }
    }
    const float tick_ms =
        (1000.0f / float(sim::World::kTicksPerSecond)) / float(clock_.multiplier(clock_.speed()));
    const float alpha = tick_ms > 0.0f ? std::min(1.0f, accum_ms_ / tick_ms) : 0.0f;
    const bool fog_ready = ground_fog_ && cfg_.human_player >= 0 && vision_.width() > 0;
    static const bool kFogSnapshotOld = std::getenv("OMC_FOG_SNAPSHOT_OLD") != nullptr;
    const bool fog_snapshots = fog_ready && !kFogSnapshotOld && !reveal_map_;
    const std::size_t units_from = scene.size();
    if (!fog_snapshots) {
        fog_snaps_.clear();
        fog_seen_.clear();
    } else {
        if (fog_seen_.size() < world_.units().size()) fog_seen_.resize(world_.units().size(), 0);
        for (auto it = fog_snaps_.begin(); it != fog_snaps_.end();) {
            const FogSnapshot& s = it->second;
            if (vision_.state_at(sim::Fixed::from_double(s.x), sim::Fixed::from_double(s.y)) == sim::Visibility::Visible) {
                if (s.has_splat && (it->first >= world_.units().size() || !world_.units()[it->first].alive)) splat_quads_.erase(it->first);
                it = fog_snaps_.erase(it);
                continue;
            }
            scene.push_back(s.inst);
            ++it;
        }
    }
    bind_cloak_look();
    cloak_tx_.begin_frame();
    for (const Unit& u : world_.units()) {
        if (!u.alive) continue;
        bool snapshot_capture = false;
        if (fog_snapshots && u.player >= 0 && u.player != cfg_.human_player && !world_.are_allied(u.player, cfg_.human_player) &&
            FogVisibility(u.fog_visibility) != FogVisibility::Visible) {
            if (vision_.state_at(u.x, u.y) == sim::Visibility::Visible) {
                fog_seen_[u.id] = 1;
            } else if (fog_seen_[u.id]) {
                fog_seen_[u.id] = 0;
                snapshot_capture = true;
            } else {
                continue;
            }
        } else if (fog_ready && u.player != cfg_.human_player &&
                   !hud_fog_body_drawn(fog_body_state(u.player, int(vision_.state_at(u.x, u.y)), fog_hidden_black_),
                                       FogVisibility(u.fog_visibility))) {
            continue;
        }
        float body_opacity = 1.0f;
        bool cloak_fx = false;
        bool cloak_enemy = false;
        render::CloakView cloak_view = render::CloakView::None;
        if ((u.cloaked_effective() || u.burrowed) && cfg_.human_player >= 0) {
            const bool own_side = u.player == cfg_.human_player || world_.are_allied(u.player, cfg_.human_player);
            if (!own_side && !world_.revealed_to(cfg_.human_player, u.id)) {
                if (!cloak_enemy_drawn(u)) continue;
                cloak_enemy = true;
            }
            static const bool kBurrowShimmer = std::getenv("OMC_BURROW_SHIMMER") != nullptr;
            if (u.cloaked_effective() && (kBurrowShimmer || !u.burrowed)) {
                cloak_fx = true;
                cloak_view = own_side ? render::CloakView::Ally : cloak_enemy ? render::CloakView::Enemy : render::CloakView::Detected;
            }
            static const char* const kBurrowOpacity = std::getenv("OMC_BURROW_OPACITY");
            if (u.burrowed && own_side && kBurrowOpacity) body_opacity = float(std::atof(kBurrowOpacity));
        }
        if (u.in_gas_structure) continue;
        if (u.loaded && world_.cargo_hidden()) continue;
        if (geyser_covered(u)) continue;
        if (u.gathers_gas && u.mine_cd.raw() > 0 && u.gather_target != sim::kNoTarget &&
            u.gather_target < world_.units().size() && world_.units()[u.gather_target].is_gas)
            continue;
        const render::CloakTransitions::Out cloak_state = cloak_tx_.update(u.id, cloak_view, cloak_clock_ms_);
        cloak_fx = cloak_state.cloak_fx;
        render::SceneInstance inst;
        const formats::Model* m =
            (u.id < driver_.actors.size()) ? driver_.actors[u.id].model : nullptr;
        inst.model = m;
        const sim::Unit* shown_u = world_.morph_origin(u.id);
        const std::string& shown_id = shown_u ? shown_u->catalog_id : u.catalog_id;
        const bool shown_flying = shown_u ? shown_u->flying : u.flying;
        const std::array<float, 3> ms = unit_draw_scale(cat_, shown_id);
        const float footprint_scale = std::max(ms[0], ms[1]);
        const float native_span = cached_diameter(model_diam_cache_, m);
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
            wz = display_height(shown_id, false);
            if (shown_flying && wz <= 0.0f) {
                const float fh = display_height(shown_id, true);
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
            if (morph_transition_models_ && world_.is_morphing(u.id) && !world_.is_morph_delayed(u.id)) {
                const std::string to = world_.morph_target_id(u.id);
                const float th = to.empty() ? wz : display_height(to, false);
                if (!to.empty() && th != wz) {
                    const float el = float((u.morph_total - u.morph_timer).to_double());
                    const float s = std::min(1.0f, std::max(0.0f, el / 1.0f));
                    wz = wz + (th - wz) * s;
                }
            }
        }
        wz += (shown_flying || wz > 0.0f) ? air_z_at(wx, wy) : ground_z_at(wx, wy);
        inst.world = render::translation(wx, wy, wz) * render::scaling(ms[0], ms[1], ms[2]);
        if (u.id < driver_.actors.size() && m) {
            render::ActorInstance& a = driver_.actors[u.id];
            inst.world =
                render::translation(wx, wy, wz) * a.orient() * render::scaling(ms[0], ms[1], ms[2]);
            inst.seq = a.current_sequence();
            if (u.constructing) {
                const auto& ladder = construction_ladder(u.catalog_id);
                if (u.warp_forming) {
                    if (a.construction_stage != 0) {
                        a.anim_play("Construction", {"Birth"},  false,  1);
                        a.construction_stage = 0;
                    }
                    inst.seq = a.current_sequence();
                    if (inst.seq >= 0 && std::size_t(inst.seq) < m->sequences.size()) {
                        double p = u.build_progress.to_double();
                        if (!(p >= 0.0)) p = 0.0;
                        if (p > 1.0) p = 1.0;
                        inst.time_ms = float(p) * float(m->sequences[std::size_t(inst.seq)].length_ms());
                    }
                } else if (!ladder.empty()) {
                    double p = u.build_progress.to_double();
                    if (!(p >= 0.0)) p = 0.0;
                    if (p > 1.0) p = 1.0;
                    int stage = int(p * double(ladder.size()));
                    if (stage >= int(ladder.size())) stage = int(ladder.size()) - 1;
                    stage = timed_ladder_stage(*m, ladder, p, u.build_time.to_double() * 1000.0, stage);
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
            inst.global_layers = a.global_layers();
            static const bool kGlobalLayerTrace = std::getenv("OMC_GLOBAL_LAYER_TRACE") != nullptr;
            if (kGlobalLayerTrace && !inst.global_layers.empty()) {
                static int ng = 0;
                if (ng++ < 24)
                    std::fprintf(stderr, "[gl] tick %d unit %u %s seq %d t %.0f | layers %zu first seq %u t %.0f\n", world_.tick(), unsigned(u.id), u.catalog_id.c_str(),
                                 inst.seq, double(inst.time_ms), inst.global_layers.size(), unsigned(inst.global_layers[0].seq), double(inst.global_layers[0].time_ms));
            }
        }
        bool selected = false;
        for (uint32_t s : selection_)
            if (s == u.id) { selected = true; break; }
        bool in_active_subgroup = false;
        if (selected && sel_groups_.active < sel_groups_.subgroups.size())
            for (uint32_t s : sel_groups_.subgroups[sel_groups_.active].units)
                if (s == u.id) { in_active_subgroup = true; break; }
        if (u.id < driver_.actors.size() && m) {
            render::ActorInstance& ba = driver_.actors[u.id];
            if (!ba.blend_time_set) {
                ba.blend_time_set = true;
                if (cat_)
                    if (const auto bt = cat_->unit_anim_blend_time(u.catalog_id)) ba.blend_time_ms = *bt * 1000.0f;
            }
            ba.note_pose(inst.seq, inst.time_ms);
            inst.blend_seq = ba.prev_seq;
            inst.blend_time_ms = ba.prev_time_ms;
            inst.blend_weight = ba.blend_weight();
            if (!ba.turret_bound) {
                ba.turret_bound = true;
                if (cat_ && !u.catalog_id.empty() && !u.wpn_id.empty()) {
                    const std::string tid = cat_->weapon_turret(u.catalog_id, u.wpn_id);
                    if (const auto ts = tid.empty() ? std::nullopt : cat_->turret(tid)) {
                        const int att = render::attachment_index(*m, "TurretZ");
                        if (att >= 0 && std::size_t(att) < m->attachments.size()) {
                            ba.turret_bone = int(m->attachments[std::size_t(att)].bone);
                            const float d2r = 3.1415927f / 180.0f;
                            ba.turret_rate_rad = float(ts->yaw_rate) * d2r;
                            ba.turret_arc_rad = float(ts->yaw_arc) * d2r;
                            ba.turret_start_rad = float(ts->yaw_start) * d2r;
                            ba.turret_idle_rate_rad = float(ts->yaw_idle_rate) * d2r;
                            ba.turret_spin = ts->idle == "Spin";
                            ba.turret_hold = ts->idle == "Hold" && !std::getenv("OMC_TURRET_HOLD_RESETS");
                            if (!std::getenv("OMC_NO_TURRET_FIDGET")) {
                                ba.turret_fidget_min_s = float(ts->fidget_delay_min);
                                ba.turret_fidget_max_s = float(ts->fidget_delay_max);
                                ba.turret_fidget_turn_rad = float(ts->fidget_turn_angle) * d2r;
                                ba.turret_fidget_rate_rad = float(ts->fidget_turning_rate) * d2r;
                                ba.turret_fidget_chance = float(ts->fidget_turn_chance);
                            }
                            ba.turret_yaw = ba.turret_start_rad;
                        }
                    }
                }
            }
            if (ba.turret_bone >= 0) {
                ba.turret_has_goal = false;
                if (u.target < world_.units().size() && world_.units()[u.target].alive) {
                    const Unit& t = world_.units()[u.target];
                    const float tx = float(t.x.to_double()) - wx, ty = float(t.y.to_double()) - wy;
                    if (tx != 0.0f || ty != 0.0f) {
                        ba.turret_goal = std::atan2(ty, tx) - ba.facing;
                        ba.turret_has_goal = true;
                    }
                }
                inst.turret_bone = ba.turret_bone;
                inst.turret_yaw = ba.turret_yaw;
            }
            static const bool no_reach = std::getenv("OMC_NO_TENTACLE_REACH") != nullptr;
            if (!no_reach && !ba.reach_bound) {
                ba.reach_bound = true;
                if (cat_ && !u.wpn_id.empty()) {
                    bool ret = false;
                    for (const core::XmlNode& n : cat_->resolved("CEffect", cat_->weapon_effect_id(u.wpn_id)))
                        if (n.name == "Flags" && n.attr_or("index", "") == "Return" && n.attr_or("value", "0") == "1") ret = true;
                    const int tz = ret ? render::attachment_index(*m, "TurretZ") : -1;
                    const int wa = ret ? render::attachment_index(*m, "Weapon") : -1;
                    if (tz >= 0 && std::size_t(tz) < m->attachments.size()) {
                        const int root = int(m->attachments[std::size_t(tz)].bone);
                        auto is_point = [&](int b) {
                            const std::string& nm = m->bones[std::size_t(b)].name;
                            return nm.rfind("Ref_", 0) == 0 || nm.rfind("Dummy", 0) == 0 || nm == "Attacher";
                        };
                        std::function<int(int)> depth = [&](int b) {
                            int best = 0;
                            for (std::size_t c = 0; c < m->bones.size(); ++c)
                                if (m->bones[c].parent == b && !is_point(int(c))) best = std::max(best, 1 + depth(int(c)));
                            return best;
                        };
                        int tip = root;
                        for (int guard = 0; guard < int(m->bones.size()); ++guard) {
                            int next = -1, nd = -1;
                            for (std::size_t c = 0; c < m->bones.size(); ++c)
                                if (m->bones[c].parent == tip && !is_point(int(c))) {
                                    const int d = depth(int(c));
                                    if (d > nd) { nd = d; next = int(c); }
                                }
                            if (next < 0) break;
                            tip = next;
                        }
                        if (tip != root) {
                            ba.reach_root = root;
                            ba.reach_tip = tip;
                            const std::string lm = cat_->weapon_effect_id(u.wpn_id);
                            ba.retract_delay = float(std::atof(cat_->scalar("CEffect", lm, "ReturnDelay").value_or("0").c_str()));
                            const auto rm = cat_->effect_missile_movers(lm, "ReturnMovers");
                            for (const auto& mv : rm) {
                                if (ba.ret_n >= 2) break;
                                ba.ret_lte[ba.ret_n] = float(mv.range_lte);
                                ba.ret_v0[ba.ret_n] = float(mv.speed);
                                ba.ret_acc[ba.ret_n] = float(mv.accel);
                                ba.ret_vmax[ba.ret_n] = float(mv.max_speed);
                                ++ba.ret_n;
                            }
                            if (wa >= 0 && std::size_t(wa) < m->attachments.size()) ba.reach_weapon = int(m->attachments[std::size_t(wa)].bone);
                        }
                    }
                }
            }
            if (!no_reach && ba.reach_tip >= 0) {
                bool live = false;
                float hx = 0.0f, hy = 0.0f;
                std::uint32_t tid = 0;
                for (const sim::World::ShotView& v : world_.shots_in_flight())
                    if (v.attacker == u.id) {
                        hx = float(v.x.to_double());
                        hy = float(v.y.to_double());
                        tid = v.target;
                        live = true;
                        break;
                    }
                static const bool no_return = std::getenv("OMC_NO_TENTACLE_RETURN") != nullptr;
                const float gt = (float(world_.tick()) + alpha) / float(sim::World::kTicksPerSecond);
                if (live) {
                    ba.reach_live = true;
                    ba.reach_last_x = hx;
                    ba.reach_last_y = hy;
                    ba.reach_target_id = tid;
                    ba.reach_has_target = true;
                    ba.retract_t0 = -1.0f;
                } else if (ba.reach_live) {
                    ba.reach_live = false;
                    static const bool kTurnAtHead = std::getenv("OMC_REACH_TURN_AT_HEAD") != nullptr;
                    if (!kTurnAtHead && ba.reach_has_target && ba.reach_target_id < world_.units().size()) {
                        const sim::Unit& tu = world_.unit(ba.reach_target_id);
                        ba.reach_last_x = float(tu.x.to_double());
                        ba.reach_last_y = float(tu.y.to_double());
                    }
                    ba.reach_has_target = false;
                    if (!no_return) {
                        ba.retract_t0 = gt;
                        ba.retract_from_x = ba.reach_last_x;
                        ba.retract_from_y = ba.reach_last_y;
                        ba.retract_dist = std::hypot(ba.reach_last_x - wx, ba.reach_last_y - wy);
                        int pick = ba.ret_n - 1;
                        for (int k = 0; k < ba.ret_n; ++k)
                            if (ba.ret_lte[k] <= 0.0f || ba.retract_dist <= ba.ret_lte[k]) { pick = k; break; }
                        ba.retract_v0 = pick >= 0 ? ba.ret_v0[pick] : 0.0f;
                        ba.retract_acc = pick >= 0 ? ba.ret_acc[pick] : 0.0f;
                        ba.retract_vmax = pick >= 0 ? ba.ret_vmax[pick] : 0.0f;
                        if (ba.retract_v0 <= 0.0f && ba.retract_acc <= 0.0f && ba.retract_vmax <= 0.0f) ba.retract_t0 = -1.0f;
                    }
                }
                if (!live && ba.retract_t0 >= 0.0f) {
                    const float t = gt - ba.retract_t0 - ba.retract_delay;
                    float d = 0.0f;
                    if (t > 0.0f) {
                        const float v0 = ba.retract_v0, a = ba.retract_acc, vm = ba.retract_vmax > 0.0f ? ba.retract_vmax : 1e9f;
                        if (a > 0.0f && v0 < vm) {
                            const float t1 = (vm - v0) / a;
                            d = t < t1 ? v0 * t + 0.5f * a * t * t : v0 * t1 + 0.5f * a * t1 * t1 + vm * (t - t1);
                        } else {
                            d = std::min(v0 > 0.0f ? v0 : vm, vm) * t;
                        }
                    }
                    if (std::getenv("OMC_REACH_TRACE"))
                        std::fprintf(stderr, "[reach] tick %.2f t %.3f d %.2f of %.2f (v0 %.2f a %.2f vmax %.2f delay %.3f)\n", double(gt * float(sim::World::kTicksPerSecond)),
                                     double(t), double(d), double(ba.retract_dist), double(ba.retract_v0), double(ba.retract_acc), double(ba.retract_vmax),
                                     double(ba.retract_delay));
                    if (d >= ba.retract_dist) {
                        ba.retract_t0 = -1.0f;
                    } else {
                        const float f = ba.retract_dist > 1e-4f ? d / ba.retract_dist : 1.0f;
                        hx = ba.retract_from_x + (wx - ba.retract_from_x) * f;
                        hy = ba.retract_from_y + (wy - ba.retract_from_y) * f;
                        live = true;
                    }
                }
                if (live) {
                    inst.reach_tip_bone = ba.reach_tip;
                    inst.reach_root_bone = ba.reach_root;
                    inst.reach_weapon_bone = ba.reach_weapon;
                    inst.reach_target = {hx, hy, ground_z_at(hx, hy) + 0.3f};
                }
            }
        }
        inst.team = team_colour_for(cat_, u.player, selected);
        inst.team_emissive = team_emissive_for(cat_, u.player);
        inst.opacity = body_opacity;
        if (cat_ && u.player >= 0 && !u.catalog_id.empty()) {
            const bool unpowered = sim::power_disabled(u);
            const bool depleted = u.is_gas && u.resource_left <= 0 && u.speed.raw() == 0;
            if (unpowered || depleted) {
                const std::string key = u.catalog_id + (unpowered ? "#unpowered" : "#depleted");
                auto tit = tint_cache_.find(key);
                if (tit == tint_cache_.end()) {
                    static const bool kTintAny = std::getenv("OMC_UNPOWERED_TINT_ANY_ROW") != nullptr;
                    const auto t = unpowered ? cat_->actor_tint(u.catalog_id, "BehaviorLevel.PowerUser", kTintAny ? "Level 0" : ".Start; Level 0")
                                             : cat_->actor_tint(u.catalog_id, "Behavior.Harvestable", ".Deplete");
                    tit = tint_cache_.emplace(key, t).first;
                }
                if (tit->second) inst.tint = *tit->second;
            }
            if (u.id < driver_.actors.size() && driver_.actors[u.id].bus_tint_set)
                for (int k = 0; k < 3; ++k) inst.tint[std::size_t(k)] = driver_.actors[u.id].bus_tint[k];
            if (!halluc_look_resolved_) {
                halluc_look_resolved_ = true;
                halluc_look_ = cat_->hallucination_look();
                if (halluc_look_) halluc_tag_ = behavior_id_hash(halluc_look_->behavior);
            }
            if (halluc_look_ && !u.behaviors.empty()) {
                bool illusion = false;
                for (const sim::BehaviorInstance& b : u.behaviors)
                    if (b.src == halluc_tag_) { illusion = true; break; }
                if (illusion) {
                    const bool own_side = cfg_.human_player < 0 || u.player == cfg_.human_player ||
                                          world_.are_allied(u.player, cfg_.human_player);
                    if (own_side || world_.revealed_to(cfg_.human_player, u.id)) {
                        inst.tint = halluc_look_->tint;
                        if (halluc_look_->opacity < inst.opacity) inst.opacity = halluc_look_->opacity;
                    }
                }
            }
        }
        std::vector<render::SceneInstance> burn_flames;
        if (models_ && ui_store_ && m && u.player >= 0 && u.speed.raw() == 0 && !u.catalog_id.empty() && u.hp_max.raw() > 0) {
            const auto& bstates = models_->burn_states(*ui_store_, u.catalog_id);
            int want = -1;
            if (!bstates.empty() && !u.constructing) {
                const double life = u.hp.to_double() / u.hp_max.to_double();
                const double sh = u.shields_max.raw() > 0 ? u.shields.to_double() / u.shields_max.to_double() : 1.0;
                for (std::size_t bi = 0; bi < bstates.size(); ++bi) {
                    const std::string& sname = bstates[bi].state;
                    const bool on = sname == "LightSmoke"   ? (life < 0.666 && life >= 0.5)
                                    : sname == "HeavySmoke" ? (life < 0.5 && life >= 0.333)
                                    : sname == "Fire"       ? (life < 0.333)
                                    : sname == "ProtossFire" ? (sh < 0.25 && life < 0.75)
                                                             : false;
                    if (on) want = int(bi);
                }
            }
            auto bit = burning_.find(u.id);
            if (want < 0) {
                if (bit != burning_.end()) burning_.erase(bit);
            } else {
                if (bit == burning_.end() || bit->second.state != want) {
                    BurnFx bf;
                    bf.state = want;
                    bf.start_ms = clock_ms_;
                    std::vector<int> pts;
                    for (std::size_t ai = 0; ai < m->attachments.size(); ++ai)
                        if (m->attachments[ai].name.find("Damage") != std::string::npos) pts.push_back(int(ai));
                    std::uint64_t h = 0x9E3779B97F4A7C15ull * (u.id + 1) + std::uint64_t(want);
                    for (std::size_t k = pts.size(); k > 1; --k) {
                        h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
                        std::swap(pts[k - 1], pts[std::size_t(h % k)]);
                    }
                    const std::size_t keep = std::min(pts.size(), std::max<std::size_t>(2, std::size_t(double(pts.size()) * 0.25 + 0.5)));
                    pts.resize(keep);
                    bf.points = std::move(pts);
                    bit = burning_.insert_or_assign(u.id, std::move(bf)).first;
                }
                const formats::Model* fm = bstates[std::size_t(want)].model;
                const int bseq = render::select_sequence(*fm, {"Birth"}, u.id);
                const int sseq = render::select_sequence(*fm, {"Stand"}, u.id);
                const float blen = (bseq >= 0 && std::size_t(bseq) < fm->sequences.size()) ? float(fm->sequences[std::size_t(bseq)].length_ms()) : 0.0f;
                const float slen = (sseq >= 0 && std::size_t(sseq) < fm->sequences.size()) ? float(fm->sequences[std::size_t(sseq)].length_ms()) : 0.0f;
                const float t = float(clock_ms_ - bit->second.start_ms);
                for (int ai : bit->second.points) {
                    render::Mat4 att;
                    if (inst.seq < 0 || !render::attachment_transform(*m, m->attachments[std::size_t(ai)].name,
                                                                     std::uint32_t(inst.seq), inst.time_ms, att))
                        continue;
                    render::SceneInstance fl;
                    fl.model = fm;
                    const render::Mat4 hw = inst.world * att;
                    fl.world = render::translation(hw.m[12], hw.m[13], hw.m[14]);
                    if (t < blen && bseq >= 0) { fl.seq = bseq; fl.time_ms = t; }
                    else { fl.seq = sseq >= 0 ? sseq : bseq; fl.time_ms = slen > 0.0f ? std::fmod(t - blen, slen) : 0.0f; }
                    fl.team = {0, 0, 0, -1.0f};
                    burn_flames.push_back(fl);
                }
            }
        }
        inst.trail_id = u.id + 1;
        inst.trail_time_s = float(clock_ms_ * 0.001);
        if (cloak_fx) {
            inst.fx.kind = render::FxKind::Cloak;
            inst.fx.t = float(clock_ms_ * 0.001);
            static const bool kLookOld = std::getenv("OMC_CLOAK_LOOK_OLD") != nullptr;
            if (!kLookOld && cloak_look_.valid() && cloak_state.seq >= 0) {
                inst.fx.p0[0] = cloak_state.look.opacity;
                inst.fx.p0[1] = cloak_state.look.strength * kCloakUvPerStrength;
            } else {
                inst.fx.p0[0] = cloak_enemy && cloak_enemy_look_ == 1 ? 0.0f : kCloakOpacity;
                inst.fx.p0[1] = kCloakDisplaceStrength * kCloakUvPerStrength;
            }
        } else if (u.constructing && u.warp_forming && !draws_authored_warp_in(u.catalog_id, inst.model)) {
            double p = u.build_progress.to_double();
            if (!(p >= 0.0)) p = 0.0;
            if (p > 1.0) p = 1.0;
            inst.fx.kind = render::FxKind::Dissolve;
            inst.fx.p0[0] = float(p);
            inst.fx.p0[1] = inst.team[0];
            inst.fx.p0[2] = inst.team[1];
            inst.fx.p0[3] = inst.team[2];
        }
        const bool targeting = pending_attack_ || has_pending_ability_cast();
        if (targeting && u.id == hover_unit_) {
            const float leg = 0.15f;
            float ph = std::fmod(float(clock_ms_ * 0.001), 2.0f * leg) / leg;
            if (ph > 1.0f) ph = 2.0f - ph;
            const float a[3] = {1.0f, 192.0f / 255.0f, 192.0f / 255.0f}, b[3] = {1.0f, 128.0f / 255.0f, 128.0f / 255.0f};
            for (int k = 0; k < 3; ++k) inst.tint[std::size_t(k)] *= a[k] + (b[k] - a[k]) * ph;
        }
        static const bool kUpgradeFlashOld = std::getenv("OMC_UPGRADE_FLASH_OLD") != nullptr;
        if (kUpgradeFlashOld && u.player >= 0 && std::size_t(u.player) < upg_flash_s_.size() && upg_flash_s_[std::size_t(u.player)] > 0.0f &&
            u.speed.raw() != 0) {
            const float f = upg_flash_s_[std::size_t(u.player)] / 0.5f;
            const float hdr = 1.0f + 9.0f * f;
            for (int k = 0; k < 3; ++k) inst.tint[std::size_t(k)] *= hdr;
        }
        if (morph_transition_models_ && world_.is_morphing(u.id) && !world_.is_morph_delayed(u.id))
            if (const MorphShow* show = morph_show(u)) {
                const double elapsed = (u.morph_total - u.morph_timer).to_double();
                if (elapsed >= show->delay_s) {
                    inst.model = show->model;
                    inst.seq = show->seq;
                    inst.time_ms = float(std::min(double(show->len_ms), (elapsed - show->delay_s) * 1000.0));
                    const double run_ms = (elapsed - show->delay_s) * 1000.0;
                    if (show->stand_seq >= 0 && show->stand_ms > 0.0f && run_ms > double(show->len_ms)) {
                        inst.seq = show->stand_seq;
                        inst.time_ms = float(std::fmod(run_ms - double(show->len_ms), double(show->stand_ms)));
                    }
                    inst.blend_seq = -1;
                    inst.blend_weight = 0.0f;
                    inst.turret_bone = -1;
                    inst.global_layers.clear();
                }
            }
        if (inst.model && !cloak_enemy) {
            if (pick_pose_.size() <= u.id) pick_pose_.resize(std::size_t(u.id) + 1);
            pick_pose_[u.id] = PickPose{inst.model, inst.world, inst.seq, inst.time_ms, pick_frame_};
        }
        scene.push_back(inst);
        if (snapshot_capture) {
            FogSnapshot s;
            s.inst = inst;
            s.inst.team = team_colour_for(cat_, u.player, false);
            s.inst.tint = render::SceneInstance{}.tint;
            s.inst.blend_seq = -1;
            s.inst.blend_weight = 0.0f;
            s.inst.trail_id = 0;
            s.catalog_id = u.catalog_id;
            s.player = u.player;
            s.x = float(u.x.to_double());
            s.y = float(u.y.to_double());
            s.minimap_radius = float(u.minimap_radius.to_double());
            s.constructing = u.constructing;
            s.build_progress = float(u.build_progress.to_double());
            if (const auto sp = fog_splat_frame_.find(u.id); sp != fog_splat_frame_.end()) {
                s.splat = sp->second;
                s.has_splat = true;
            }
            fog_snaps_[u.id] = std::move(s);
        }
        if (cloak_enemy) continue;
        if (zone_markers_ && world_.has_zone(u.id) && models_ && ui_store_) {
            const auto zm = zone_marker_.find(u.id);
            const std::string actor = zm != zone_marker_.end() ? zm->second.first : zone_marker_of(u.catalog_id);
            float zsc[3] = {1.0f, 1.0f, 1.0f};
            if (const formats::Model* mm = actor.empty() ? nullptr : models_->scaffold_model(*ui_store_, actor, zsc)) {
                render::SceneInstance zi;
                zi.model = mm;
                const float zx = float(u.zone_x.to_double()), zy = float(u.zone_y.to_double());
                zi.world = render::translation(zx, zy, ground_z_at(zx, zy)) * render::scaling(zsc[0], zsc[1], zsc[2]);
                zi.team = inst.team;
                const double run_ms = zm != zone_marker_.end() ? double(world_.tick() - zm->second.second) * 1000.0 /
                                                                     double(sim::World::kTicksPerSecond)
                                                               : 1.0e6;
                const int birth = render::select_sequence(*mm, {"Birth"}, 0);
                const int stand = render::select_sequence(*mm, {"Stand"}, 0);
                const float blen = birth >= 0 ? float(mm->sequences[std::size_t(birth)].length_ms()) : 0.0f;
                if (birth >= 0 && run_ms < double(blen)) {
                    zi.seq = birth;
                    zi.time_ms = float(run_ms);
                } else if (stand >= 0) {
                    zi.seq = stand;
                    const float sl = float(mm->sequences[std::size_t(stand)].length_ms());
                    zi.time_ms = sl > 0.0f ? float(std::fmod(run_ms - double(blen), double(sl))) : 0.0f;
                } else {
                    zi.seq = -1;
                }
                scene.push_back(zi);
            }
        }
        scene.insert(scene.end(), burn_flames.begin(), burn_flames.end());
        if (u.id < driver_.actors.size() && models_ && ui_store_ && cat_) {
            render::ActorInstance& host = driver_.actors[u.id];
            for (std::size_t ci = 0; ci < host.children.size();) {
                render::ActorChild& ch = host.children[ci];
                if (!ch.rig) {
                    float sc3[3] = {1.0f, 1.0f, 1.0f};
                    const formats::Model* sm = models_->scaffold_model(*ui_store_, ch.name, sc3);
                    static const bool kChildTrace = std::getenv("OMC_CHILD_TRACE") != nullptr;
                    if (kChildTrace) {
                        static std::set<std::string> told;
                        if (told.insert(ch.name).second) {
                            std::fprintf(stderr, "[child] %s of %s: model %s (verts %u particles %zu seqs %zu)\n", ch.name.c_str(), u.catalog_id.c_str(), sm ? "yes" : "NO",
                                         sm ? sm->vertex_count : 0u, sm ? sm->particles.size() : std::size_t(0), sm ? sm->sequences.size() : std::size_t(0));
                            for (const render::ActorEventRule& r : ch.rules) {
                                std::fprintf(stderr, "[child]   %s.%s -> %s", r.head.c_str(), r.sub.c_str(), r.verb.c_str());
                                for (const std::string& a : r.args) std::fprintf(stderr, " %s", a.c_str());
                                std::fprintf(stderr, "\n");
                            }
                            if (sm)
                                for (const auto& sq : sm->sequences) std::fprintf(stderr, "[child]   seq '%s' %u ms flags %u\n", sq.name.c_str(), unsigned(sq.length_ms()), unsigned(sq.flags));
                        }
                    }
                    if (!sm) { ++ci; continue; }
                    ch.rig = std::make_shared<render::ActorInstance>();
                    ch.rig->model = sm;
                    ch.rig->seed = u.id + 7;
                    ch.rig->has_oneshot = false;
                    for (int k = 0; k < 3; ++k) ch.scale[k] = sc3[k];
                    if (auto f = cat_->actor_host_forward(ch.name)) ch.yaw = std::atan2(f->second, f->first);
                    static const bool kChildQuarter = std::getenv("OMC_CHILD_YAW_NO_QUARTER") == nullptr;
                    if (kChildQuarter && cat_->actor_host_forward(ch.name)) ch.yaw += 1.5707963f;
                    static const bool kChildHostScale = std::getenv("OMC_CHILD_HOST_SCALE") != nullptr;
                    if (kChildHostScale && sc3[0] == 1.0f && sc3[1] == 1.0f && sc3[2] == 1.0f) {
                        const std::array<float, 3> hs = data::resolve_unit_model_scale(*cat_, data::unit_actor_id(*cat_, u.catalog_id));
                        for (int k = 0; k < 3; ++k) ch.scale[k] = hs[std::size_t(k)];
                    }
                    for (const render::ActorEventRule& r : ch.rules)
                        if (r.head == "ActorCreation" && r.verb == "Create" && !r.args.empty()) {
                            auto ladder = cat_->actor_progress_stages(r.args[0]);
                            if (!ladder.empty()) ch.ladder = std::move(ladder);
                        }
                    static const bool kEffectChild = std::getenv("OMC_NO_EFFECT_CHILD") == nullptr;
                    if (kEffectChild && ch.ladder.empty() && !u.constructing)
                        for (const render::ActorEventRule& r : ch.rules)
                            if (r.head == "ActorCreation" && r.verb == "AnimPlay" && r.args.size() >= 2) {
                                std::vector<std::string> props;
                                for (std::size_t k = 1; k < r.args.size(); ++k) {
                                    std::size_t s0 = 0;
                                    const std::string& a = r.args[k];
                                    while (s0 <= a.size()) {
                                        const std::size_t e = a.find(',', s0);
                                        const std::string tok = a.substr(s0, e == std::string::npos ? std::string::npos : e - s0);
                                        if (!tok.empty()) props.push_back(tok);
                                        if (e == std::string::npos) break;
                                        s0 = e + 1;
                                    }
                                }
                                const bool forever = std::find(props.begin(), props.end(), "PlayForever") != props.end();
                                if (forever) props.erase(std::remove(props.begin(), props.end(), "PlayForever"), props.end());
                                ch.rig->anim_play(r.args[0], props, forever, 2);
                                const int eseq = ch.rig->current_sequence();
                                ch.finish_len_ms = forever ? 1e30f : (eseq >= 0 && std::size_t(eseq) < sm->sequences.size()) ? float(sm->sequences[std::size_t(eseq)].length_ms()) : 0.0f;
                                ch.finishing = true;
                                break;
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
                        if (rig.model) stage = timed_ladder_stage(*rig.model, ch.ladder, p, u.build_time.to_double() * 1000.0, stage);
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
                static const float kChildYawAdd = std::getenv("OMC_CHILD_YAW_ADD") ? float(std::atof(std::getenv("OMC_CHILD_YAW_ADD"))) * 0.01745329252f : 0.0f;
                static const float kChildScale = std::getenv("OMC_CHILD_SCALE") ? float(std::atof(std::getenv("OMC_CHILD_SCALE"))) : 1.0f;
                c.world = render::translation(wx, wy, wz) * render::rotation_z(ch.yaw + kChildYawAdd) *
                          render::scaling(ch.scale[0] * kChildScale, ch.scale[1] * kChildScale, ch.scale[2] * kChildScale);
                c.seq = rig.current_sequence();
                c.time_ms = rig.current_time();
                c.team = inst.team;
                static const bool kChildEmissive = std::getenv("OMC_CHILD_NO_TEAM_EMISSIVE") == nullptr;
                if (kChildEmissive) c.team_emissive = inst.team_emissive;
                bool self_lit_team = false;
                if (!construction_child_team_ && c.model) {
                    std::unordered_map<const formats::Model*, bool>& self_lit_of = self_lit_cache_;
                    auto sl = self_lit_of.find(c.model);
                    if (sl == self_lit_of.end()) {
                        bool lit = false;
                        for (const formats::M3Material& mm : c.model->materials) {
                            if (mm.emis_op != 4) continue;
                            std::string dif, emi;
                            for (const formats::M3Layer& ly : mm.layers) {
                                if (ly.usage == "Diffuse") dif = ly.image_path;
                                else if (ly.usage == "Emissive") emi = ly.image_path;
                            }
                            if (!emi.empty() && emi == dif) lit = true;
                        }
                        sl = self_lit_of.emplace(c.model, lit).first;
                    }
                    self_lit_team = sl->second;
                }
                if (self_lit_team) {
                    c.team = {0.0f, 0.0f, 0.0f, -1.0f};
                    c.team_emissive = {0.0f, 0.0f, 0.0f, -1.0f};
                }
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
                float cf = fl > 1e-3f ? fx / fl : 1.0f;
                float sf = fl > 1e-3f ? fy / fl : 0.0f;
                static const bool kOffsetQuarter = std::getenv("OMC_CARRY_OFFSET_NO_QUARTER") == nullptr;
                if (kOffsetQuarter) { const float t = cf; cf = -sf; sf = t; }
                ox = lx * cf - ly * sf;
                oy = lx * sf + ly * cf;
            } else {
                const float off = visual_r + 0.15f;
                ox = fl > 1e-3f ? fx / fl * off : off;
                oy = fl > 1e-3f ? fy / fl * off : 0.0f;
            }
            float cs = ms[0];
            static const char* const kCarryScale = std::getenv("OMC_CARRY_SCALE");
            if (kCarryScale) cs = float(std::atof(kCarryScale));
            const formats::Model* cm = nullptr;
            if (models_ && ui_store_ && u.carry_source != sim::kNoTarget &&
                u.carry_source < world_.units().size()) {
                const std::string& src = world_.units()[u.carry_source].catalog_id;
                if (!src.empty()) cm = models_->carry_model(*ui_store_, src);
            }
            if (!cm) continue;
            static const bool kCarryTrace = std::getenv("OMC_CARRY_TRACE") != nullptr;
            if (kCarryTrace) {
                static int carry_traced = 0;
                if (carry_traced < 24) {
                    ++carry_traced;
                    std::fprintf(stderr, "[carry] unit %u %s at (%.2f, %.2f) facing (%.2f, %.2f) scale %.3f off %.3f %.3f %.3f verts %u bounds z %.3f..%.3f x %.3f..%.3f\n",
                                 unsigned(u.id), u.catalog_id.c_str(), double(wx), double(wy), double(fx), double(fy), double(cs), double(ox), double(oy), double(oz), unsigned(cm->vertex_count),
                                 double(cm->bounds_min[2]), double(cm->bounds_max[2]), double(cm->bounds_min[0]), double(cm->bounds_max[0]));
                    std::fprintf(stderr, "[carry]   hardpoint axes |x| %.3f |y| %.3f |z| %.3f  x=(%.2f,%.2f,%.2f) z=(%.2f,%.2f,%.2f)  body scale %.3f %.3f %.3f\n",
                                 double(std::sqrt(att.m[0] * att.m[0] + att.m[1] * att.m[1] + att.m[2] * att.m[2])),
                                 double(std::sqrt(att.m[4] * att.m[4] + att.m[5] * att.m[5] + att.m[6] * att.m[6])),
                                 double(std::sqrt(att.m[8] * att.m[8] + att.m[9] * att.m[9] + att.m[10] * att.m[10])), double(att.m[0]), double(att.m[1]), double(att.m[2]),
                                 double(att.m[8]), double(att.m[9]), double(att.m[10]), double(ms[0]), double(ms[1]), double(ms[2]));
                }
            }
            render::SceneInstance chunk;
            chunk.model = cm;
            chunk.world = render::translation(wx + ox, wy + oy, wz + oz) *
                          render::scaling(cs, cs, cs);
            static const bool kCarryTurn = std::getenv("OMC_CARRY_UNTURNED") == nullptr;
            if (kCarryTurn && fl > 1e-3f) {
                render::Mat4 hp = att;
                const bool has_hp = std::fabs(hp.m[0]) + std::fabs(hp.m[1]) + std::fabs(hp.m[2]) > 1e-3f;
                if (!has_hp) hp = render::scaling(1.0f, 1.0f, 1.0f);
                hp.m[12] = 0.0f; hp.m[13] = 0.0f; hp.m[14] = 0.0f;
                static const float kCarryQuarter = std::getenv("OMC_CARRY_YAW_NO_QUARTER") ? 0.0f : 1.5707963f;
                chunk.world = render::translation(wx + ox, wy + oy, wz + oz) * render::rotation_z(std::atan2(fy, fx) + kCarryQuarter) * hp *
                              render::scaling(cs, cs, cs);
            }
            chunk.team = {0, 0, 0, -1.0f};
            static const bool kCarrySeq = std::getenv("OMC_CARRY_SEQ") != nullptr;
            chunk.seq = (!cm->sequences.empty() && kCarrySeq) ? render::select_sequence(*cm, {"Stand"}, u.id) : -1;
            chunk.time_ms = float(clock_ms_);
            scene.push_back(chunk);
        }

        constexpr std::array<float, 3> kSelectedRingRgb = {0.0f, 168.0f / 255.0f, 90.0f / 255.0f};
        constexpr std::array<float, 3> kSubgroupRingRgb = {0.0f, 250.0f / 255.0f, 25.0f / 255.0f};
        constexpr std::array<float, 3> kEnemyRingRgb = {230.0f / 255.0f, 0.0f, 0.0f};
        constexpr std::array<float, 3> kAllyRingRgb = {1.0f, 1.0f, 0.0f};
        std::array<float, 3> sel_rgb = kSelectedRingRgb, sub_rgb = kSubgroupRingRgb;
        std::array<float, 3> ally_rgb = kAllyRingRgb, foe_rgb = kEnemyRingRgb;
        if (cat_) {
            auto to_f = [](const std::array<std::uint8_t, 3>& c) {
                return std::array<float, 3>{float(c[0]) / 255.0f, float(c[1]) / 255.0f,
                                            float(c[2]) / 255.0f};
            };
            sel_rgb = to_f(sel_look.selected);
            sub_rgb = to_f(sel_look.subgroup);
            ally_rgb = to_f(sel_look.ally);
            foe_rgb = to_f(sel_look.enemy);
        }
        const std::array<float, 3>& own = in_active_subgroup ? sub_rgb : sel_rgb;
        const std::array<float, 3>& foreign =
            world_.are_allied(u.player, cfg_.human_player) ? ally_rgb : foe_rgb;
        const std::array<float, 3>& rel = u.player == cfg_.human_player ? own : foreign;
        std::array<float, 4> ring_rc = u.player >= 0
                                      ? std::array<float, 4>{rel[0], rel[1], rel[2], 1.0f}
                                      : std::array<float, 4>{0.9f, 0.85f, 0.30f, 1.0f};
        ring_rc[3] = 2.0f;
        if (selected) {
            const float kOuterWidth = sel_look.width;
            const float kInnerBoundary = sel_look.inner_boundary;
            const float kInnerFallOff = sel_look.inner_falloff;
            const float ring_r = selection_ring_radius(cat_, u.catalog_id, float(u.radius.to_double()));
            const float rr = ring_r > 1e-3f ? ring_r : visual_r;
            const float kLineWorld = env_sel_line_003 ? kOuterWidth : 0.05f;
            const float line_frac = env_sel_width_ratio ? kOuterWidth : std::min(0.5f, kLineWorld / std::max(rr, 1e-3f));
            const float inner = 1.0f - line_frac;
            const int key = int(inner * 2000.0f + 0.5f);
            static std::map<int, formats::Model> ring_cache;
            auto rit = ring_cache.find(key);
            if (rit == ring_cache.end()) rit = ring_cache.emplace(key, render::make_ring_model(inner)).first;
            const formats::Model* rm = &rit->second;
            std::array<float, 4> rc = ring_rc;
            if (env_sel_rim_uniform || env_sel_band_flat || env_sel_band_radial) {
                render::SceneInstance ring;
                ring.model = rm;
                ring.world = render::translation(wx, wy, wz + 0.05f) * render::scaling(rr, rr, 1.0f);
                ring.team = rc;
                ring.seq = -1;
                scene.push_back(ring);
            } else {
                const int kRimLayers = 6;
                const float d = 1.0f - kInnerBoundary, rad = kInnerBoundary;
                const float layer_alpha = 1.0f - std::pow(1.0f - 0.50f, 1.0f / float(kRimLayers));
                for (int li = 0; li < kRimLayers; ++li) {
                    const float want = 2.0f * d * (float(li) + 0.5f) / float(kRimLayers);
                    float lo_a = 0.0f, hi_a = 3.14159265f;
                    for (int it = 0; it < 24; ++it) {
                        const float mid = 0.5f * (lo_a + hi_a);
                        const float th = 1.0f - (-d * std::cos(mid) + std::sqrt(rad * rad - d * d * std::sin(mid) * std::sin(mid)));
                        (th > want ? lo_a : hi_a) = mid;
                    }
                    const float half = lo_a;
                    const int akey = 300000 + key * 8 + li;
                    auto ait = ring_cache.find(akey);
                    if (ait == ring_cache.end())
                        ait = ring_cache.emplace(akey, render::make_arc_ring_model(inner, 1, half / 3.14159265f, 64)).first;
                    render::SceneInstance arc;
                    arc.model = &ait->second;
                    arc.world = render::translation(wx, wy, wz + 0.05f) * render::rotation_z(-1.57079633f - half) * render::scaling(rr, rr, 1.0f);
                    arc.team = rc;
                    arc.seq = -1;
                    arc.opacity = layer_alpha;
                    scene.push_back(arc);
                }
            }
            if (env_sel_band_flat) {
                const float band_outer = rr * (1.0f - kOuterWidth);
                const float band_inner = kInnerBoundary / (1.0f - kOuterWidth);
                const int bkey = int(band_inner * 200.0f + 0.5f);
                auto bit = ring_cache.find(bkey);
                if (bit == ring_cache.end()) bit = ring_cache.emplace(bkey, render::make_ring_model(band_inner)).first;
                render::SceneInstance band;
                band.model = &bit->second;
                band.world = render::translation(wx, wy, wz + 0.05f) * render::scaling(band_outer, band_outer, 1.0f);
                band.team = rc;
                band.seq = -1;
                band.opacity = kInnerFallOff;
                scene.push_back(band);
            } else if (!env_sel_band_radial) {
                const int kLayers = 8;
                const float max_off = 1.0f - kInnerBoundary;
                const float peak = 0.30f;
                for (int st = 0; st < kLayers; ++st) {
                    const float shrink = 2.0f * max_off * float(st) / float(kLayers);
                    const int ckey = 200000 + int(max_off * 1000.0f + 0.5f) * 100 + st;
                    auto bit = ring_cache.find(ckey);
                    if (bit == ring_cache.end()) bit = ring_cache.emplace(ckey, render::make_crescent_model(max_off, shrink)).first;
                    render::SceneInstance band;
                    band.model = &bit->second;
                    band.world = render::translation(wx, wy, wz + 0.05f) * render::scaling(rr * inner, rr * inner, 1.0f);
                    band.team = rc;
                    band.seq = -1;
                    band.opacity = peak / float(kLayers);
                    scene.push_back(band);
                }
            } else {
                const int kSteps = 4;
                const float lo = kInnerBoundary, hi = std::max(kInnerBoundary + 0.01f, inner);
                for (int st = 0; st < kSteps; ++st) {
                    const float r0 = lo + (hi - lo) * float(st) / float(kSteps), r1 = lo + (hi - lo) * float(st + 1) / float(kSteps);
                    const float hole = r0 / r1;
                    const int bkey = 100000 + int(hole * 2000.0f + 0.5f);
                    auto bit = ring_cache.find(bkey);
                    if (bit == ring_cache.end()) bit = ring_cache.emplace(bkey, render::make_ring_model(hole)).first;
                    render::SceneInstance band;
                    band.model = &bit->second;
                    band.world = render::translation(wx, wy, wz + 0.05f) * render::scaling(rr * r1, rr * r1, 1.0f);
                    band.team = rc;
                    band.seq = -1;
                    band.opacity = 0.16f * (float(st) + 0.5f) / float(kSteps);
                    scene.push_back(band);
                }
            }
        }
        if (!selected && u.id == hover_unit_ && u.player >= 0) {
            const float kPreOuterWidth = sel_look.width;
            constexpr float kPreInnerWidth = 0.09f;
            const int kPreSegments = sel_look.pre_segments;
            const float kPreSolid = sel_look.pre_solid;
            const float kPreAlpha = sel_look.pre_alpha;
            const float kPreRotPerSec = sel_look.pre_rotation;
            const float hole = 1.0f - (kPreOuterWidth + kPreInnerWidth);
            const int pkey = int(hole * 1000.0f) * 10000 + kPreSegments * 100 + int(kPreSolid * 99.0f);
            static std::map<int, formats::Model> pre_cache;
            auto pit = pre_cache.find(pkey);
            if (pit == pre_cache.end())
                pit = pre_cache.emplace(pkey, render::make_arc_ring_model(hole, kPreSegments, kPreSolid)).first;
            const formats::Model& pre_ring = pit->second;
            const float ring_r = selection_ring_radius(cat_, u.catalog_id, float(u.radius.to_double()));
            const float rr = ring_r > 1e-3f ? ring_r : visual_r;
            const float spin = float(clock_ms_ * 0.001) * kPreRotPerSec * 6.2831853071795864f;
            render::SceneInstance hov;
            hov.model = &pre_ring;
            hov.world = render::translation(wx, wy, wz + 0.05f) * render::rotation_z(spin) *
                        render::scaling(rr, rr, 1.0f);
            hov.team = ring_rc;
            hov.seq = -1;
            hov.opacity = kPreAlpha;
            scene.push_back(hov);
        }
        if (selected && cat_ && u.player == cfg_.human_player && !u.catalog_id.empty()) {
            std::vector<double> radii;
            for (const data::Catalog::RangeRing& rr : cat_->unit_range_rings(u.catalog_id))
                if (rr.on_selection) radii.push_back(rr.radius);
            if (!pending_cast_abil_.empty())
                if (const auto r = cat_->ability_range_ring(pending_cast_abil_)) radii.push_back(*r);
            for (double rad : radii) emit_range_ring(scene, wx, wy, wz, float(rad) + float(u.radius.to_double()));
        }
        for (const sim::BehaviorInstance& bi : u.behaviors) {
            if (!bi.lift || bi.caster == sim::kNoTarget) continue;
            if (bi.caster >= world_.units().size()) continue;
            const sim::Unit& src = world_.units()[bi.caster];
            if (!src.alive) continue;
            float pa[3], pb[3];
            unit_attach_world(src, nullptr, nullptr, pa);
            unit_attach_world(u, nullptr, nullptr, pb);
            emit_beam(scene, pa, pb, 0.25f);
        }
    }
    const std::size_t fx_from = tag_pass(units_from, render::PassGroup::Units);

    for (const sim::HealerInstance& h : world_.healers()) {
        if (h.caster >= world_.units().size() || h.target >= world_.units().size()) continue;
        const sim::Unit& src = world_.units()[h.caster];
        const sim::Unit& dst = world_.units()[h.target];
        if (!src.alive || !dst.alive) continue;
        static const bool kHealBeamOld = std::getenv("OMC_HEAL_BEAM_OLD") != nullptr;
        if (!kHealBeamOld && std::any_of(beams_.begin(), beams_.end(), [&](const BeamFx& b) {
                return b.heal && b.model && b.attacker == h.caster && b.target == h.target && b.phase < 2;
            }))
            continue;
        float pa[3], pb[3];
        unit_attach_world(src, nullptr, nullptr, pa);
        unit_attach_world(dst, nullptr, nullptr, pb);
        emit_beam(scene, pa, pb, 0.18f);
    }

    if (models_ && ui_store_ && cat_ && cfg_.human_player >= 0)
        for (uint32_t sid : selection_) {
            if (sid >= world_.units().size()) continue;
            const Unit& su = world_.units()[sid];
            if (!su.alive || su.player != cfg_.human_player || !su.has_rally || su.catalog_id.empty()) continue;
            const int race = cat_->unit_race_index(su.catalog_id);
            const char* rid = race == 1 ? "CommandUIHarnessRallyProtoss" : race == 2 ? "CommandUIHarnessRallyZerg" : "CommandUIHarnessRallyTerran";
            const formats::Model* rm = models_->cmodel(*ui_store_, rid);
            if (!rm) continue;
            const float rx = float(su.rally_x.to_double()), ry = float(su.rally_y.to_double());
            render::SceneInstance ri;
            ri.model = rm;
            ri.world = render::translation(rx, ry, ground_z_at(rx, ry));
            ri.seq = render::select_sequence(*rm, {"Stand"}, sid);
            if (ri.seq >= 0 && std::size_t(ri.seq) < rm->sequences.size()) {
                const float len = float(rm->sequences[std::size_t(ri.seq)].length_ms());
                ri.time_ms = len > 0.0f ? float(std::fmod(clock_ms_, double(len))) : 0.0f;
            }
            ri.team = team_colour_for(cat_, su.player, false);
            ri.team_emissive = team_emissive_for(cat_, su.player);
            scene.push_back(ri);
        }

    if ((pending_build_ || pending_refinery_) && cat_ && !build_proto_.catalog_id.empty()) {
        const float gx = float(build_px_.to_double()), gy = float(build_py_.to_double());
        for (const data::Catalog::RangeRing& rr : cat_->unit_range_rings(build_proto_.catalog_id))
            if (rr.on_placement)
                emit_range_ring(scene, gx, gy, ground_z_at(gx, gy), float(rr.radius) + float(build_proto_.radius.to_double()));
    }
    if (const formats::Model* gm = placement_ghost_model()) {
        bool all = true;
        for (char c : ghost_.cell_ok) all = all && c != 0;
        if (ghost_.cell_ok.empty()) all = ghost_.ok;
        const float gx = ghost_.x, gy = ghost_.y;
        const std::array<float, 3> gs = data::resolve_unit_model_scale(*cat_, data::unit_actor_id(*cat_, ghost_.unit_id));
        const float face = cat_->unit_facing_radians(ghost_.unit_id);
        render::SceneInstance inst;
        inst.model = gm;
        inst.world = render::translation(gx, gy, ground_z_at(gx, gy)) * render::rotation_z(face + 1.5707963f) *
                     render::scaling(gs[0], gs[1], gs[2]);
        inst.seq = render::select_sequence(*gm, {"Stand"}, 0);
        inst.time_ms = 0.0f;
        inst.team = team_colour_for(cat_, cfg_.human_player, false);
        inst.team_emissive = team_emissive_for(cat_, cfg_.human_player);
        inst.opacity = 0.5f;
        inst.tint = all ? std::array<float, 3>{0.6f, 1.0f, 0.6f} : std::array<float, 3>{1.0f, 0.45f, 0.45f};
        scene.push_back(inst);
    }
    emit_order_cues(scene);

    for (const Corpse& c : corpses_) {
        if (!c.model) continue;
        if (fog_ready && c.player != cfg_.human_player &&
            !hud_fog_body_drawn(fog_body_state(c.player, int(vision_.state_at(sim::Fixed::from_double(c.x), sim::Fixed::from_double(c.y))), fog_hidden_black_), FogVisibility(c.fog_visibility)))
            continue;
        if (fog_hides_enemy(c.player, c.x, c.y, c.fog_visibility)) continue;
        render::SceneInstance inst;
        inst.model = c.model;
        inst.world = render::translation(c.x, c.y, c.z) * c.orient * render::scaling(c.scale[0], c.scale[1], c.scale[2]);
        inst.seq = c.seq;
        inst.time_ms = c.time_ms;
        inst.team = team_colour_for(cat_, c.player, false);
        inst.team_emissive = team_emissive_for(cat_, c.player);
        scene.push_back(inst);
    }
    for (const ImpactFx& f : impact_fx_) {
        if (!f.model) continue;
        if (fog_ready && f.player != cfg_.human_player &&
            vision_.state_at(sim::Fixed::from_double(f.x), sim::Fixed::from_double(f.y)) != sim::Visibility::Visible)
            continue;
        render::SceneInstance inst;
        inst.model = f.model;
        inst.world = render::translation(f.x, f.y, f.z) * render::rotation_z(f.yaw + 1.5707963f) *
                     render::scaling(f.scale, f.scale, f.scale);
        inst.seq = f.seq;
        inst.time_ms = f.time_ms;
        inst.team = {0, 0, 0, -1.0f};
        scene.push_back(inst);
    }

    for (const EffectDoodad& d : effect_doodads_) {
        if (!d.model) continue;
        if (fog_ready && d.player != cfg_.human_player &&
            vision_.state_at(sim::Fixed::from_double(d.x), sim::Fixed::from_double(d.y)) != sim::Visibility::Visible)
            continue;
        const int sq = d.seq[d.phase] >= 0 ? d.seq[d.phase] : (d.seq[1] >= 0 ? d.seq[1] : 0);
        render::SceneInstance inst;
        inst.model = d.model;
        inst.world = render::translation(d.x, d.y, d.z) * render::rotation_z(d.yaw + 1.5707963f);
        inst.seq = sq;
        inst.time_ms = d.time_ms;
        inst.team = {0, 0, 0, -1.0f};
        scene.push_back(inst);
    }
    for (const EffectActor& a : fx_host_.actors()) {
        if (!a.model || a.seq < 0) continue;
        if (fog_ready && a.player != cfg_.human_player &&
            vision_.state_at(sim::Fixed::from_double(a.x), sim::Fixed::from_double(a.y)) != sim::Visibility::Visible)
            continue;
        float p[3] = {a.x, a.y, ground_z_at(a.x, a.y)};
        const bool attached = !a.stationary && a.unit < world_.units().size() && world_.units()[a.unit].alive;
        if (attached) {
            const Unit& hu = world_.units()[a.unit];
            if (a.site_op.find("Center") != std::string::npos) unit_attach_world(hu, "Ref_Center", nullptr, p);
            else if (hu.flying) p[2] = air_z_at(p[0], p[1]);
        }
        render::SceneInstance inst;
        inst.model = a.model;
        inst.world = render::translation(p[0], p[1], p[2]) * render::rotation_z(1.5707963f + 1.5707963f) *
                     render::scaling(a.scale[0], a.scale[1], a.scale[2]);
        inst.seq = a.seq;
        inst.time_ms = a.time_ms;
        inst.team = team_colour_for(cat_, a.player, false);
        inst.team_emissive = team_emissive_for(cat_, a.player);
        scene.push_back(inst);
    }

    static const bool kAddonView = std::getenv("OMC_NO_ADDON_BUILD_VIEW") == nullptr;
    if (kAddonView && cat_ && models_ && ui_store_) {
        for (const Unit& host : world_.units()) {
            if (!host.alive || host.addon_building == 0) {
                if (host.id < addon_total_ms_.size()) addon_total_ms_[host.id] = 0.0f;
                continue;
            }
            if (fog_ready && host.player != cfg_.human_player && !audio_event_audible(host)) continue;
            const std::string key = host.catalog_id + "#" + std::to_string(host.addon_building);
            auto ait = addon_ids_.find(key);
            if (ait == addon_ids_.end()) {
                sim::Unit body;
                const bool ok = addon_body_factory(*cat_)(host, host.addon_building, body);
                ait = addon_ids_.emplace(key, ok ? body.catalog_id : std::string()).first;
            }
            if (ait->second.empty()) continue;
            const std::string& aid = ait->second;
            if (addon_total_ms_.size() <= host.id) addon_total_ms_.resize(host.id + 1, 0.0f);
            const float left_ms = float(host.addon_build_left.to_double() * 1000.0);
            if (left_ms > addon_total_ms_[host.id]) addon_total_ms_[host.id] = left_ms;
            const float total = addon_total_ms_[host.id];
            const double p = total > 0.0f ? std::clamp(1.0 - double(left_ms) / double(total), 0.0, 1.0) : 0.0;
            sim::Fixed ax, ay;
            sim::World::addon_slot(host.x, host.y, ax, ay);
            const float fx = float(ax.to_double()), fy = float(ay.to_double()), fz = ground_z_at(fx, fy);
            auto ladder_pose = [&](const formats::Model& m, const std::vector<std::vector<std::string>>& ladder, int& seq,
                                   float& t) {
                seq = -1;
                t = 0.0f;
                if (ladder.empty()) return;
                const double now = p * double(total);
                std::vector<double> len(ladder.size(), -1.0);
                double fixed = 0.0;
                int stands = 0;
                for (std::size_t si = 0; si < ladder.size(); ++si) {
                    const bool stand = std::find(ladder[si].begin(), ladder[si].end(), "Stand") != ladder[si].end();
                    const int sq = render::select_sequence(m, ladder[si], 0);
                    if (!stand && sq >= 0) {
                        len[si] = double(m.sequences[std::size_t(sq)].length_ms());
                        fixed += len[si];
                    } else {
                        ++stands;
                    }
                }
                const double each = stands > 0 && fixed < double(total) ? (double(total) - fixed) / double(stands)
                                                                       : double(total) / double(ladder.size());
                double at = 0.0;
                for (std::size_t si = 0; si < ladder.size(); ++si) {
                    const double span = len[si] >= 0.0 ? len[si] : each;
                    if (now < at + span || si + 1 == ladder.size()) {
                        seq = render::select_sequence(m, ladder[si], 0);
                        if (seq >= 0) {
                            const double clip = double(m.sequences[std::size_t(seq)].length_ms());
                            const double in = now - at;
                            t = float(len[si] >= 0.0 ? std::min(in, clip) : (clip > 0.0 ? std::fmod(in, clip) : 0.0));
                        }
                        return;
                    }
                    at += span;
                }
            };
            const float face = cat_->unit_facing_radians(aid);
            if (const formats::Model* am = models_->ensure(*ui_store_, aid)) {
                render::SceneInstance inst;
                inst.model = am;
                const std::array<float, 3> s = data::resolve_unit_model_scale(*cat_, data::unit_actor_id(*cat_, aid));
                inst.world = render::translation(fx, fy, fz) * render::rotation_z(face + 1.5707963f) * render::scaling(s[0], s[1], s[2]);
                ladder_pose(*am, construction_ladder(aid), inst.seq, inst.time_ms);
                if (inst.seq < 0) inst.seq = render::select_sequence(*am, {"Stand"}, 0);
                inst.team = team_colour_for(cat_, host.player, false);
                inst.team_emissive = team_emissive_for(cat_, host.player);
                scene.push_back(inst);
            }
            float sc3[3] = {1.0f, 1.0f, 1.0f};
            const std::string child = aid + "Build";
            if (const formats::Model* sm = models_->scaffold_model(*ui_store_, child, sc3)) {
                std::vector<std::vector<std::string>> ladder;
                for (const data::Catalog::ActorRule& r : cat_->actor_rules(child, "CActorModel"))
                    if (r.head == "ActorCreation" && r.verb == "Create" && !r.args.empty()) {
                        auto l = cat_->actor_progress_stages(r.args[0]);
                        if (!l.empty()) ladder = std::move(l);
                    }
                render::SceneInstance c;
                c.model = sm;
                float cyaw = 0.0f;
                if (auto f = cat_->actor_host_forward(child)) cyaw = std::atan2(f->second, f->first) + 1.5707963f;
                c.world = render::translation(fx, fy, fz) * render::rotation_z(cyaw) * render::scaling(sc3[0], sc3[1], sc3[2]);
                ladder_pose(*sm, ladder, c.seq, c.time_ms);
                c.team = team_colour_for(cat_, host.player, false);
                c.team_emissive = team_emissive_for(cat_, host.player);
                scene.push_back(c);
            }
        }
    }

    for (BeamFx& b : beams_) {
        if (!b.model || b.attacker >= world_.units().size()) continue;
        const Unit& from = world_.units()[b.attacker];
        if (fog_ready && from.player != cfg_.human_player && vision_.state_at(from.x, from.y) != sim::Visibility::Visible)
            continue;
        float a[3];
        unit_attach_world(from, b.heal ? "Ref_Center" : "Ref_Weapon", "Ref_Center", a);
        if (b.target < world_.units().size() && world_.units()[b.target].alive)
            unit_attach_world(world_.units()[b.target], "Ref_Center", nullptr, b.to);
        const float dx = b.to[0] - a[0], dy = b.to[1] - a[1];
        const float yaw = (dx != 0.0f || dy != 0.0f) ? std::atan2(dy, dx) : 0.0f;
        render::SceneInstance inst;
        inst.model = b.model;
        inst.world = render::translation(a[0], a[1], a[2]) * render::rotation_z(yaw + 1.5707963f) *
                     render::scaling(b.scale, b.scale, b.scale);
        inst.seq = b.seq[b.phase];
        inst.time_ms = b.time_ms;
        inst.team = {0, 0, 0, -1.0f};
        inst.beam_bone = b.beam_bone;
        inst.beam_target = {b.to[0], b.to[1], b.to[2]};
        scene.push_back(inst);
    }

    if (models_ && ui_store_) {
        for (const sim::World::ShotView& sv : world_.shots_in_flight()) {
            if (sv.attacker >= world_.units().size()) continue;
            const sim::Unit& shooter = world_.units()[sv.attacker];
            const std::string& wid = sv.slot == 1 ? shooter.wpn2_id : shooter.wpn_id;
            const formats::Model* mm = models_->missile_model(*ui_store_, wid);
            if (OMC_ENV_ONCE("OMC_SHOT_TRACE")) {
                static std::set<std::string> told;
                if (told.insert(shooter.catalog_id + "/" + wid).second)
                    std::fprintf(stderr, "[shot] %s weapon '%s' slot %d in flight -> %s\n", shooter.catalog_id.c_str(), wid.c_str(), int(sv.slot), mm ? "a missile model" : "NO MODEL");
            }
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
            float shot_launch_z = sz;
            {
                const float impact_z = sz;
                float launch_z = ground_z_at(float(shooter.x.to_double()), float(shooter.y.to_double()));
                if (shooter.id < driver_.actors.size()) {
                    render::ActorInstance& sa = driver_.actors[shooter.id];
                    render::Mat4 latt;
                    const int sseq = sa.model ? sa.current_sequence() : -1;
                    if (sseq >= 0 &&
                        (render::attachment_transform(*sa.model, "Ref_Weapon", std::uint32_t(sseq), sa.current_time(), latt) ||
                         render::attachment_transform(*sa.model, "Ref_Center", std::uint32_t(sseq), sa.current_time(), latt)))
                        launch_z += latt.m[14];
                    if (shooter.flying && cat_) launch_z += cat_->unit_display_height(shooter.catalog_id);
                }
                const float prog = std::min(1.0f, std::max(0.0f, float(sv.progress.to_double())));
                sz = launch_z + (impact_z - launch_z) * prog;
                shot_launch_z = launch_z;
            }
            render::SceneInstance shot;
            shot.model = mm;
            shot.world = render::translation(sx, sy, sz) * render::rotation_z(yaw + 1.5707963f);
            shot.team = {0, 0, 0, -1.0f};
            shot.seq = -1;
            static const bool kShotPath = std::getenv("OMC_SHOT_NO_PATH") == nullptr;
            if (kShotPath && sv.speed.raw() > 0) {
                shot.path_from = {float(sv.launch_x.to_double()), float(sv.launch_y.to_double()), shot_launch_z};
                shot.path_speed = float(sv.speed.to_double());
                float trail_s = 0.0f;
                for (const formats::M3Particle& pe : mm->particles)
                    if (pe.trailing_flag) trail_s = std::max(trail_s, std::max(pe.lifetime, pe.rand_life2 ? pe.lifetime2 : 0.0f));
                const std::string key = std::to_string(sv.attacker) + "/" + std::to_string(int(sv.slot)) + "/" + std::to_string(sv.launch_x.raw()) + "/" + std::to_string(sv.launch_y.raw());
                if (trail_s > 0.0f) shots_seen_[key] = SeenShot{shot, clock_ms_, trail_s};
            }
            scene.push_back(shot);
        }
        for (auto it = shots_seen_.begin(); it != shots_seen_.end();) {
            if (it->second.last_ms >= clock_ms_) { ++it; continue; }
            const float gone = float((clock_ms_ - it->second.last_ms) * 0.001);
            if (gone > it->second.trail_s || gone < 0.0f) { it = shots_seen_.erase(it); continue; }
            render::SceneInstance spent = it->second.inst;
            spent.path_ended_s = gone;
            scene.push_back(spent);
            ++it;
        }
        if (cat_)
            for (const sim::PendingEffect& pe : world_.pending_effects()) {
                if (pe.missile_tag == 0 || pe.flight.raw() <= 0) continue;
                if (effect_tag_names_.empty())
                    for (const std::string& id : cat_->ids("CEffect")) effect_tag_names_.emplace(behavior_id_hash(id), id);
                const auto nm = effect_tag_names_.find(pe.missile_tag);
                if (nm == effect_tag_names_.end()) continue;
                const formats::Model* mm = models_->missile_model(*ui_store_, nm->second);
                if (!mm) continue;
                float tx = float(pe.ctx.tx.to_double()), ty = float(pe.ctx.ty.to_double());
                if (pe.home && pe.ctx.target < world_.units().size() && world_.units()[pe.ctx.target].alive) {
                    tx = float(world_.units()[pe.ctx.target].x.to_double());
                    ty = float(world_.units()[pe.ctx.target].y.to_double());
                }
                const float lx = float(pe.lx.to_double()), ly = float(pe.ly.to_double());
                if (fog_ready && vision_.state_at(pe.lx, pe.ly) != sim::Visibility::Visible &&
                    vision_.state_at(pe.ctx.tx, pe.ctx.ty) != sim::Visibility::Visible)
                    continue;
                const float prog = std::min(1.0f, std::max(0.0f, 1.0f - float(pe.remaining.to_double() / pe.flight.to_double())));
                const float mx = lx + (tx - lx) * prog, my = ly + (ty - ly) * prog;
                float launch_z = ground_z_at(lx, ly) + 1.0f;
                if (pe.ctx.source < world_.units().size()) {
                    const sim::Unit& su = world_.units()[pe.ctx.source];
                    float a3[3];
                    unit_attach_world(su, "Ref_Weapon", "Ref_Center", a3);
                    launch_z = a3[2];
                }
                const float impact_z = ground_z_at(tx, ty) + 0.5f;
                render::SceneInstance shot;
                shot.model = mm;
                const float yaw = (tx != lx || ty != ly) ? std::atan2(ty - ly, tx - lx) : 0.0f;
                shot.world = render::translation(mx, my, launch_z + (impact_z - launch_z) * prog) * render::rotation_z(yaw + 1.5707963f);
                shot.team = {0, 0, 0, -1.0f};
                shot.seq = -1;
                scene.push_back(shot);
            }
    }

    const std::size_t fluff_from = tag_pass(fx_from, render::PassGroup::Effects);
    if (fluff_ && models_ && ui_store_) {
        static const float kFluffRange = std::getenv("OMC_FLUFF_RANGE") ? float(std::atof(std::getenv("OMC_FLUFF_RANGE"))) : 40.0f;
        const float fx = camctl_.focus.x, fy = camctl_.focus.y;
        if (fluff_draw_.size() != fluff_->size()) fluff_draw_.assign(fluff_->size(), DoodadDraw{});
        for (std::size_t di = 0; di < fluff_->size(); ++di) {
            const formats::PlacedDoodad& d = (*fluff_)[di];
            const float dx = d.x - fx, dy = d.y - fy;
            if (dx * dx + dy * dy > kFluffRange * kFluffRange) continue;
            DoodadDraw& dd = fluff_draw_[di];
            if (!dd.ready || !same_placement(dd.src, d)) {
                dd = DoodadDraw{};
                dd.src = d;
                dd.ready = true;
                dd.model = models_->foliage_model(*ui_store_, d.type, d.variation);
                if (dd.model) dd.world = app::doodad_world_matrix(d);
            }
            if (!dd.model) continue;
            render::SceneInstance inst;
            inst.model = dd.model;
            inst.world = dd.world;
            inst.team = {0, 0, 0, -2.0f};
            inst.seq = -1;
            scene.push_back(inst);
        }
    }
    const std::size_t doodads_from = tag_pass(fluff_from, render::PassGroup::Foliage);
    if (doodads_ && models_ && ui_store_) {
        const char* const env_doodad_var = OMC_ENV_ONCE("OMC_DOODAD_VAR");
        const char* const env_doodad_skip = OMC_ENV_ONCE("OMC_DOODAD_SKIP");
        const char* const env_doodad_xform = OMC_ENV_ONCE("OMC_DOODAD_XFORM");
        const char* const env_doodad_seq = OMC_ENV_ONCE("OMC_DOODAD_SEQ");
        static const bool kDoodadClock = std::getenv("OMC_DOODAD_FROZEN") == nullptr;
        static const bool kDoodadTakes = std::getenv("OMC_DOODAD_NO_VARIATIONS") == nullptr;
        const bool reuse = !env_doodad_var && !env_doodad_skip && !env_doodad_xform && !env_doodad_seq;
        const std::uint8_t density = std::uint8_t(render::render_quality().doodads);
        if (reuse && doodad_draw_.size() != doodads_->size()) doodad_draw_.assign(doodads_->size(), DoodadDraw{});
        for (std::size_t di = 0; di < doodads_->size(); ++di) {
            const formats::PlacedDoodad& d = (*doodads_)[di];
            if (reuse) {
                DoodadDraw& dd = doodad_draw_[di];
                if (!dd.ready || !same_placement(dd.src, d)) {
                    dd = DoodadDraw{};
                    dd.src = d;
                    dd.ready = true;
                    dd.model = models_->doodad_model(*ui_store_, d.type, d.variation);
                    if (dd.model) {
                        dd.world = app::doodad_world_matrix(d);
                        dd.seed = doodad_seed(d);
                        dd.seq = render::select_sequence(*dd.model, {"Stand"}, dd.seed);
                        auto tk = stand_takes_.find(dd.model);
                        if (tk == stand_takes_.end()) tk = stand_takes_.emplace(dd.model, render::variation_takes(*dd.model, "Stand")).first;
                        dd.takes = &tk->second;
                        dd.thin_at = app::doodad_thin_level(cat_, d, *dd.model);
                    }
                }
                if (!dd.model) continue;
                if (dd.thin_at != 0 && density >= dd.thin_at) continue;
                render::SceneInstance inst;
                inst.model = dd.model;
                inst.world = dd.world;
                inst.team = {0, 0, 0, -2.0f};
                inst.seq = dd.seq;
                if (kDoodadClock && inst.seq >= 0 && std::size_t(inst.seq) < dd.model->sequences.size()) {
                    const double len = double(dd.model->sequences[std::size_t(inst.seq)].length_ms());
                    if (len > 0.0) inst.time_ms = float(std::fmod(clock_ms_, len));
                }
                if (kDoodadTakes && kDoodadClock && inst.seq >= 0) {
                    float take_ms = 0.0f;
                    const int take = render::select_variation(*dd.model, *dd.takes, dd.seed, double(clock_ms_), &take_ms, dd.walk);
                    if (take >= 0) {
                        inst.seq = take;
                        inst.time_ms = take_ms;
                    }
                }
                scene.push_back(inst);
                continue;
            }
            int dvar = d.variation;
            if (const char* dv = env_doodad_var) {
                const std::string spec = dv;
                const std::size_t c1 = spec.find(':');
                if (c1 != std::string::npos && spec.compare(0, c1, d.type) == 0) dvar = std::atoi(spec.c_str() + c1 + 1);
            }
            if (const char* sk = env_doodad_skip)
                if (d.type == sk) continue;
            const formats::Model* dm = models_->doodad_model(*ui_store_, d.type, dvar);
            if (!dm) continue;
            render::SceneInstance inst;
            inst.model = dm;
            inst.world = app::doodad_world_matrix(d);
            if (const char* dx = env_doodad_xform) {
                const std::string spec = dx;
                const std::size_t c1 = spec.find(':');
                float sxy = 1.0f, sz = 1.0f, dz = 0.0f;
                if (c1 != std::string::npos && spec.compare(0, c1, d.type) == 0 && std::sscanf(spec.c_str() + c1 + 1, "%f:%f:%f", &sxy, &sz, &dz) >= 1)
                    inst.world = render::translation(0.0f, 0.0f, dz) * inst.world * render::scaling(sxy, sxy, sz);
            }
            inst.team = {0, 0, 0, -2.0f};
            const std::uint64_t dseed = doodad_seed(d);
            inst.seq = render::select_sequence(*dm, {"Stand"}, dseed);
            if (kDoodadClock && inst.seq >= 0 && std::size_t(inst.seq) < dm->sequences.size()) {
                const double len = double(dm->sequences[std::size_t(inst.seq)].length_ms());
                if (len > 0.0) inst.time_ms = float(std::fmod(clock_ms_, len));
            }
            if (kDoodadTakes && kDoodadClock && inst.seq >= 0) {
                float take_ms = 0.0f;
                const int take = render::select_variation(*dm, "Stand", dseed, double(clock_ms_), &take_ms);
                if (take >= 0) {
                    inst.seq = take;
                    inst.time_ms = take_ms;
                }
            }
            if (const char* ds = env_doodad_seq) {
                const std::string spec = ds;
                const std::size_t c1 = spec.find(':');
                if (c1 != std::string::npos && spec.compare(0, c1, d.type) == 0) {
                    static std::set<const formats::Model*> listed;
                    if (listed.insert(dm).second) {
                        std::fprintf(stderr, "[doodadseq] %s var %d picked %d bones %zu at %.2f %.2f %.2f rot %.3f scale %.3f %.3f %.3f verts %u\n", d.type.c_str(), int(d.variation), inst.seq,
                                     dm->bones.size(), double(d.x), double(d.y), double(d.z), double(d.rotation), double(d.sx), double(d.sy), double(d.sz), dm->vertex_count);
                        for (std::size_t qi = 0; qi < dm->sequences.size(); ++qi)
                            std::fprintf(stderr, "[doodadseq]   %zu '%s' %u ms\n", qi, dm->sequences[qi].name.c_str(), dm->sequences[qi].length_ms());
                    }
                    const std::size_t c2 = spec.find(':', c1 + 1);
                    const int want = std::atoi(spec.c_str() + c1 + 1);
                    if (want >= 0 && std::size_t(want) < dm->sequences.size()) inst.seq = want;
                    if (want == -2) inst.seq = -1;
                    if (c2 != std::string::npos) inst.time_ms = float(std::atof(spec.c_str() + c2 + 1));
                }
            }
            scene.push_back(inst);
        }
    }
    const std::size_t cliffs_from = tag_pass(doodads_from, render::PassGroup::Doodads);
    if (cliffs_) scene.insert(scene.end(), cliffs_->begin(), cliffs_->end());
    tag_pass(cliffs_from, render::PassGroup::Terrain);
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

bool MeleeGame::modal_armed() const {
    return has_pending_ability_cast() || pending_warp_ || pending_build_ || pending_refinery_ || pending_land_ ||
           pending_unload_ || pending_defender_ || pending_root_ || pending_attack_ || pending_patrol_ || pending_move_;
}

void MeleeGame::clear_armed_modals() {
    cancel_ability_cast();
    pending_warp_ = false;
    pending_warp_gate_ = sim::kNoTarget;
    pending_warp_unit_.clear();
    pending_build_ = pending_refinery_ = false;
    pending_land_ = false;
    pending_land_unit_ = sim::kNoTarget;
    pending_land_addon_.clear();
    pending_unload_ = false;
    pending_unload_unit_ = sim::kNoTarget;
    pending_defender_ = false;
    pending_liberator_ = sim::kNoTarget;
    pending_root_ = false;
    pending_root_abil_.clear();
    pending_attack_ = pending_patrol_ = pending_move_ = false;
}

bool MeleeGame::minimap_place_armed(sim::Fixed wx, sim::Fixed wy, bool queued) {
    if (has_pending_ability_cast()) {
        if (pending_cast_mode_ == AbilTargetMode::Point) {
            const std::uint32_t caster = human_lead_caster();
            if (caster != sim::kNoTarget && is_nova_cast(caster, pending_cast_abil_)) {
                if (world_.launch_nova(caster, wx, wy)) cancel_ability_cast();
            } else if (cast_pending_at_point(wx, wy, queued)) {
                cue_confirm_point(float(wx.to_double()), float(wy.to_double()), false);
            }
        }
        return true;
    }
    if (pending_land_) {
        if (pending_land_unit_ < world_.units().size() && world_.unit(pending_land_unit_).alive &&
            world_.unit(pending_land_unit_).flying) {
            land_site(pending_land_unit_, wx, wy);
            const bool ordered = !pending_land_addon_.empty()
                                     ? human_land_addon(pending_land_unit_, wx, wy, pending_land_addon_)
                                     : world_.order_land(pending_land_unit_, wx, wy);
            if (ordered) {
                pending_land_ = false;
                pending_land_unit_ = sim::kNoTarget;
                pending_land_addon_.clear();
            }
        }
        return true;
    }
    if (pending_unload_) {
        if (pending_unload_unit_ < world_.units().size() && world_.unit(pending_unload_unit_).alive) {
            const bool mobile = world_.unit(pending_unload_unit_).speed.raw() > 0;
            if (mobile) world_.issue(pending_unload_unit_, sim::Order{sim::OrderType::Move, wx, wy, sim::kNoTarget});
            world_.issue(pending_unload_unit_, sim::Order{sim::OrderType::Unload, wx, wy, sim::kNoTarget}, mobile);
        }
        pending_unload_ = false;
        pending_unload_unit_ = sim::kNoTarget;
        return true;
    }
    if (pending_defender_) {
        human_siege_defender(pending_liberator_, wx, wy);
        pending_defender_ = false;
        pending_liberator_ = sim::kNoTarget;
        return true;
    }
    if (pending_root_) {
        root_order(wx, wy);
        return true;
    }
    return false;
}

bool MeleeGame::root_site(const std::string& abil, std::uint32_t id, sim::Fixed& x, sim::Fixed& y) const {
    if (!cat_ || id >= world_.units().size()) return false;
    const auto ms = cat_->morph_spec(abil);
    if (!ms || ms->target.empty()) return false;
    double fx = 0.5, fy = 0.5;
    if (cat_->unit_placement_grid_phase(ms->target, fx, fy)) {
        auto snap = [](sim::Fixed v, double phase) {
            return sim::Fixed::from_double(std::floor(v.to_double() - phase + 0.5) + phase);
        };
        x = snap(x, fx);
        y = snap(y, fy);
    }
    if (x.to_double() < 0.0 || y.to_double() < 0.0 || x.to_double() > double(map_w_) || y.to_double() > double(map_h_))
        return false;
    sim::Unit proto = make_unit(*cat_, ms->target, cfg_.human_player);
    proto.speed = sim::Fixed();
    return world_.morph_site_ok(proto, x, y, id);
}

bool MeleeGame::root_order(sim::Fixed wx, sim::Fixed wy) {
    std::uint32_t best = sim::kNoTarget;
    double best_d = 0.0;
    for (std::uint32_t id : selection_) {
        if (root_walks_.count(id) || !morph_command_allowed(id, pending_root_abil_)) continue;
        const sim::Unit& u = world_.unit(id);
        const double dx = u.x.to_double() - wx.to_double(), dy = u.y.to_double() - wy.to_double();
        const double d = dx * dx + dy * dy;
        if (best == sim::kNoTarget || d < best_d) {
            best = id;
            best_d = d;
        }
    }
    if (best == sim::kNoTarget || !root_site(pending_root_abil_, best, wx, wy)) return false;
    world_.issue(best, sim::Order{sim::OrderType::Move, wx, wy, sim::kNoTarget});
    root_walks_[best] = RootWalk{pending_root_abil_, wx, wy};
    pending_root_ = false;
    pending_root_abil_.clear();
    return true;
}

void MeleeGame::step_root_walks() {
    for (auto it = root_walks_.begin(); it != root_walks_.end();) {
        const std::uint32_t id = it->first;
        const RootWalk walk = it->second;
        if (id >= world_.units().size() || !world_.unit(id).alive || world_.is_morphing(id)) {
            it = root_walks_.erase(it);
            continue;
        }
        const sim::Unit& u = world_.unit(id);
        const bool walking = !u.orders.empty() && u.orders.front().type == sim::OrderType::Move &&
                             u.orders.front().tx == walk.x && u.orders.front().ty == walk.y;
        if (!u.orders.empty() && !walking) {
            it = root_walks_.erase(it);
            continue;
        }
        const double dx = u.x.to_double() - walk.x.to_double(), dy = u.y.to_double() - walk.y.to_double();
        const double d2 = dx * dx + dy * dy;
        if (d2 <= 0.01 || (!walking && d2 <= 1.0)) {
            sim::Fixed sx = walk.x, sy = walk.y;
            if (root_site(walk.abil, id, sx, sy)) {
                world_.stop(id);
                world_.teleport_unit(id, sx, sy);
                human_morph(id, walk.abil);
            }
            it = root_walks_.erase(it);
            continue;
        }
        if (!walking) {
            it = root_walks_.erase(it);
            continue;
        }
        ++it;
    }
}

bool MeleeGame::cancel_pending_modal() {
    if (has_pending_ability_cast()) { cancel_ability_cast(); return true; }
    if (pending_root_) { pending_root_ = false; pending_root_abil_.clear(); return true; }
    if (hud_ui_.minimap.ping_armed) { hud_ui_.minimap.ping_armed = false; return true; }
    if (pending_warp_) {
        pending_warp_ = false;
        pending_warp_gate_ = sim::kNoTarget;
        pending_warp_unit_.clear();
    }
    if (pending_build_ || pending_refinery_) {
        pending_build_ = pending_refinery_ = false;
        return true;
    }
    if (pending_land_) { pending_land_ = false; pending_land_unit_ = sim::kNoTarget; pending_land_addon_.clear(); return true; }
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
    bool settings_placed = false;
    if (ui_store_ && view_w && view_h)
        if (const HudPauseRects* pr = hud_pause_rects(*ui_store_, float(view_w) / float(view_h))) {
            const double sc = double(view_h) / double(kRefHeight);
            auto at = [sc](const HudAuthoredRects::R& r) {
                return PauseRect{int(r.x0 * sc + 0.5), int(r.y0 * sc + 0.5), int(r.x1 * sc + 0.5), int(r.y1 * sc + 0.5)};
            };
            if (pr->save.ok()) { out.save = at(pr->save); out.has_save = true; }
            if (pr->load.ok()) { out.load = at(pr->load); out.has_load = true; }
            if (pr->settings.ok() && at(pr->resume).y0 == out.resume.y0) {
                out.settings = at(pr->settings);
                settings_placed = true;
            }
        }
    if (!settings_placed) {
        const int h = out.resume.y1 - out.resume.y0;
        const int gap = std::max(4, (out.quit.y0 - out.resume.y1) > 0 ? out.quit.y0 - out.resume.y1 : 20);
        out.settings = {out.resume.x0, out.resume.y0 - gap - h, out.resume.x1, out.resume.y0 - gap};
    }
    out.has_settings = bool(on_settings_);
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
                if (audio_->play_event(u.catalog_id, data::UnitEvent::Ready, 1.0f, listener_distance_to(u), listener_pan_to(u)))
                    mark(i, data::UnitEvent::Ready);
            audio_alive_[i] = u.alive ? 1 : 0;
            audio_cd_[i] = u.cooldown;
            continue;
        }
        if (audible && audio_alive_[i] && !u.alive && !u.catalog_id.empty())
            if (audio_->play_event(u.catalog_id, data::UnitEvent::Death, 1.0f, listener_distance_to(u), listener_pan_to(u)))
                mark(i, data::UnitEvent::Death);
        if (audible && u.alive && u.cooldown > audio_cd_[i] && audio_cd_[i] == zero && !u.catalog_id.empty())
            if (audio_->play_event(u.catalog_id, data::UnitEvent::Attack, kCombatCueTrim, listener_distance_to(u), listener_pan_to(u)))
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
    audio_->play_event(u.catalog_id, ev, gain, listener_distance_to(u), listener_pan_to(u));
}

void MeleeGame::snap_build_site(sim::Fixed& wx, sim::Fixed& wy) const {
    if (!cat_ || build_proto_.catalog_id.empty()) return;
    double sfx = 0.5, sfy = 0.5;
    if (!cat_->unit_placement_grid_phase(build_proto_.catalog_id, sfx, sfy)) return;
    auto snap = [](sim::Fixed v, double phase) { return sim::Fixed::from_double(std::floor(v.to_double() - phase + 0.5) + phase); };
    wx = snap(wx, sfx);
    wy = snap(wy, sfy);
}

bool MeleeGame::build_site_on_grid(sim::Fixed wx, sim::Fixed wy) const {
    if (!world_.placement().configured()) return true;
    int gcx = 0, gcy = 0;
    world_.placement().to_cell(wx, wy, gcx, gcy);
    int gminx = 0, gmaxx = 0, gminy = 0, gmaxy = 0;
    for (const std::pair<std::int16_t, std::int16_t>& o : build_proto_.footprint_offsets) {
        if (o.first < gminx) gminx = o.first;
        if (o.first > gmaxx) gmaxx = o.first;
        if (o.second < gminy) gminy = o.second;
        if (o.second > gmaxy) gmaxy = o.second;
    }
    return world_.placement().can_place(gcx + gminx, gcy + gminy, gmaxx - gminx + 1, gmaxy - gminy + 1, build_proto_.requires_vespene) &&
           sim::footprint_clear_of_nobuild(world_, world_.placement(), gcx + gminx, gcy + gminy, gmaxx - gminx + 1,
                                           gmaxy - gminy + 1);
}

void MeleeGame::apply_start_grants() {
    if (std::getenv("OMC_NO_START_GRANTS") || !cat_) return;
    for (std::uint32_t h = 0; h < world_.units().size(); ++h) {
        const sim::Unit& hall = world_.unit(h);
        if (!hall.alive || hall.player < 0 || hall.catalog_id.empty() || !cat_->find("CUnit", hall.catalog_id)) continue;
        std::string create_set;
        for (const core::XmlNode& n : cat_->resolved("CUnit", hall.catalog_id))
            if (n.name == "EffectArray" && n.attr_or("index", "") == "Create")
                if (const std::string* v = n.attr("value"); v && !v->empty()) create_set = *v;
        if (create_set.empty() || !cat_->find("CEffect", create_set)) continue;
        for (const core::XmlNode& m : cat_->resolved("CEffect", create_set)) {
            if (m.name != "EffectArray") continue;
            const std::string* eff = m.attr("value");
            if (!eff || eff->empty() || !cat_->find("CEffect", *eff)) continue;
            for (const core::XmlNode& u : cat_->resolved("CEffect", *eff))
                if (u.name == "Upgrades")
                    if (const std::string* up = u.attr("Upgrade"); up && !up->empty() && u.attr_or("Count", "1") != "0") {
                        world_.grant_upgrade(hall.player, *up);
                        if (std::getenv("OMC_START_RALLY_TRACE"))
                            std::fprintf(stderr, "[start-grant] %s %u (player %d): %s -> %s\n", hall.catalog_id.c_str(), h, hall.player, eff->c_str(), up->c_str());
                    }
        }
    }
}

void MeleeGame::apply_start_rally() {
    if (!std::getenv("OMC_NO_START_RALLY") && cat_) {
        const sim::Fixed reach = sim::Fixed::from_ratio(13, 2);
        const std::size_t n = world_.units().size();
        for (std::uint32_t h = 0; h < n; ++h) {
            const sim::Unit& hall = world_.unit(h);
            if (std::getenv("OMC_START_RALLY_TRACE") && (hall.catalog_id == "CommandCenter" || hall.catalog_id == "Nexus" || hall.catalog_id == "Hatchery"))
                std::fprintf(stderr, "[start-rally] candidate %s %u alive %d player %d dropoff %d has_rally %d worker_rally %d rally_abil '%s'\n", hall.catalog_id.c_str(), h,
                             int(hall.alive), hall.player, int(hall.drop_off_ground), int(hall.has_rally), int(hall.has_worker_rally),
                             cat_->unit_rally_ability(hall.catalog_id).c_str());
            if (!hall.alive || hall.player < 0 || hall.is_worker || !hall.drop_off_ground || hall.catalog_id.empty() || hall.has_rally ||
                hall.has_worker_rally || cat_->unit_rally_ability(hall.catalog_id).empty())
                continue;
            std::uint32_t best = sim::kNoTarget;
            sim::Fixed best_d2;
            for (std::uint32_t f = 0; f < n; ++f) {
                const sim::Unit& fld = world_.unit(f);
                if (!fld.alive || fld.resource_left <= 0 || fld.is_gas) continue;
                const sim::Fixed dx = fld.x - hall.x, dy = fld.y - hall.y;
                const sim::Fixed d2 = dx * dx + dy * dy;
                const sim::Fixed one = sim::Fixed::from_int(1), half = sim::Fixed::from_ratio(1, 2);
                sim::Fixed bx = (dx.raw() < 0 ? sim::Fixed() - dx : dx) - one, by = (dy.raw() < 0 ? sim::Fixed() - dy : dy) - half;
                if (bx.raw() < 0) bx = sim::Fixed();
                if (by.raw() < 0) by = sim::Fixed();
                if (std::getenv("OMC_START_RALLY_TRACE"))
                    std::fprintf(stderr, "[start-rally]   field %u at (%.2f, %.2f): centre %.2f, footprint %.2f\n", f, fld.x.to_double(), fld.y.to_double(),
                                 std::sqrt(d2.to_double()), std::sqrt((bx * bx + by * by).to_double()));
                if (bx * bx + by * by > reach * reach) continue;
                if (best == sim::kNoTarget || d2 > best_d2) { best = f; best_d2 = d2; }
            }
            if (best == sim::kNoTarget) continue;
            world_.set_rally(h, world_.unit(best).x, world_.unit(best).y);
            if (std::getenv("OMC_START_RALLY_TRACE"))
                std::fprintf(stderr, "[start-rally] %s %u at (%.2f, %.2f) -> field %u at (%.2f, %.2f), %.2f away (radius %.3f)\n", hall.catalog_id.c_str(), h,
                             hall.x.to_double(), hall.y.to_double(), best, world_.unit(best).x.to_double(), world_.unit(best).y.to_double(),
                             std::sqrt(best_d2.to_double()), world_.unit(best).radius.to_double());
        }
    }
}

bool MeleeGame::step(const render::InputState& in, float dt_ms,
                     std::vector<render::SceneInstance>& scene, render::Camera& cam) {
    ensure_audio();
    const float dt_s = dt_ms / 1000.0f;
    fog_dt_s_ = dt_s;
    clock_ms_ += double(dt_ms);
    {
        const std::size_t np = layout_.players.size();
        if (upg_seen_.size() != np) { upg_seen_.assign(np, 0); upg_flash_s_.assign(np, 0.0f);
            for (std::size_t p = 0; p < np; ++p) upg_seen_[p] = world_.researched(int32_t(p)).size(); }
        for (std::size_t p = 0; p < np; ++p) {
            const std::size_t n = world_.researched(int32_t(p)).size();
            if (n > upg_seen_[p]) upg_flash_s_[p] = 0.5f;
            upg_seen_[p] = n;
            if (upg_flash_s_[p] > 0.0f) upg_flash_s_[p] = std::max(0.0f, upg_flash_s_[p] - dt_s);
        }
    }
    user_scrolled_ = false;
    if (world_.cargo_hidden())
        selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                        [&](std::uint32_t id) {
                                            return id < world_.units().size() && world_.unit(id).loaded;
                                        }),
                         selection_.end());
    const float aspect = in.view_h ? float(in.view_w) / float(in.view_h) : 1.0f;

    if (dt_ms > 0.0f) {
        const float inst = 1000.0f / dt_ms;
        fps_smooth_ = fps_smooth_ > 0.0f ? fps_smooth_ * 0.9f + inst * 0.1f : inst;
    }
    mouse_x_ = in.mouse_x;
    mouse_y_ = in.mouse_y;
    alt_held_ = in.key_alt;
    hud_ui_tick(dt_s);

    const bool esc_edge = in.quit && !prev_esc_;
    prev_esc_ = in.quit;
    static const bool esc_opens = std::getenv("OMC_ESC_OPENS_MENU") != nullptr;
    bool menu_key = false;
    if (cat_ && !cat_->hotkeys().empty()) menu_key = ui_hotkey_fires(cat_->hotkeys(), "MenuGame", in);
    else
        for (const std::string& t : in.key_tokens) menu_key = menu_key || t == "F10";
    esc_card_ = false;
    if (esc_edge) {
        if (pause_menu_open_) { pause_menu_open_ = false; clock_.set_paused(hud_ui_.menu.paused_by_key); }
        else if (cancel_pending_modal()) {   }
        else if (esc_opens) open_pause_menu();
        else esc_card_ = true;
    } else if (menu_key && !pause_menu_open_) {
        open_pause_menu();
    }
    if (pause_menu_open_) {
        if (in.left_pressed && in.view_w && in.view_h) {
            PauseRect resume, quit;
            pause_menu_rects(in.view_w, in.view_h, resume, quit);
            auto hit = [&](const PauseRect& r) {
                return in.mouse_x >= float(r.x0) && in.mouse_x < float(r.x1) &&
                       in.mouse_y >= float(r.y0) && in.mouse_y < float(r.y1);
            };
            if (hit(resume)) { pause_menu_open_ = false; clock_.set_paused(hud_ui_.menu.paused_by_key); }
            else if (hit(quit)) {
                if (cfg_.human_player >= 0) surrender(cfg_.human_player);
                return false;
            }
            else {
                const PauseRects all = pause_rects_all(in.view_w, in.view_h);
                if (all.has_settings && hit(all.settings)) on_settings_();
                else if (all.has_save && hit(all.save) && on_save_) on_save_();
                else if (all.has_load && hit(all.load) && on_load_) on_load_();
            }
        }
        emit_scene(scene);
        cam = camctl_.camera();
        return true;
    }

    if (!mem_step_marked_ && render::mem_mark_fn()) {
        render::mem_mark("first step: begin");
        render::mem_lap_marks() = true;
    }
    render::FrameProfLap step_lap;
    update_camera(in, dt_s);
    step_lap.lap("st.camera");

    if (!framed_ && in.view_w && in.view_h) {
        std::vector<render::Vec3> pts;
        for (const Unit& u : world_.units())
            if (u.alive) pts.push_back({float(u.x.to_double()), float(u.y.to_double()), 0.0f});
        camctl_.fit(pts, aspect);
        framed_ = true;
    }

    if (!replay_drive_) handle_commands(in, aspect);
    step_lap.lap("st.commands");

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
                                          sim::Fixed::from_int(2), build_verb_for(q.worker))) {
                cue_build_issued(q.worker, q.x, q.y, q.proto.catalog_id);
                if (q.is_producer) human_producer_pending_ = true;
            }
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
        auto steal = [&](int keep) { steal_control_groups(keep); };
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
            const std::vector<uint32_t> army = army_units();
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

    if (!replay_drive_) hud_dispatch_hotkeys(in);
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

    step_lap.lap("st.hotkeys");
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
        if (!start_grants_done_) {
            start_grants_done_ = true;
            apply_start_grants();
            static const bool kStartGrantFlashOld = std::getenv("OMC_START_GRANT_FLASH_OLD") != nullptr;
            if (!kStartGrantFlashOld)
                for (std::size_t p = 0; p < upg_seen_.size(); ++p) upg_seen_[p] = world_.researched(int32_t(p)).size();
        }
        if (!replay_drive_ && !start_rally_done_) {
            start_rally_done_ = true;
            apply_start_rally();
        }
        if (!defender_approach_.empty()) step_defender_approach();
        {
            OMC_PROF_SCOPE("st.world");
            world_.step();
        }
        if (const char* lt = OMC_ENV_ONCE("OMC_LIFE_TRACE")) {
            static std::map<std::uint32_t, double> last;
            for (const sim::Unit& lu : world_.units()) {
                if (!lu.alive || lu.catalog_id != lt) continue;
                const double v = lu.hp.to_double() + lu.shields.to_double();
                auto it = last.find(lu.id);
                if (it == last.end() || it->second != v)
                    std::fprintf(stderr, "[life] tick %d %s %u life %.1f shields %.1f\n", world_.tick(), lt, unsigned(lu.id), lu.hp.to_double(), lu.shields.to_double());
                last[lu.id] = v;
            }
        }
        if (research_) research_->apply_completed(upgrade_fold_new_);
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
        auto drive_ai = [&](size_t p) {
            if (ai_paused_ || world_.player(int32_t(p)).defeated) return;
            OMC_PROF_SCOPE("st.ai");
            ais_[p]->update(world_);
            maybe_ai_research(int(p));
            if (ai_inject_) run_ai_inject(int(p));
            if (cast_policy_) cast_policy_->run(world_, int(p));
        };
        auto own_period = [&](size_t p) { return p < ai_own_period_.size() ? ai_own_period_[p] : 0; };
        if (!replay_drive_ && !ai_own_period_.empty()) {
            if (ai_own_counter_.size() < ai_own_period_.size()) ai_own_counter_.resize(ai_own_period_.size(), 0);
            for (size_t p = 0; p < ais_.size(); ++p)
                if (own_period(p) > 0 && (cfg_.human_player < 0 || int(p) != cfg_.human_player) &&
                    ++ai_own_counter_[p] >= own_period(p)) {
                    ai_own_counter_[p] = 0;
                    drive_ai(p);
                }
        }
        if (!replay_drive_ && ++tick_counter_ >= cfg_.ai_period_ticks) {
            tick_counter_ = 0;
            for (size_t p = 0; p < ais_.size(); ++p)
                if ((cfg_.human_player < 0 || int(p) != cfg_.human_player) && own_period(p) == 0) drive_ai(p);
            static const bool kScanOld = std::getenv("OMC_LOCK_ON_OLD") != nullptr;
            if (kScanOld && cast_policy_ && cfg_.human_player >= 0)
                cast_policy_->run_autocast(world_, cfg_.human_player);
        }
        static const bool kScanFastOld = std::getenv("OMC_LOCK_ON_OLD") != nullptr;
        if (!kScanFastOld && !replay_drive_ && cast_policy_ && cfg_.human_player >= 0 && world_.tick() % 4 == 0)
            cast_policy_->run_autocast(world_, cfg_.human_player);
        if (!replay_drive_) run_morph_autocast();
        if (!root_walks_.empty()) step_root_walks();
    }
    step_lap.lap("st.sim");

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
            bool swapped = false;
            if (u.player >= 0)
                for (const auto& sw : upgrade_model_swaps(u.catalog_id)) {
                    if (!world_.has_upgrade(u.player, sw.first)) continue;
                    if (const formats::Model* m = models_->cmodel(*ui_store_, sw.second)) {
                        driver_.set_model(std::uint32_t(id), m);
                        swapped = true;
                        break;
                    }
                }
            if (swapped) continue;
            if (const std::string& osw = option_model_swap(actor_cat_.get(), u.catalog_id); !osw.empty())
                if (const formats::Model* m = models_->cmodel(*ui_store_, osw)) {
                    driver_.set_model(std::uint32_t(id), m);
                    continue;
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
            const std::string& cid = world_.morph_shown_id(std::uint32_t(id));
            const std::uint8_t building = bu.constructing ? 1 : 0;
            if (bound_cat_[id] == cid && bound_building_[id] == building) continue;
            bound_cat_[id] = cid;
            bound_building_[id] = building;
            if (cid.empty()) continue;
            const formats::Model* m = building ? models_->ensure_build(*ui_store_, cid) : nullptr;
            if (!m)
                if (const std::string& osw = option_model_swap(actor_cat_.get(), cid); !osw.empty()) {
                    m = models_->cmodel(*ui_store_, osw);
                    if (std::getenv("OMC_SHADER_OPTION_TRACE"))
                        std::fprintf(stderr, "[optswap] unit %zu %s -> %s: %s\n", id, cid.c_str(), osw.c_str(), m ? "bound" : "DID NOT RESOLVE");
                }
            static const bool kVarById = std::getenv("OMC_UNIT_VARIATION_BY_ID") != nullptr;
            const int authored_var = (!kVarById && id < layout_.unit_variation.size()) ? layout_.unit_variation[id] : -1;
            if (!m && !OMC_ENV_ONCE("OMC_NO_UNIT_VARIATIONS"))
                m = models_->ensure_variation(*ui_store_, cid, authored_var >= 0 ? unsigned(authored_var) : unsigned(id));
            if (!m) m = models_->ensure(*ui_store_, cid);
            if (m) driver_.set_model(std::uint32_t(id), m);
        }
    }

    step_lap.lap("st.bind");
    const float actor_dt = !actor_game_clock_ ? dt_ms
                                              : (clock_.paused() ? 0.0f : dt_ms * float(clock_.multiplier(clock_.speed())));
    driver_.observe(world_, actor_dt, ticked);
    cloak_clock_ms_ += double(actor_dt);
    step_lap.lap("st.driver");
    drain_train_events();
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
        for (const sim::World::ResourceEmptyEvent& e : world_.take_resource_empty_events()) {
            if (cfg_.human_player < 0 || e.player != cfg_.human_player) continue;
            std::string family;
            if (cat_ && e.node < world_.units().size())
                family = cat_->resource_exhausted_alert(world_.units()[e.node].catalog_id);
            messages_.emit(MsgZone::Alert, e.is_gas ? "Vespene geyser depleted" : "Mineral field depleted",
                           10.0f, true, float(e.x.to_double()), float(e.y.to_double()));
            const std::string sid = alert_sound_id(AlertKind::ResourceExhausted, family);
            if (sid.empty()) continue;
            last_alert_sound_ = sid;
            if (audio_) audio_->play_sound(sid, 1.0f);
        }
    }
    if (world_.last_error() != sim::CmdError::None) {
        if (cat_ && world_.last_error_player() == cfg_.human_player) {
            const char* key = sim::cmd_error_key(world_.last_error());
            if (const std::string* text = cat_->strings().find(key); text && !text->empty()) {
                float ttl = 5.0f;
                if (hud_authored_ && hud_authored_->error_timing.ok())
                    ttl = hud_authored_->error_timing.duration_ms / 1000.0f;
                messages_.emit(MsgZone::Error, *text, ttl);
            }
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
                    const auto snap = fog_snaps_.find(id);
                    if (snap == fog_snaps_.end() || !snap->second.has_splat) splat_quads_.erase(id);
                }
            }
        }
        if (constructing_seen_.size() < world_.units().size()) constructing_seen_.resize(world_.units().size(), 0);
        for (std::uint32_t id = 0; id < world_.units().size(); ++id) {
            const Unit& u = world_.unit(id);
            constructing_seen_[id] = (u.alive && u.constructing) ? 1 : 0;
        }
    }
    if (ticked && models_ && ui_store_) {
        for (const sim::World::HitEvent& h : world_.take_hit_events()) {
            if (h.attacker >= world_.units().size() || h.target >= world_.units().size()) continue;
            const Unit& shooter = world_.units()[h.attacker];
            const Unit& victim = world_.units()[h.target];
            const std::string& wid = h.slot == 1 ? shooter.wpn2_id : shooter.wpn_id;
            static const bool kFlareOld = std::getenv("OMC_SHIELD_FLARE_OLD") != nullptr;
            static const data::Catalog* flare_cat = nullptr;
            static app::ShieldFlareSpec flare;
            if (cat_ && flare_cat != cat_) { flare = app::shield_flare_spec(*cat_); flare_cat = cat_; }
            const bool flare_data = !kFlareOld && flare.valid();
            if (victim.shields_max.raw() > 0 && h.on_shields) {
                if (const formats::Model* sm = models_->cmodel(*ui_store_, flare_data ? flare.model : std::string("ProtossShieldDamage")); sm && !sm->sequences.empty()) {
                    int sseq = render::select_sequence(*sm, flare_data ? flare.clip : std::vector<std::string>{"Birth"}, h.target);
                    if (sseq < 0) sseq = render::select_sequence(*sm, {"Stand"}, h.target);
                    if (sseq < 0) sseq = 0;
                    ImpactFx sf;
                    sf.model = sm;
                    sf.seq = sseq;
                    sf.length_ms = float(sm->sequences[std::size_t(sseq)].length_ms());
                    float c3[3];
                    unit_attach_world(victim, "Ref_Center", nullptr, c3);
                    sf.x = c3[0]; sf.y = c3[1]; sf.z = c3[2];
                    const float ax = float(shooter.x.to_double()) - sf.x, ay = float(shooter.y.to_double()) - sf.y;
                    if (ax != 0.0f || ay != 0.0f) sf.yaw = std::atan2(ay, ax);
                    const float dmg01 = std::min(1.0f, std::max(0.0f, (float(h.amount.to_double()) - 5.0f) / (45.0f - 5.0f)));
                    sf.scale = 0.7f + (2.0f - 0.7f) * dmg01;
                    if (flare_data) sf.scale = app::shield_flare_radius(flare, float(h.amount.to_double()));
                    sf.player = victim.player;
                    if (impact_fx_.size() < 512) impact_fx_.push_back(sf);
                    const float an = std::sqrt(ax * ax + ay * ay);
                    if (flare_data && !flare.ripple_model.empty() && an > 0.0f) {
                        if (const formats::Model* rm = models_->cmodel(*ui_store_, flare.ripple_model); rm && !rm->sequences.empty()) {
                            int rseq = render::select_sequence(*rm, flare.ripple_clip, h.target);
                            if (rseq < 0) rseq = 0;
                            ImpactFx rf = sf;
                            rf.model = rm;
                            rf.seq = rseq;
                            rf.length_ms = float(rm->sequences[std::size_t(rseq)].length_ms());
                            rf.x = sf.x + ax / an * sf.scale;
                            rf.y = sf.y + ay / an * sf.scale;
                            rf.scale = 1.0f;
                            if (impact_fx_.size() < 512) impact_fx_.push_back(rf);
                        }
                    }
                }
            }
            const formats::Model* fm = models_->impact_model(*ui_store_, wid);
            if (OMC_ENV_ONCE("OMC_SHOT_TRACE")) {
                static std::set<std::string> told_impact;
                if (told_impact.insert(shooter.catalog_id + "/" + wid).second)
                    std::fprintf(stderr, "[impact] %s weapon '%s' hit -> %s (%zu clips)\n", shooter.catalog_id.c_str(), wid.c_str(), fm ? "an impact model" : "NO IMPACT",
                                 fm ? fm->sequences.size() : std::size_t(0));
            }
            if (!fm || fm->sequences.empty()) continue;
            int seq = render::select_sequence(*fm, {"Birth"}, h.target);
            if (seq < 0) seq = render::select_sequence(*fm, {"Stand"}, h.target);
            if (seq < 0) seq = 0;
            ImpactFx fx;
            fx.model = fm;
            fx.seq = seq;
            fx.length_ms = float(fm->sequences[std::size_t(seq)].length_ms());
            fx.x = float(h.x.to_double());
            fx.y = float(h.y.to_double());
            fx.z = ground_z_at(fx.x, fx.y);
            if (victim.id < driver_.actors.size()) {
                render::ActorInstance& ta = driver_.actors[victim.id];
                render::Mat4 att;
                const int tseq = ta.model ? ta.current_sequence() : -1;
                if (tseq >= 0 && render::attachment_transform(*ta.model, "Ref_Center", std::uint32_t(tseq), ta.current_time(), att))
                    fx.z += att.m[14];
            }
            if (victim.flying && cat_) fx.z += cat_->unit_display_height(victim.catalog_id);
            fx.player = victim.player;
            if (impact_fx_.size() < 512) impact_fx_.push_back(fx);
        }
        for (const sim::World::LaunchEvent& l : world_.take_launch_events()) {
            if (l.attacker >= world_.units().size()) continue;
            const Unit& shooter = world_.units()[l.attacker];
            const std::string& wid = l.slot == 1 ? shooter.wpn2_id : shooter.wpn_id;
            if (!wid.empty() && l.target < world_.units().size()) {
                const float period_ms = float((l.slot == 1 ? shooter.wpn2_period : shooter.wpn_period).to_double()) * 1000.0f;
                beam_refresh(l.attacker, l.target, l.slot, wid, false, std::max(250.0f, period_ms * 1.5f));
            }
            const formats::Model* fm = models_->launch_model(*ui_store_, wid);
            if (!fm || fm->sequences.empty()) continue;
            int seq = render::select_sequence(*fm, {"Birth"}, l.attacker);
            if (seq < 0) seq = render::select_sequence(*fm, {"Stand"}, l.attacker);
            if (seq < 0) seq = 0;
            ImpactFx fx;
            fx.model = fm;
            fx.seq = seq;
            fx.length_ms = float(fm->sequences[std::size_t(seq)].length_ms());
            fx.x = float(shooter.x.to_double());
            fx.y = float(shooter.y.to_double());
            fx.z = ground_z_at(fx.x, fx.y);
            if (shooter.flying && cat_) fx.z += cat_->unit_display_height(shooter.catalog_id);
            if (shooter.id < driver_.actors.size()) {
                render::ActorInstance& sa = driver_.actors[shooter.id];
                render::Mat4 latt;
                const int sseq = sa.model ? sa.current_sequence() : -1;
                if (sseq >= 0 && (render::attachment_transform(*sa.model, "Ref_Weapon", std::uint32_t(sseq), sa.current_time(), latt) ||
                                  render::attachment_transform(*sa.model, "Ref_Center", std::uint32_t(sseq), sa.current_time(), latt))) {
                    const float c = std::cos(sa.facing + 1.5707963f), s = std::sin(sa.facing + 1.5707963f);
                    fx.x += latt.m[12] * c - latt.m[13] * s;
                    fx.y += latt.m[12] * s + latt.m[13] * c;
                    fx.z += latt.m[14];
                }
                fx.yaw = sa.facing;
            }
            if (l.target < world_.units().size()) {
                const Unit& t = world_.units()[l.target];
                const float tx = float(t.x.to_double()), ty = float(t.y.to_double());
                if (tx != fx.x || ty != fx.y) fx.yaw = std::atan2(ty - fx.y, tx - fx.x);
            }
            fx.player = shooter.player;
            if (impact_fx_.size() < 512) impact_fx_.push_back(fx);
        }
    }
    if (ticked)
        for (const sim::HealerInstance& h : world_.healers()) {
            if (h.vital != sim::HealVital::Life) continue;
            if (h.caster >= world_.units().size() || h.target >= world_.units().size()) continue;
            if (!world_.units()[h.caster].alive || !world_.units()[h.target].alive) continue;
            beam_refresh(h.caster, h.target, 0, "MedivacHealBeam", true, 250.0f);
        }
    static const bool kBatteryBeamOld = std::getenv("OMC_BATTERY_BEAM_OLD") != nullptr;
    if (ticked && !kBatteryBeamOld && cat_)
        for (const sim::HealerInstance& h : world_.healers()) {
            if (h.vital == sim::HealVital::Life) continue;
            if (h.caster >= world_.units().size() || h.target >= world_.units().size()) continue;
            if (!world_.units()[h.caster].alive || !world_.units()[h.target].alive) continue;
            const std::string& type = world_.units()[h.caster].catalog_id;
            auto it = recharge_beam_.find(type);
            if (it == recharge_beam_.end()) {
                std::string beam;
                for (const std::string& ab : cat_->array_links("CUnit", type, "AbilArray")) {
                    if (ab.empty() || !beam.empty()) continue;
                    std::string eff = cat_->scalar("CAbil", ab, "Effect").value_or("");
                    if (eff.empty()) eff = ab;
                    for (const data::Catalog::ActorSubscription& s : cat_->actors_on("Effect", eff, "Start")) {
                        if (s.family != "CActorAction") continue;
                        std::string b = cat_->scalar(s.family, s.actor, "Beam").value_or("");
                        const std::string token = s.actor.substr(0, s.actor.find('@'));
                        const std::size_t at = b.find("##id##");
                        if (at != std::string::npos) b.replace(at, 6, token);
                        if (!b.empty()) { beam = b; break; }
                    }
                }
                it = recharge_beam_.emplace(type, beam).first;
            }
            if (!it->second.empty()) beam_refresh(h.caster, h.target, 1, it->second, true, 250.0f);
        }
    if (!beams_.empty()) {
        for (BeamFx& b : beams_) {
            b.time_ms += dt_s * 1000.0f;
            b.idle_ms += dt_s * 1000.0f;
            const auto len = [&](int ph) {
                const int sq = b.seq[ph];
                return (b.model && sq >= 0 && std::size_t(sq) < b.model->sequences.size())
                           ? float(b.model->sequences[std::size_t(sq)].length_ms()) : 0.0f;
            };
            const bool lost = b.attacker >= world_.units().size() || !world_.units()[b.attacker].alive ||
                              b.target >= world_.units().size() || !world_.units()[b.target].alive;
            if (b.phase == 0 && b.time_ms >= len(0)) { b.phase = 1; b.time_ms = 0.0f; }
            if (b.phase == 1) {
                const bool close = b.continuous ? (lost || b.idle_ms > b.keep_ms) : (b.time_ms >= len(1));
                if (close) { b.phase = 2; b.time_ms = 0.0f; }
                else if (b.continuous && len(1) > 0.0f && b.time_ms >= len(1)) b.time_ms = std::fmod(b.time_ms, len(1));
            }
        }
        beams_.erase(std::remove_if(beams_.begin(), beams_.end(), [](const BeamFx& b) {
                         if (b.phase < 2) return false;
                         const int sq = b.seq[2];
                         const float l = (b.model && sq >= 0 && std::size_t(sq) < b.model->sequences.size())
                                             ? float(b.model->sequences[std::size_t(sq)].length_ms()) : 0.0f;
                         return b.time_ms >= l;
                     }),
                     beams_.end());
    }
    if (!effect_doodads_.empty()) {
        for (EffectDoodad& d : effect_doodads_) {
            d.time_ms += dt_s * 1000.0f;
            const int sq = d.seq[d.phase];
            const float len = (d.model && sq >= 0) ? float(d.model->sequences[std::size_t(sq)].length_ms()) : 0.0f;
            if (d.phase == 0 && d.time_ms >= len) { d.phase = 1; d.time_ms = 0.0f; }
            else if (d.phase == 1 && len > 0.0f && d.time_ms >= len) d.time_ms = std::fmod(d.time_ms, len);
            else if (d.phase == 2 && (sq < 0 || d.time_ms >= len)) d.phase = 3;
        }
        effect_doodads_.erase(std::remove_if(effect_doodads_.begin(), effect_doodads_.end(),
                                             [](const EffectDoodad& d) { return d.phase >= 3; }),
                              effect_doodads_.end());
    }
    if (!fx_host_.bound() && cat_ && models_ && ui_store_)
        fx_host_.bind(
            cat_, [this](const std::string& id) { return models_->cmodel(*ui_store_, id); },
            [this](const std::string& sound) {
                last_effect_sound_ = sound;
                if (audio_) audio_->play_sound(sound, 1.0f);
            });
    fx_host_.observe(world_, dt_s * 1000.0f, ticked);
    if (!impact_fx_.empty()) {
        for (ImpactFx& f : impact_fx_) f.time_ms += dt_s * 1000.0f;
        impact_fx_.erase(std::remove_if(impact_fx_.begin(), impact_fx_.end(),
                                        [](const ImpactFx& f) { return f.length_ms <= 0.0f || f.time_ms >= f.length_ms; }),
                         impact_fx_.end());
    }
    if (!corpses_.empty()) {
        static const bool kDeadOld = std::getenv("OMC_CORPSE_DEAD_OLD") != nullptr;
        const float dt_ms = dt_s * 1000.0f;
        std::size_t keep = 0;
        for (std::size_t i = 0; i < corpses_.size(); ++i)
            if (advance_corpse(corpses_[i], dt_ms, !kDeadOld)) {
                if (keep != i) corpses_[keep] = corpses_[i];
                ++keep;
            }
        corpses_.resize(keep);
    }
    if (layout_.players.size() > 2 && !MatchResult::sole_player_rule()) world_.refresh_defeats();
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
    step_lap.lap("st.post");
    emit_scene(scene);
    if (!mem_step_marked_ && render::mem_lap_marks()) {
        render::mem_mark("first step: emit_scene (actors, world models)");
        render::mem_lap_marks() = false;
        mem_step_marked_ = true;
    }
    cam = camctl_.camera();
    static const bool esc_ends = std::getenv("OMC_ESC_FRAME_ENDS_MATCH") != nullptr;
    return esc_ends ? !in.quit : true;
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

std::vector<std::uint8_t> MeleeGame::portrait_call(const std::string& pid, int mode) {
    if (!portrait_renderer_ || !models_ || !ui_store_) return {1};
    const MeleeModels::Portrait* p = models_->portrait(*ui_store_, pid);
    if (!p) return {1};
    const std::uint32_t pw = last_portrait_w_ ? last_portrait_w_ : 1, ph = last_portrait_h_ ? last_portrait_h_ : 1;
    std::vector<std::uint8_t> img;
    portrait_load_mode() = mode;
    try { img = portrait_renderer_(*ui_store_, p->model, p->fov_rad, p->cam_bone, p->znear, p->zfar, pw, ph, p->light.found ? &p->light : nullptr); } catch (...) {}
    portrait_load_mode() = 0;
    return img;
}

void MeleeGame::warm_selection_assets(std::uint32_t view_w, std::uint32_t view_h) {
    if (!selection_warm_ || warming_ || cfg_.human_player < 0 || !ui_store_ || !cat_ || replay_drive_) return;
    std::vector<std::uint32_t> picks;
    const bool later = portrait_async_ && warm_started_;
    if (warm_seen_.size() < world_.units().size()) warm_seen_.resize(world_.units().size(), 0);
    for (const Unit& u : world_.units()) {
        if (warm_seen_[u.id]) continue;
        if (u.player < 0) { warm_seen_[u.id] = 1; continue; }
        if (!u.alive || u.in_gas_structure || u.catalog_id.empty()) continue;
        if (u.player != cfg_.human_player) {
            if (!warm_foreign_) continue;
            if (!reveal_map_ && vision_.width() > 0 &&
                !hud_fog_body_drawn(fog_body_state(u.player, int(vision_.state_at(u.x, u.y)), fog_hidden_black_), FogVisibility(u.fog_visibility)))
                continue;
        }
        warm_seen_[u.id] = 1;
        if (warmed_types_.count(u.catalog_id)) continue;
        warmed_types_.insert(u.catalog_id);
        if (later) {
            OMC_PROF_SCOPE("ov.warm_request");
            if (!models_ || models_->prefetch_portrait(*ui_store_, u.catalog_id)) portrait_call(u.catalog_id, 2);
            warm_pending_.push_back({u.id, u.catalog_id});
            render::frame_prof_count("warm.requested", 1.0);
        } else {
            picks.push_back(u.id);
        }
    }
    if (!world_.units().empty()) warm_started_ = true;
    if (picks.empty() && later && !warm_pending_.empty()) {
        for (std::size_t k = 0; k < warm_pending_.size() && k < 4; ++k) {
            const PendingWarm pw = warm_pending_[k];
            if (pw.id >= world_.units().size() || !world_.unit(pw.id).alive) {
                warm_pending_.erase(warm_pending_.begin() + std::ptrdiff_t(k));
                break;
            }
            if (models_ && !models_->prefetch_portrait(*ui_store_, pw.type)) continue;
            if (portrait_call(pw.type, 2).size() != 1) break;
            warm_pending_.erase(warm_pending_.begin() + std::ptrdiff_t(k));
            picks.push_back(pw.id);
            break;
        }
        render::frame_prof_count("warm.pending", double(warm_pending_.size()));
    }
    if (picks.empty()) return;
    OMC_PROF_SCOPE("ov.warm");
    render::frame_prof_count("warm.types", double(picks.size()));
    if (portrait_async_ && !later)
        for (std::uint32_t id : picks) portrait_call(world_.unit(id).catalog_id, 2);
    const std::vector<std::uint32_t> saved_sel = selection_;
    const SelectionGroups saved_groups = sel_groups_;
    const std::vector<std::uint32_t> saved_members = sel_groups_members_;
    const int saved_page = group_page_;
    const auto saved_hover = hover_unit_;
    const int saved_trail_tick = status_trail_tick_;
    const std::vector<float> saved_trails[4] = {status_trail_life_, status_trail_shield_, status_rate_life_, status_rate_shield_};
    warming_ = true;
    std::vector<std::uint8_t> scratch;
    try {
        for (std::uint32_t id : picks) {
            selection_ = {id};
            build_overlay(view_w, view_h, scratch);
        }
        if (picks.size() >= 2) {
            selection_ = picks;
            build_overlay(view_w, view_h, scratch);
        }
    } catch (...) {
    }
    warming_ = false;
    selection_ = saved_sel;
    sel_groups_ = saved_groups;
    sel_groups_members_ = saved_members;
    group_page_ = saved_page;
    hover_unit_ = saved_hover;
    status_trail_tick_ = saved_trail_tick;
    status_trail_life_ = saved_trails[0];
    status_trail_shield_ = saved_trails[1];
    status_rate_life_ = saved_trails[2];
    status_rate_shield_ = saved_trails[3];
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

bool MeleeGame::cloak_enemy_drawn(const sim::Unit& u) const {
    static const bool kOld = std::getenv("OMC_CLOAK_ENEMY_OLD") != nullptr;
    if (kOld || !cat_ || u.burrowed || !u.cloaked_effective()) return false;
    if (cloak_enemy_look_ < 0) {
        const auto enemy = cat_->cloak_transition("None", "Enemy");
        const auto detected = cat_->cloak_transition("None", "Detected");
        if (!enemy || enemy->loop.empty() || cat_->actor_cloak_model().empty())
            cloak_enemy_look_ = 0;
        else
            cloak_enemy_look_ = (!detected || detected->loop != enemy->loop) ? 1 : 2;
    }
    return cloak_enemy_look_ > 0;
}

void MeleeGame::bind_cloak_look() {
    if (cloak_look_bound_ == 1 || cloak_look_bound_ == -1 || !cat_) return;
    if (cloak_look_bound_ == 0) {
        static const bool kEnterOld = std::getenv("OMC_CLOAK_TRANSITION_OLD") != nullptr;
        const data::Catalog* cat = cat_;
        cloak_tx_.bind(
            &cloak_look_,
            [cat](render::CloakView from, render::CloakView to, std::string& enter, std::string& loop) {
                const auto t = cat->cloak_transition(render::cloak_view_name(from), render::cloak_view_name(to));
                if (!t) return false;
                enter = t->enter;
                loop = t->loop;
                return true;
            },
            !kEnterOld);
        cloak_look_bound_ = 2;
    }
    if (!models_ || !ui_store_) return;
    cloak_look_bound_ = -1;
    const std::string id = cat_->actor_cloak_model();
    const formats::Model* m = id.empty() ? nullptr : models_->cmodel(*ui_store_, id);
    casc::Storage* store = ui_store_;
    auto load_mask = [store](const std::string& path) {
        std::vector<float> hist;
        static const char* kRoots[] = {"mods/core.sc2mod/base.sc2assets/", "mods/liberty.sc2mod/base.sc2assets/",
                                       "mods/swarm.sc2mod/base.sc2assets/", "mods/void.sc2mod/base.sc2assets/", ""};
        for (const char* root : kRoots) {
            try {
                const std::vector<std::uint8_t> bytes = store->read_by_name(std::string(root) + path);
                formats::DdsInfo info;
                const std::vector<std::uint8_t> rgba = formats::dds_decode_rgba(bytes, info);
                if (rgba.size() < 4 || !info.width || !info.height) continue;
                const std::size_t n = rgba.size() / 4;
                std::uint8_t lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
                for (std::size_t i = 0; i < n; ++i)
                    for (int k = 0; k < 4; ++k) {
                        lo[k] = std::min(lo[k], rgba[i * 4 + std::size_t(k)]);
                        hi[k] = std::max(hi[k], rgba[i * 4 + std::size_t(k)]);
                    }
                const int ch = (lo[0] == hi[0] && lo[3] != hi[3]) ? 3 : 0;
                hist.assign(256, 0.0f);
                for (std::size_t i = 0; i < n; ++i) hist[rgba[i * 4 + std::size_t(ch)]] += 1.0f;
                for (float& h : hist) h /= float(n);
                break;
            } catch (...) {
            }
        }
        return hist;
    };
    if (m && cloak_look_.bind(m, load_mask)) cloak_look_bound_ = 1;
}

void MeleeGame::refresh_creep_seen() {
    static const bool kOld = std::getenv("OMC_CREEP_FOG_OLD") != nullptr;
    const sim::CreepGrid& live = world_.creep().grid();
    creep_fogged_ = !kOld && ground_fog_ && !reveal_map_ && cfg_.human_player >= 0 && vision_.width() > 0 && live.width() > 0 &&
                    live.height() > 0;
    if (!creep_fogged_) {
        creep_seen_tick_ = -1;
        return;
    }
    const std::uint64_t dims = (std::uint64_t(std::uint32_t(live.width())) << 48) ^ (std::uint64_t(std::uint32_t(live.height())) << 32) ^
                               (std::uint64_t(std::uint32_t(vision_.width())) << 16) ^ std::uint64_t(std::uint32_t(vision_.height()));
    const int w = live.width(), h = live.height();
    if (dims != creep_seen_dims_ || creep_seen_.width() != w || creep_seen_.height() != h) {
        creep_seen_dims_ = dims;
        creep_seen_.configure(w, h, live.cell_size(), live.origin_x(), live.origin_y());
        creep_vis_index_.assign(std::size_t(w) * std::size_t(h), -1);
        const sim::Fixed half = live.cell_size() / sim::Fixed::from_int(2);
        for (int cy = 0; cy < h; ++cy)
            for (int cx = 0; cx < w; ++cx) {
                const sim::Fixed wx = live.origin_x() + live.cell_size() * sim::Fixed::from_int(cx) + half;
                const sim::Fixed wy = live.origin_y() + live.cell_size() * sim::Fixed::from_int(cy) + half;
                int vx = -1, vy = -1;
                vision_.to_cell(wx, wy, vx, vy);
                if (vx < 0 || vy < 0 || vx >= vision_.width() || vy >= vision_.height() || vx > 0xFFFF || vy > 0x7FFF) continue;
                creep_vis_index_[std::size_t(cy) * std::size_t(w) + std::size_t(cx)] = std::int32_t(vx | (vy << 16));
            }
        creep_seen_tick_ = -1;
        creep_seen_dirty_ = true;
    }
    if (creep_seen_tick_ == std::int64_t(world_.tick())) return;
    creep_seen_tick_ = std::int64_t(world_.tick());
    const std::vector<std::uint8_t>& lc = live.cells();
    std::vector<std::uint8_t>& sc = creep_seen_.cells_mut();
    bool changed = false;
    for (std::size_t i = 0; i < creep_vis_index_.size(); ++i) {
        const std::int32_t vi = creep_vis_index_[i];
        if (vi < 0) continue;
        if (vision_.state(vi & 0xFFFF, vi >> 16) != sim::Visibility::Visible) continue;
        const std::uint8_t v = lc[i] ? 1 : 0;
        if (sc[i] != v) {
            sc[i] = v;
            changed = true;
        }
    }
    if (changed) creep_seen_dirty_ = true;
}

void MeleeGame::minimap_fog_content(HudContent& content) {
    if (cfg_.human_player < 0) return;
    static const bool kCloakOld = std::getenv("OMC_MINIMAP_CLOAK_OLD") != nullptr;
    if (!kCloakOld && !reveal_map_) {
        content.minimap_hidden.assign(world_.units().size(), 0);
        for (const Unit& u : world_.units()) {
            if (!u.alive || u.player < 0 || u.player == cfg_.human_player || world_.are_allied(u.player, cfg_.human_player)) continue;
            if (!u.cloaked_effective() && !u.burrowed) continue;
            if (world_.revealed_to(cfg_.human_player, u.id)) continue;
            content.minimap_hidden[u.id] = 1;
        }
    }
    for (const auto& kv : fog_snaps_) {
        HudContent::MinimapSnapshot s;
        s.unit = kv.first;
        s.player = kv.second.player;
        s.x = kv.second.x;
        s.y = kv.second.y;
        s.radius = kv.second.minimap_radius;
        content.minimap_snapshots.push_back(s);
    }
    static const bool kCreepOld = std::getenv("OMC_MINIMAP_CREEP_OLD") != nullptr;
    if (kCreepOld) return;
    if (!creep_minimap_rgb_set_ && ui_store_ && !creep_diffuse_.empty()) {
        creep_minimap_rgb_set_ = true;
        static const char* kRoots[] = {"mods/core.sc2mod/base.sc2assets/", "mods/liberty.sc2mod/base.sc2assets/",
                                       "mods/swarm.sc2mod/base.sc2assets/", "mods/void.sc2mod/base.sc2assets/", ""};
        for (const char* root : kRoots) {
            try {
                const std::vector<std::uint8_t> bytes = ui_store_->read_by_name(std::string(root) + creep_diffuse_);
                formats::DdsInfo info;
                const std::vector<std::uint8_t> rgba = formats::dds_decode_rgba(bytes, info);
                if (rgba.size() < 4 || !info.width || !info.height) continue;
                double acc[3] = {0.0, 0.0, 0.0};
                const std::size_t n = rgba.size() / 4;
                for (std::size_t i = 0; i < n; ++i)
                    for (int k = 0; k < 3; ++k) acc[k] += rgba[i * 4 + std::size_t(k)];
                for (int k = 0; k < 3; ++k) creep_minimap_rgb_[k] = std::uint8_t(std::min(255.0, acc[k] / double(n) + 0.5));
                break;
            } catch (...) {
            }
        }
    }
    if (creep_minimap_rgb_[0] == 0 && creep_minimap_rgb_[1] == 0 && creep_minimap_rgb_[2] == 0) return;
    const sim::CreepGrid& g = creep_fogged_ ? creep_seen_ : world_.creep().grid();
    if (g.width() <= 0 || g.height() <= 0) return;
    content.minimap_creep = &g.cells();
    content.creep_w = g.width();
    content.creep_h = g.height();
    content.creep_x0 = float(g.origin_x().to_double());
    content.creep_y0 = float(g.origin_y().to_double());
    content.creep_cell = float(g.cell_size().to_double());
    for (int k = 0; k < 3; ++k) content.creep_rgb[k] = creep_minimap_rgb_[k];
    content.creep_alpha = 1.0f;
}

void MeleeGame::update_minimap_fog(int viewed) {
    if (map_w_ <= 0.0f || map_h_ <= 0.0f) { minimap_fog_.clear(); minimap_fw_ = minimap_fh_ = 0; return; }
    const sim::Bounds bounds{sim::Fixed(), sim::Fixed(),
                             sim::Fixed::from_double(map_w_), sim::Fixed::from_double(map_h_)};
    if (fog_blend_speed_ < 0.0f) {
        fog_blend_speed_ = 2.0f;
        fog_hidden_black_ = false;
        fog_veil_alpha_ = 165;
        if (cat_) {
            if (auto v = cat_->tag_default_scalar("CFoW", "BlendSpeed"); v && !v->empty())
                fog_blend_speed_ = float(std::atof(v->c_str()));
            if (auto v = cat_->scalar("CFoW", fog_type_, "BlendSpeed"); v && !v->empty())
                fog_blend_speed_ = float(std::atof(v->c_str()));
            if (auto v = cat_->scalar("CFoW", fog_type_, "Color"); v && !v->empty()) {
                const int a = std::atoi(v->c_str());
                fog_veil_alpha_ = std::uint8_t(a < 0 ? 0 : (a > 255 ? 255 : a));
            }
            if (auto v = cat_->scalar("CFoW", fog_type_, "Hidden"); v && !v->empty())
                fog_hidden_black_ = std::atoi(v->c_str()) != 0;
        }
    }
    vision_.compute(world_, viewed,   sim::Fixed::from_int(10),
                      sim::Fixed::from_int(1), bounds,
                    world_.placement().configured() ? &world_.placement() : nullptr);
    const int fw = vision_.width(), fh = vision_.height();
    if (fw <= 0 || fh <= 0) { minimap_fog_.clear(); minimap_fw_ = minimap_fh_ = 0; return; }
    minimap_fw_ = fw;
    minimap_fh_ = fh;
    minimap_fog_.assign(std::size_t(fw) * std::size_t(fh), 0);
    for (int cy = 0; cy < fh; ++cy)
        for (int cx = 0; cx < fw; ++cx)
            minimap_fog_[std::size_t(cy) * std::size_t(fw) + std::size_t(cx)] =
                reveal_map_ ? std::uint8_t(sim::Visibility::Visible) : std::uint8_t(fog_ground_state(int(vision_.state(cx, cy)), fog_hidden_black_));
    const int S = 2;
    const int W = fw * S, H = fh * S;
    ground_fog_field_.w = std::uint32_t(W);
    ground_fog_field_.h = std::uint32_t(H);
    ground_fog_field_.world_w = map_w_;
    ground_fog_field_.world_h = map_h_;
    const bool snap = fog_blend_.size() != minimap_fog_.size() || fog_blend_speed_ <= 0.0f || fog_dt_s_ <= 0.0f;
    if (snap) fog_blend_.assign(minimap_fog_.size(), 0.0f);
    const float max_step = 255.0f * fog_blend_speed_ * fog_dt_s_;
    static std::vector<std::uint8_t> cell_a, prev_cell_a;
    static const MeleeGame* prev_game = nullptr;
    static int prev_w = 0, prev_h = 0;
    static float prev_key[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    cell_a.resize(minimap_fog_.size());
    for (std::size_t i = 0; i < minimap_fog_.size(); ++i) {
        const int st = int(minimap_fog_[i]);
        const float target = st == int(sim::Visibility::Explored) ? float(fog_veil_alpha_) : float(hud_fog_veil_alpha(st));
        float& cur = fog_blend_[i];
        if (snap) cur = target;
        else if (cur < target) cur = std::min(target, cur + max_step);
        else if (cur > target) cur = std::max(target, cur - max_step);
        cell_a[i] = std::uint8_t(cur + 0.5f);
    }
    const bool extra_veils = OMC_ENV_ONCE("OMC_PLAYABLE_VEIL") || OMC_ENV_ONCE("OMC_PIT_VEIL") || OMC_ENV_ONCE("OMC_LEVEL_VEIL") ||
                             OMC_ENV_ONCE("OMC_PLAYABLE_RAMP") || OMC_ENV_ONCE("OMC_FOG_FIELD_UNCACHED");
    const float field_key[6] = {map_w_, map_h_, playable_x0_, playable_y0_, playable_x1_, playable_y1_};
    if (!extra_veils && prev_game == this && prev_w == W && prev_h == H && ground_fog_field_.alpha.size() == std::size_t(W) * std::size_t(H) &&
        std::equal(field_key, field_key + 6, prev_key) && prev_cell_a == cell_a)
        return;
    prev_game = nullptr;
    ground_fog_field_.alpha.resize(std::size_t(W) * std::size_t(H));
    for (int cy = 0; cy < fh; ++cy)
        for (int cx = 0; cx < fw; ++cx) {
            const std::uint8_t a = cell_a[std::size_t(cy) * std::size_t(fw) + std::size_t(cx)];
            for (int sy = 0; sy < S; ++sy) {
                std::uint8_t* row = &ground_fog_field_.alpha[std::size_t(cy * S + sy) * std::size_t(W) + std::size_t(cx * S)];
                for (int sx = 0; sx < S; ++sx) row[sx] = a;
            }
        }
    static const bool kRimBlurred = std::getenv("OMC_BOUNDS_VEIL_BLURRED") != nullptr;
    const auto lay_rim = [&]() {
        if (map_w_ > 0.0f && map_h_ > 0.0f && !OMC_ENV_ONCE("OMC_NO_BOUNDS_VEIL")) {
            const float cw = map_w_ / float(W), ch = map_h_ / float(H);
            static const float kInset = std::getenv("OMC_BOUNDS_VEIL_INSET") ? float(std::atof(std::getenv("OMC_BOUNDS_VEIL_INSET"))) : 1.5f;
            static bool traced = false;
            if (!traced && OMC_ENV_ONCE("OMC_BOUNDS_VEIL_TRACE")) {
                traced = true;
                std::fprintf(stderr, "[bounds] map %.1f x %.1f playable %.1f %.1f %.1f %.1f inset %.1f\n", double(map_w_), double(map_h_), double(playable_x0_), double(playable_y0_),
                             double(playable_x1_), double(playable_y1_), double(kInset));
            }
            static const char* kRimPlay = std::getenv("OMC_RIM_PLAYABLE");
            float rx0 = kInset, ry0 = kInset, rx1 = map_w_ - kInset, ry1 = map_h_ - kInset;
            if (kRimPlay && playable_x1_ > playable_x0_ && playable_y1_ > playable_y0_) {
                const float e = float(std::atof(kRimPlay));
                rx0 = playable_x0_ - e; ry0 = playable_y0_ - e; rx1 = playable_x1_ + e; ry1 = playable_y1_ + e;
            }
            static std::vector<std::uint8_t> out_x;
            out_x.resize(std::size_t(W));
            for (int x = 0; x < W; ++x) {
                const float wx = (float(x) + 0.5f) * cw;
                out_x[std::size_t(x)] = (wx < rx0 || wx > rx1) ? 255 : 0;
            }
            for (int y = 0; y < H; ++y) {
                const float wy = (float(y) + 0.5f) * ch;
                std::uint8_t* row = &ground_fog_field_.alpha[std::size_t(y) * std::size_t(W)];
                if (wy < ry0 || wy > ry1) {
                    std::memset(row, 255, std::size_t(W));
                    continue;
                }
                const std::uint8_t* mask = out_x.data();
                for (int x = 0; x < W; ++x) row[x] = std::uint8_t(row[x] | mask[x]);
            }
        }
    };
    if (kRimBlurred) lay_rim();
    if (const char* pv = OMC_ENV_ONCE("OMC_PLAYABLE_VEIL"); pv && playable_x1_ > playable_x0_ && playable_y1_ > playable_y0_ && map_w_ > 0.0f && map_h_ > 0.0f) {
        const int pa = std::min(255, std::max(0, std::atoi(pv)));
        const float cw = map_w_ / float(W), ch = map_h_ / float(H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float wx = (float(x) + 0.5f) * cw, wy = (float(y) + 0.5f) * ch;
                if (wx < playable_x0_ || wx > playable_x1_ || wy < playable_y0_ || wy > playable_y1_) {
                    static const char* kLow = std::getenv("OMC_PLAYABLE_VEIL_LOW");
                    if (kLow && ground_z_at(wx, wy) >= float(std::atof(kLow))) continue;
                    std::uint8_t& a = ground_fog_field_.alpha[std::size_t(y) * std::size_t(W) + std::size_t(x)];
                    a = std::uint8_t(std::max(int(a), pa));
                }
            }
    }
    if (const char* pt = OMC_ENV_ONCE("OMC_PIT_VEIL"); pt && world_.nav().configured() && world_.nav().has_levels() && playable_x1_ > playable_x0_ &&
                                                      playable_y1_ > playable_y0_ && map_w_ > 0.0f && map_h_ > 0.0f) {
        const int pa = std::min(255, std::max(0, std::atoi(pt)));
        const sim::NavGrid& nv = world_.nav();
        const float ncell = float(nv.cell_size().to_double());
        int min_in = 255;
        for (int cy = 0; cy < nv.height(); ++cy)
            for (int cx = 0; cx < nv.width(); ++cx) {
                const float wx = (float(cx) + 0.5f) * ncell, wy = (float(cy) + 0.5f) * ncell;
                if (wx > playable_x0_ && wx < playable_x1_ && wy > playable_y0_ && wy < playable_y1_) min_in = std::min(min_in, int(nv.level_cell(cx, cy)));
            }
        const float cw = map_w_ / float(W), ch = map_h_ / float(H);
        std::size_t veiled = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float wx = (float(x) + 0.5f) * cw, wy = (float(y) + 0.5f) * ch;
                if (wx >= playable_x0_ && wx <= playable_x1_ && wy >= playable_y0_ && wy <= playable_y1_) continue;
                if (ncell <= 0.0f || int(nv.level_cell(int(wx / ncell), int(wy / ncell))) >= min_in) continue;
                std::uint8_t& a = ground_fog_field_.alpha[std::size_t(y) * std::size_t(W) + std::size_t(x)];
                a = std::uint8_t(std::max(int(a), pa));
                ++veiled;
            }
        static bool told = false;
        if (!told && std::getenv("OMC_BOUNDS_VEIL_TRACE")) {
            told = true;
            std::fprintf(stderr, "[pit] lowest level inside the playable rectangle %d, %zu field cells veiled\n", min_in, veiled);
        }
    }
    if (const char* lv = OMC_ENV_ONCE("OMC_LEVEL_VEIL"); lv && world_.nav().configured() && map_w_ > 0.0f && map_h_ > 0.0f) {
        int lvl = 0, pa = 0;
        if (std::sscanf(lv, "%d:%d", &lvl, &pa) == 2) {
            pa = std::min(255, std::max(0, pa));
            const sim::NavGrid& nv = world_.nav();
            const float ncell = float(nv.cell_size().to_double());
            const float cw = map_w_ / float(W), ch = map_h_ / float(H);
            std::size_t veiled = 0;
            int hist[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const float wx = (float(x) + 0.5f) * cw, wy = (float(y) + 0.5f) * ch;
                    const int L = ncell > 0.0f ? int(nv.level_cell(int(wx / ncell), int(wy / ncell))) : 0;
                    ++hist[std::min(L, 7)];
                    if (L > lvl) continue;
                    std::uint8_t& a = ground_fog_field_.alpha[std::size_t(y) * std::size_t(W) + std::size_t(x)];
                    a = std::uint8_t(std::max(int(a), pa));
                    ++veiled;
                }
            static bool told = false;
            if (!told && std::getenv("OMC_BOUNDS_VEIL_TRACE")) {
                told = true;
                std::fprintf(stderr, "[level] cells by level %d %d %d %d %d %d %d %d, %zu veiled\n", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], veiled);
            }
        }
    }
    if (const char* pr = OMC_ENV_ONCE("OMC_PLAYABLE_RAMP"); pr && playable_x1_ > playable_x0_ && playable_y1_ > playable_y0_ && map_w_ > 0.0f && map_h_ > 0.0f) {
        float d0 = 0.0f, rr = 1.0f;
        if (std::sscanf(pr, "%f:%f", &d0, &rr) == 2 && rr > 0.0f) {
            const float cw = map_w_ / float(W), ch = map_h_ / float(H);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const float wx = (float(x) + 0.5f) * cw, wy = (float(y) + 0.5f) * ch;
                    const float dx = std::max(std::max(playable_x0_ - wx, wx - playable_x1_), 0.0f);
                    const float dy = std::max(std::max(playable_y0_ - wy, wy - playable_y1_), 0.0f);
                    const float d = std::max(dx, dy);
                    const int pa = int(255.0f * std::min(1.0f, std::max(0.0f, (d - d0) / rr)) + 0.5f);
                    std::uint8_t& a = ground_fog_field_.alpha[std::size_t(y) * std::size_t(W) + std::size_t(x)];
                    a = std::uint8_t(std::max(int(a), pa));
                }
        }
    }
    {
        static std::vector<std::uint8_t> tmp;
        tmp.resize(ground_fog_field_.alpha.size());
        fog_box_pass<2>(ground_fog_field_.alpha.data(), tmp.data(), W, H);
        fog_box_pass<1>(ground_fog_field_.alpha.data(), tmp.data(), W, H);
    }
    if (!kRimBlurred) lay_rim();
    prev_cell_a = cell_a;
    prev_game = this;
    prev_w = W;
    prev_h = H;
    std::copy(field_key, field_key + 6, prev_key);
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

const render::OverlayFont* MeleeGame::style_face(const std::string& font_path) {
    if (font_path.empty() || !ui_store_ || OMC_ENV_ONCE("OMC_NO_HEADER_FACE")) return nullptr;
    auto it = style_faces_.find(font_path);
    if (it == style_faces_.end()) {
        it = style_faces_.emplace(font_path, render::OverlayFont{}).first;
        std::string p = font_path;
        for (char& ch : p)
            if (ch == '\\') ch = '/';
        try {
            auto bytes = ui_store_->read_by_name("mods/core.sc2mod/base.sc2data/" + p);
            it->second.load(bytes);
        } catch (const std::exception&) {   }
        if (std::getenv("OMC_LABEL_FACE_TRACE"))
            std::fprintf(stderr, "[labelface] %s loaded %d\n", p.c_str(), int(it->second.valid()));
    }
    return it->second.valid() ? &it->second : nullptr;
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

void MeleeGame::ensure_console_texture(std::uint32_t view_w, std::uint32_t view_h) {
    const bool resized = console_renderer_ && cat_ && view_w && view_h && console_view_w_ && console_view_h_ &&
                         (view_w != console_view_w_ || view_h != console_view_h_);
    if (console_tried_ && !resized) return;
    if (console_race_assets() && cfg_.human_player >= 0 && hall_race() < 0 && world_.tick() < sim::World::kTicksPerSecond) return;
    console_tried_ = true;
    if (!ui_store_) return;
    if (console_renderer_ && cat_) {
        console_tex_ = console_renderer_(*ui_store_, *cat_,
                                         console_skin_id_for_race(human_race()), view_w ? view_w : kConsoleFrameWidth, view_h);
        render::mem_mark("hud: console picture (offscreen render of the console models)");
        if (console_tex_.ok()) {
            console_view_w_ = view_w;
            console_view_h_ = view_h;
            return;
        }
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
    const bool first = hud_authored_ == nullptr;
    hud_authored_ = hud_authored_rects(*ui_store_, float(view_w) / float(view_h));
    if (first && hud_authored_) render::mem_mark("hud: layout set + GameUI solve");
}

void MeleeGame::drain_train_events() {
    if (train_events_.empty()) return;
    for (const TrainEv& ev : train_events_) {
        const std::uint32_t host = ev.egg != sim::kNoTarget ? ev.egg : ev.producer;
        if (host >= driver_.actors.size()) continue;
        std::string abil;
        if (!ev.produced.empty()) {
            const auto& mk = production_.producers_of(ev.produced);
            for (const data::ProductInfo& pi : mk)
                if (!pi.ability.empty()) { abil = pi.ability; break; }
        }
        if (abil.empty()) continue;
        render::frame_prof_count("abiltrain.raised", 1.0);
        driver_.actors[host].raise("AbilTrain", abil, ev.finish ? "Finish" : "Cancel",
                                   {{"AbilTrainProduced", ev.produced}});
    }
    train_events_.clear();
}

void MeleeGame::build_overlay(std::uint32_t view_w, std::uint32_t view_h,
                              std::vector<std::uint8_t>& out) {
    OMC_PROF_SCOPE("build_overlay");
    out.clear();
    if (!view_w || !view_h) return;
    if (!warming_ && !mem_marked_) render::mem_mark("first frame: step (sim, scene, world models)");
    if (!warming_) warm_selection_assets(view_w, view_h);
    if (!warming_ && !mem_marked_) {
        render::mem_mark("first overlay: selection warm-up (portraits, panes)");
        mem_marked_ = true;
        render::mem_lap_marks() = render::mem_mark_fn() != nullptr;
    }
    if (!warming_ && portrait_async_ && models_ && ui_store_ && (++prefetch_tick_ % 15) == 1) {
        std::unordered_map<std::string, int> queued;
        for (std::int32_t pl = 0; pl < 16; ++pl) world_.pending_by_type(pl, queued);
        for (const auto& kv : queued)
            if (prefetched_types_.insert(kv.first).second) models_->prefetch(*ui_store_, kv.first);
    }
    hud_unit_pane_ = HudUnitPane{};
    hud_harvest_labels_.clear();
    hud_equipment_.clear();
    hud_portrait_ = HudPortrait{};
    ensure_overlay_font();
    const int viewed = cfg_.human_player >= 0 ? cfg_.human_player : 0;

    const float aspect = float(view_w) / float(view_h);

    render::FrameProfLap ov_lap;
    rebuild_subgroups();
    ov_lap.lap("ov.subgroups");
    if (!warming_) render::frame_prof_count("sel.size", double(selection_.size()));

    HudContent content = HudModel::build(world_, viewed, subgroup_ordered_selection());
    ov_lap.lap("ov.model");
    if (cat_ && content.lead_unit < world_.units().size()) {
        switch (cat_->unit_race_index(world_.units()[content.lead_unit].catalog_id)) {
            case 1: content.lead_wireframe.race = WireRace::Protoss; break;
            case 2: content.lead_wireframe.race = WireRace::Zerg; break;
            case 0: content.lead_wireframe.race = WireRace::Terran; break;
            default: break;
        }
    }
    if (cfg_.human_player >= 0) {
        if (MatchResult::banner(outcome_)[0] != '\0') content.selection_summary = MatchResult::banner_text(outcome_, cat_ ? &cat_->strings() : nullptr);
    }
    if (selection_.size() == 1 && selection_[0] < world_.units().size()) {
        const Unit& s = world_.unit(selection_[0]);
        if (s.player < 0) {
            const bool gas = selection_[0] < layout_.unit_type.size() &&
                             layout_.unit_type[selection_[0]] == "VespeneGeyser";
            static const bool kResOld = std::getenv("OMC_RESULT_TEXT_OLD") != nullptr;
            std::string label = gas ? "Vespene" : "Minerals";
            if (!kResOld && cat_)
                if (const std::string* v = cat_->strings().find(gas ? "UI/Gas" : "UI/Minerals"); v && !v->empty()) label = *v;
            content.selection_summary = label + ": " + std::to_string(s.resource_left);
        }
    }
    const bool authored_alert_panel = cfg_.human_player >= 0 && ui_store_ && cat_ && hud_authored_ && hud_authored_->alert.ok();
    content.game_seconds = world_.tick() / sim::World::kTicksPerSecond;
    content.idle_workers = idle_worker_count();
    content.army_units = army_unit_count();
    content.warp_gates_ready = ready_warp_gate_count();
    if (const std::uint32_t lead = cast_lead(); lead < world_.units().size()) {
        const Unit& lu = world_.unit(lead);
        content.equipment = equipment_for(lu.player, &lu);
        if (selection_.size() == 1 && !pane_queue(lead).empty()) content.equipment.clear();
        if (selection_.size() > 1 && !std::getenv("OMC_NO_GROUP_PORTRAIT")) content.equipment.clear();
        hud_equipment_ = content.equipment;
        content.lead_kills = lu.kills;
        if (std::getenv("OMC_PORTRAIT_TRACE"))
            std::fprintf(stderr, "equipment: lead %s cells %zu%s%s rect ok %d\n", lu.catalog_id.c_str(), content.equipment.size(),
                         content.equipment.empty() ? "" : " first ", content.equipment.empty() ? "" : content.equipment[0].icon.c_str(),
                         int(hud_authored_ && hud_authored_->equipment.ok()));
    } else if (OMC_ENV_ONCE("OMC_PORTRAIT_TRACE") && !selection_.empty()) {
        std::fprintf(stderr, "equipment: no cast lead for a selection of %zu\n", selection_.size());
    }
    if (const ui::StyleSet* ss = hud_style_set()) {
        auto fill_style = [&ss](const char* style_name, HudMessageStyle& out) {
            const ui::FontStyle st = ss->resolve(style_name);
            if (!st.ok()) return;
            out.set = true;
            out.height_pt = st.height;
            out.centred = st.hjustify == "Center";
            if (st.has_color) { out.r = st.r; out.g = st.g; out.b = st.b; }
            out.shadow = st.shadow;
            out.shadow_offset = st.shadow_offset;
        };
        fill_style("ErrorDisplay", content.error_style);
        fill_style("WarningDisplay", content.warning_style);
        fill_style("SubtitlePanel", content.transmission_style);
    }
    for (const HudMessage& mm : messages_.all()) {
        if (authored_alert_panel && mm.zone == MsgZone::Alert) continue;
        std::string t = mm.text;
        if (mm.count > 1) t += " x" + std::to_string(mm.count);
        content.messages.push_back({mm.zone, t, mm.ttl, mm.ttl0});
    }
    for (const HudMessage& mm : messages_.all()) {
        if (mm.zone != MsgZone::Alert || !mm.has_target) continue;
        HudContent::MinimapPing ping;
        ping.x = mm.tx;
        ping.y = mm.ty;
        ping.age = std::max(0.0f, mm.ttl0 - mm.ttl);
        if (cat_) {
            if (const auto pt = cat_->scalar("CAlert", "Attack", "PingTime"); pt && !pt->empty())
                ping.ping_time = float(std::atof(pt->c_str()));
            if (const auto pc = cat_->scalar("CAlert", "Attack", "PingColor"); pc && !pc->empty()) {
                int a = 255, r = 255, g = 0, b = 0;
                if (std::sscanf(pc->c_str(), "%d,%d,%d,%d", &a, &r, &g, &b) == 4) {
                    ping.r = std::uint8_t(r); ping.g = std::uint8_t(g); ping.b = std::uint8_t(b);
                }
            }
        }
        if (ping.age < ping.ping_time) content.pings.push_back(ping);
    }

    if (cfg_.human_player >= 0 && cat_) {
        sim::SensorTowerSystem towers;
        for (const sim::Unit& u : world_.units()) {
            if (!u.alive || u.player != cfg_.human_player || u.catalog_id.empty()) continue;
            const double r = cat_->radar_range(u.catalog_id);
            if (r <= 0.0) continue;
            if (u.constructing && cat_->radar_disabled_while_constructing(u.catalog_id)) continue;
            towers.add(u.player, u.x, u.y, sim::Fixed::from_double(r));
        }
        if (towers.count() > 0)
            for (const sim::Blip& b : towers.blips(world_, cfg_.human_player))
                content.radar_blips.emplace_back(float(b.x.to_double()), float(b.y.to_double()));
        const data::Catalog::MinimapColors mc = cat_->minimap_colors();
        for (int k = 0; k < 3; ++k) content.blip_rgb[k] = mc.blip[std::size_t(k)];
        content.blip_alpha = mc.radar_alpha;
    }

    if (cat_) {
        const std::vector<std::string> order = cat_->minimap_priority_order();
        if (!order.empty()) {
            std::size_t normal_rank = 0;
            for (std::size_t k = 0; k < order.size(); ++k)
                if (order[k] == "Normal") { normal_rank = k; break; }
            std::unordered_map<std::string, std::uint8_t> rank_of;
            content.minimap_priority.assign(world_.units().size(), std::uint8_t(normal_rank));
            for (const sim::Unit& u : world_.units()) {
                if (u.id >= content.minimap_priority.size() || u.catalog_id.empty()) continue;
                auto it = rank_of.find(u.catalog_id);
                if (it == rank_of.end()) {
                    std::uint8_t rank = std::uint8_t(normal_rank);
                    const std::string cls = cat_->minimap_render_priority(u.catalog_id);
                    if (!cls.empty())
                        for (std::size_t k = 0; k < order.size(); ++k)
                            if (order[k] == cls) { rank = std::uint8_t(k); break; }
                    it = rank_of.emplace(u.catalog_id, rank).first;
                }
                content.minimap_priority[u.id] = it->second;
            }
        }
    }

    if (cat_) {
        int max_player = -1;
        for (const sim::Unit& u : world_.units())
            if (u.player > max_player) max_player = u.player;
        if (max_player >= 0) {
            content.player_colors.assign(std::size_t(max_player) + 1, HudContent::MinimapTeamColor{});
            for (int p = 0; p <= max_player; ++p) {
                const std::array<float, 4> base = team_colour_for(cat_, p, false);
                const std::array<float, 4> sel = team_colour_for(cat_, p, true);
                if (base[3] < 0.0f) continue;
                HudContent::MinimapTeamColor& tc = content.player_colors[std::size_t(p)];
                for (int k = 0; k < 3; ++k) {
                    tc.rgb[k] = std::uint8_t(base[std::size_t(k)] * 255.0f + 0.5f);
                    tc.sel_rgb[k] = std::uint8_t(sel[std::size_t(k)] * 255.0f + 0.5f);
                }
                tc.set = true;
            }
            if (cfg_.human_player >= 0 && std::size_t(cfg_.human_player) < content.player_colors.size() &&
                !std::getenv("OMC_MINIMAP_TEAM_COLOURS")) {
                HudContent::MinimapTeamColor& own = content.player_colors[std::size_t(cfg_.human_player)];
                own.rgb[0] = 0; own.rgb[1] = 140; own.rgb[2] = 0;
                own.sel_rgb[0] = 0; own.sel_rgb[1] = 255; own.sel_rgb[2] = 0;
                own.set = true;
            }
        }
        content.minimap_selected = selection_;
    }

    if (cfg_.human_player >= 0) {
        const std::size_t n = world_.units().size();
        if (flash_hp_.size() < n) {
            flash_hp_.resize(n, -1.0);
            flash_ms_.resize(n, -1e9);
        }
        for (const sim::Unit& u : world_.units()) {
            if (u.id >= flash_hp_.size()) continue;
            const double now = u.alive ? u.hp.to_double() + u.shields.to_double() : -1.0;
            const double was = flash_hp_[u.id];
            if (was >= 0.0 && now >= 0.0 && now < was) flash_ms_[u.id] = clock_ms_;
            flash_hp_[u.id] = now;
        }
        const double hold_ms = cat_ ? cat_->unit_damage_flash_ms() : 3000.0;
        for (const sim::Unit& u : world_.units()) {
            if (!u.alive || u.loaded || u.in_gas_structure || u.id >= flash_ms_.size()) continue;
            if (u.minimap_radius.raw() <= 0) continue;
            if (clock_ms_ - flash_ms_[u.id] > hold_ms) continue;
            HudContent::MinimapFlash fl;
            fl.unit = u.id;
            if (cat_) {
                const std::array<std::uint8_t, 3> c = minimap_attacked_rgb(cat_, u.player);
                fl.r = c[0];
                fl.g = c[1];
                fl.b = c[2];
            }
            content.minimap_flashes.push_back(fl);
        }
    }
    minimap_fog_content(content);

    if (cat_) {
        const data::Catalog::MinimapColors mc = cat_->minimap_colors();
        for (int k = 0; k < 3; ++k) {
            content.resource_rgb[k] = mc.resource[std::size_t(k)];
            content.frustum_rgb[k] = mc.frustum[std::size_t(k)];
            content.border_rgb[k] = mc.unit_border[std::size_t(k)];
        }
        content.border_px = int(mc.border_size + 0.5f);
        content.min_dot_pt = mc.min_dot_size;
    }

    if (cat_ && ui_store_) {
        for (const sim::Unit& u : world_.units()) {
            if (!u.alive || u.loaded || u.in_gas_structure || u.catalog_id.empty()) continue;
            if (u.minimap_radius.raw() <= 0) continue;
            const auto look = cat_->unit_minimap_icon(u.catalog_id);
            if (!look) continue;
            const IconAtlas::Icon& ic = icons_.get(*ui_store_, look->icon);
            if (!ic.ok()) continue;
            HudContent::MinimapIcon mi;
            mi.x = float(u.x.to_double());
            mi.y = float(u.y.to_double());
            mi.rgba = &ic.rgba;
            mi.tw = int(ic.w);
            mi.th = int(ic.h);
            mi.scale = look->scale;
            mi.unit = u.id;
            content.minimap_icons.push_back(mi);
        }
    }
    ov_lap.lap("ov.content");

    CameraFootprint cam;
    if (map_w_ > 0.0f && map_h_ > 0.0f) {
        Control ctrl = make_control(camctl_.view_proj(aspect), view_w, view_h, viewed);
        if (!OMC_ENV_ONCE("OMC_MINIMAP_QUAD_TERRAIN_HIT")) ctrl.set_ground_height(nullptr);
        const float cxs[4] = {0.0f, float(view_w), float(view_w), 0.0f};
        const float quad_bottom = OMC_ENV_ONCE("OMC_MINIMAP_QUAD_Z0") ? float(view_h) : float(view_h) * 0.75f;
        const float cys[4] = {0.0f, 0.0f, quad_bottom, quad_bottom};
        cam.valid = true;
        for (int k = 0; k < 4 && cam.valid; ++k) {
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{cxs[k], cys[k]}, wx, wy)) {
                cam.x[k] = float(wx.to_double());
                cam.y[k] = float(wy.to_double());
                const render::Vec3 e = camctl_.eye();
                const float gz = ground_z_at(camctl_.focus.x, camctl_.focus.y);
                if (k == 0 && OMC_ENV_ONCE("OMC_STATUS_ANCHOR_TRACE")) {
                    static int quad_traced = 0;
                    if (quad_traced++ < 2)
                        std::fprintf(stderr, "[minimapquad] eye %.2f %.2f %.2f focus %.2f %.2f %.2f ground %.2f target z %.2f\n", double(e.x), double(e.y), double(e.z),
                                     double(camctl_.focus.x), double(camctl_.focus.y), double(camctl_.focus.z), double(gz), double(camctl_.target().z));
                }
                if (!OMC_ENV_ONCE("OMC_MINIMAP_QUAD_Z0") && e.z > gz + 1.0f && e.z > 1.0f) {
                    const float x0 = cam.x[k], y0 = cam.y[k];
                    float h = OMC_ENV_ONCE("OMC_MINIMAP_QUAD_TERRAIN_HIT") ? gz : camctl_.target().z;
                    if (OMC_ENV_ONCE("OMC_MINIMAP_QUAD_TERRAIN"))
                        for (int it = 0; it < 4; ++it) {
                            const float tt = (e.z - h) / e.z;
                            h = ground_z_at(e.x + (x0 - e.x) * tt, e.y + (y0 - e.y) * tt);
                        }
                    const float t = (e.z - h) / e.z;
                    cam.x[k] = e.x + (x0 - e.x) * t;
                    cam.y[k] = e.y + (y0 - e.y) * t;
                }
            } else {
                cam.valid = false;
            }
        }
    }

    ensure_minimap_terrain();
    update_minimap_fog(viewed);
    MinimapBackdrop backdrop;
    backdrop.extent = minimap_extent();
    if (OMC_ENV_ONCE("OMC_ENV_TRACE")) {
        static bool traced = false;
        if (!traced) {
            traced = true;
            std::fprintf(stderr, "[minimap] map %.0f x %.0f playable (%.1f,%.1f)-(%.1f,%.1f) picture %d x %d authored %d\n", double(map_w_), double(map_h_),
                         double(playable_x0_), double(playable_y0_), double(playable_x1_), double(playable_y1_), minimap_tw_, minimap_th_, int(minimap_terrain_authored_));
        }
    }
    if (!minimap_terrain_.empty()) {
        backdrop.terrain = &minimap_terrain_;
        backdrop.tw = minimap_tw_;
        backdrop.th = minimap_th_;
        if (minimap_terrain_authored_ && !minimap_terrain_cropped_ && playable_x1_ > playable_x0_ && playable_y1_ > playable_y0_) {
            minimap_terrain_cropped_ = true;
            const int pw = int(playable_x1_ - playable_x0_ + 0.5f), ph = int(playable_y1_ - playable_y0_ + 0.5f);
            if (!std::getenv("OMC_MINIMAP_TGA_PADDED") && pw > 0 && ph > 0 && (pw < minimap_tw_ || ph < minimap_th_) &&
                minimap_terrain_.size() == std::size_t(minimap_tw_) * std::size_t(minimap_th_) * 3) {
                const int cw = std::min(pw, minimap_tw_), ch = std::min(ph, minimap_th_);
                const int ox = (minimap_tw_ - cw) / 2, oy = (minimap_th_ - ch) / 2;
                std::vector<std::uint8_t> cut(std::size_t(cw) * std::size_t(ch) * 3);
                for (int y = 0; y < ch; ++y)
                    std::memcpy(&cut[std::size_t(y) * std::size_t(cw) * 3],
                                &minimap_terrain_[(std::size_t(y + oy) * std::size_t(minimap_tw_) + std::size_t(ox)) * 3], std::size_t(cw) * 3);
                minimap_terrain_ = std::move(cut);
                minimap_tw_ = cw;
                minimap_th_ = ch;
                backdrop.terrain = &minimap_terrain_;
                backdrop.tw = minimap_tw_;
                backdrop.th = minimap_th_;
            }
        }
        if (minimap_terrain_authored_ && playable_x1_ > playable_x0_ && playable_y1_ > playable_y0_) {
            backdrop.terrain_extent.x0 = playable_x0_;
            backdrop.terrain_extent.y0 = playable_y0_;
            backdrop.terrain_extent.x1 = playable_x1_;
            backdrop.terrain_extent.y1 = playable_y1_;
        }
    }
    hud_minimap_apply(content, backdrop);
    if (!minimap_fog_.empty()) {
        backdrop.fog = &minimap_fog_;
        backdrop.fw = minimap_fw_;
        backdrop.fh = minimap_fh_;
        backdrop.viewer_player = cfg_.human_player;
    }
    ov_lap.lap("ov.minimap");
    ensure_console_texture(view_w, view_h);
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
    content.widget_art = cfg_.human_player >= 0 && ui_store_ && console_panel.authored &&
                         !std::getenv("OMC_NO_CONSOLE_WIDGET_ART");
    draw::set_forced_font_px(forced_px);
    ov_lap.lap("ov.layout");
    static std::vector<std::uint8_t> hud;
    {
        OMC_PROF_SCOPE("ov.hud_render");
        HudRender::render_into(hud, content, world_, map_w_, map_h_, view_w, view_h, &cam, &backdrop,
                               console_tex_.ok() ? &console_panel : nullptr);
    }
    draw::set_forced_font_px(0);
    ov_lap.lap("ov.hud");
    std::vector<float> status_anchor_z(world_.units().size(), 0.0f);
    std::vector<float> status_bar_w(world_.units().size(), 0.0f);
    std::vector<float> status_bar_off(world_.units().size(), 0.0f);
    const bool bar_offset_rule = cat_ && !OMC_ENV_ONCE("OMC_STATUS_ANCHOR_OVERHEAD");
    static const bool kPrepUncached = std::getenv("OMC_STATUS_PREP_UNCACHED") != nullptr;
    const bool anchor_bounds_once = OMC_ENV_ONCE("OMC_STATUS_ANCHOR_BOUNDS") != nullptr;
    const bool anchor_trace_once = OMC_ENV_ONCE("OMC_STATUS_ANCHOR_TRACE") != nullptr;
    const bool bar_by_radius_once = OMC_ENV_ONCE("OMC_BAR_WIDTH_BY_RADIUS") != nullptr;
    auto anchor_bounds = [&] { return kPrepUncached ? std::getenv("OMC_STATUS_ANCHOR_BOUNDS") != nullptr : anchor_bounds_once; };
    auto anchor_trace = [&] { return kPrepUncached ? std::getenv("OMC_STATUS_ANCHOR_TRACE") != nullptr : anchor_trace_once; };
    auto bar_by_radius = [&] { return kPrepUncached ? std::getenv("OMC_BAR_WIDTH_BY_RADIUS") != nullptr : bar_by_radius_once; };
    auto display_height = [&](const std::string& id) {
        if (kPrepUncached) return cat_->unit_display_height(id);
        auto hit = display_h_cache_.find(id);
        if (hit == display_h_cache_.end()) hit = display_h_cache_.emplace(id, cat_->unit_display_height(id)).first;
        return hit->second;
    };
    for (const Unit& u : world_.units()) {
        if (!u.alive) continue;
        const float ux = float(u.x.to_double()), uy = float(u.y.to_double());
        const formats::Model* m = (u.id < driver_.actors.size()) ? driver_.actors[u.id].model : nullptr;
        std::unordered_map<const formats::Model*, float>& ref_z = ref_z_cache_;
        float top = m ? m->bounds_max[2] : float(u.radius.to_double());
        if (m && !anchor_bounds()) {
            auto it = ref_z.find(m);
            if (it == ref_z.end()) {
                float z = -1.0f;
                render::Mat4 att;
                const int sseq = render::select_sequence(*m, {"Stand"}, 0);
                if (render::attachment_index(*m, "Ref_Overhead") >= 0 &&
                    render::attachment_transform(*m, "Ref_Overhead", std::uint32_t(sseq < 0 ? 0 : sseq), 0.0f, att))
                    z = att.m[14];
                if (anchor_trace()) {
                    std::fprintf(stderr, "[statusanchor] ref_z=%.3f bounds_top=%.3f scale=%.3f %s\n", double(z),
                                 double(m->bounds_max[2]), double(unit_draw_scale(cat_, u.catalog_id)[2]), m->name.c_str());
                    for (const auto& a : m->attachments) {
                        render::Mat4 t;
                        if (render::attachment_transform(*m, a.name, std::uint32_t(sseq < 0 ? 0 : sseq), 0.0f, t))
                            std::fprintf(stderr, "[statusanchor]   %s z=%.3f\n", a.name.c_str(), double(t.m[14]));
                    }
                }
                it = ref_z.emplace(m, z).first;
            }
            if (it->second > 0.0f) top = it->second;
        }
        status_anchor_z[u.id] = ground_z_at(ux, uy) + (m ? top * unit_draw_scale(cat_, u.catalog_id)[2] : top);
        if (bar_offset_rule) {
            static std::unordered_map<std::string, float> authored_off;
            auto oit = authored_off.find(u.catalog_id);
            if (oit == authored_off.end())
                oit = authored_off.emplace(u.catalog_id, float(cat_->scalar_num("CActorUnit", u.catalog_id, "BarOffset").value_or(100.0))).first;
            status_bar_off[u.id] = oit->second;
            status_anchor_z[u.id] = ground_z_at(ux, uy);
            static const bool kAirOrigin = std::getenv("OMC_STATUS_AIR_ON_GROUND") == nullptr;
            float dh = kAirOrigin ? display_height(u.catalog_id) : 0.0f;
            if (kAirOrigin && u.flying && dh <= 0.0f) {
                const float fh = display_height(u.catalog_id + "Flying");
                dh = fh > 0.0f ? fh : 3.25f;
            }
            if (dh > 0.0f) status_anchor_z[u.id] = air_z_at(ux, uy) + dh;
        }
        if (cat_ && !bar_by_radius()) {
            static std::unordered_map<std::string, float> authored_w;
            auto wit = authored_w.find(u.catalog_id);
            if (wit == authored_w.end())
                wit = authored_w.emplace(u.catalog_id, float(cat_->scalar_num("CActorUnit", u.catalog_id, "BarWidth").value_or(60.0))).first;
            status_bar_w[u.id] = wit->second * float(view_h) / 900.0f;
        }
        static std::vector<std::uint32_t> traced_bodies;
        if (anchor_trace() && u.player == viewed && u.cargo_space > 0 &&
            std::find(traced_bodies.begin(), traced_bodies.end(), u.id) == traced_bodies.end() && (traced_bodies.push_back(u.id), true))
            std::fprintf(stderr,"[statusbody] %s radius=%.3f bar_w=%.1f\n", u.catalog_id.c_str(), u.radius.to_double(), double(status_bar_w[u.id]));
    }
    ov_lap.lap("ov.status_prep");
    std::vector<std::uint32_t> status_sel = selection_;
    static const bool kNoHover = std::getenv("OMC_NO_HOVER") != nullptr;
    if (!kNoHover && cfg_.human_player >= 0 && !hud_covers_point(view_w, view_h, console_tex_.ok() ? &console_panel : nullptr, mouse_x_, mouse_y_) && !hud_buttons_.at(mouse_x_, mouse_y_)) {
        Control hctrl = make_control(camctl_.view_proj(aspect), view_w, view_h, viewed);
        const ScreenPoint mp{mouse_x_, mouse_y_};
        std::uint32_t hov = std::getenv("OMC_CLICK_PICK_OLD") ? hctrl.pick_own(world_, mp) : click_select_at(hctrl, mp);
        if (hov == sim::kNoTarget && std::getenv("OMC_CLICK_PICK_OLD")) hov = hctrl.pick_target(world_, mp);
        if (hov < world_.units().size() && !reveal_map_ && cfg_.human_player >= 0 && vision_.width() > 0) {
            const Unit& hu = world_.units()[hov];
            if (hu.player != cfg_.human_player &&
                !hud_fog_body_drawn(fog_body_state(hu.player, int(vision_.state_at(hu.x, hu.y)), fog_hidden_black_), FogVisibility(hu.fog_visibility)))
                hov = sim::kNoTarget;
            static const bool kHoverLeakOld = std::getenv("OMC_FOG_LEAK_OLD") != nullptr || std::getenv("OMC_FOG_SNAPSHOT_OLD") != nullptr;
            if (!kHoverLeakOld && ground_fog_ && hov != sim::kNoTarget && hu.player >= 0 && hu.player != cfg_.human_player &&
                !world_.are_allied(hu.player, cfg_.human_player) && FogVisibility(hu.fog_visibility) != FogVisibility::Visible &&
                vision_.state_at(hu.x, hu.y) != sim::Visibility::Visible && fog_snaps_.find(hov) == fog_snaps_.end())
                hov = sim::kNoTarget;
        }
        if (hov < world_.units().size() && !world_.units()[hov].unselectable &&
            std::find(status_sel.begin(), status_sel.end(), hov) == status_sel.end())
            status_sel.push_back(hov);
        hover_unit_ = hov < world_.units().size() ? hov : sim::kNoTarget;
    } else {
        hover_unit_ = sim::kNoTarget;
    }
    ov_lap.lap("ov.hover");
    render::UnitOverlay::StatusBarOptions status_opts;
    status_opts.mode = status_bar_mode_;
    status_opts.reveal_all = alt_held_;
    status_opts.own_rows = !OMC_ENV_ONCE("OMC_NO_OWN_STATUS_ROWS");
    status_opts.viewer_player = viewed;
    status_opts.selection = &status_sel;
    status_opts.anchor_z = &status_anchor_z;
    status_opts.bar_width = &status_bar_w;
    status_opts.bar_offset = &status_bar_off;
    std::vector<std::uint8_t> status_cargo_ok(world_.units().size(), 1);
    if (cat_ && !OMC_ENV_ONCE("OMC_CARGO_ROW_ALWAYS"))
        for (const Unit& u : world_.units()) {
            if (!u.alive || u.cargo_space <= 0 || u.catalog_id.empty()) continue;
            static std::unordered_map<std::string, std::string> load_req;
            auto it = load_req.find(u.catalog_id);
            if (it == load_req.end()) {
                std::string req;
                for (const std::string& ab : cat_->array_links("CUnit", u.catalog_id, "AbilArray")) {
                    const data::Catalog::Entry* e = ab.empty() ? nullptr : cat_->find("CAbil", ab);
                    if (!e || e->tag != "CAbilTransport") continue;
                    req = cat_->button_requirement(ab, "Load");
                    break;
                }
                it = load_req.emplace(u.catalog_id, req).first;
            }
            if (!it->second.empty() && !data::eval_requirement(*cat_, it->second, "Use", tech_state_of(u.player)))
                status_cargo_ok[u.id] = 0;
        }
    status_opts.cargo_ok = &status_cargo_ok;
    status_opts.px_scale = float(view_h) / 900.0f;
    {
        const std::size_t n = world_.units().size();
        status_trail_life_.resize(n, -1.0f);
        status_trail_shield_.resize(n, -1.0f);
        status_rate_life_.resize(n, 0.0f);
        status_rate_shield_.resize(n, 0.0f);
        const int now = int(world_.tick());
        const float dt = status_trail_tick_ < 0 ? 0.0f : float(now - status_trail_tick_) / float(sim::World::kTicksPerSecond);
        status_trail_tick_ = now;
        auto advance = [&](float cur, float& shown, float& rate) {
            if (shown < 0.0f || cur >= shown) { shown = cur; rate = 0.0f; return; }
            const float want = (shown - cur) / 1.0f;
            if (want > rate) rate = want;
            shown -= rate * dt;
            if (shown <= cur) { shown = cur; rate = 0.0f; }
        };
        for (const Unit& su : world_.units()) {
            if (!su.alive || su.hp_max.raw() <= 0) continue;
            advance(float(su.hp.to_double() / su.hp_max.to_double()), status_trail_life_[su.id], status_rate_life_[su.id]);
            if (su.shields_max.raw() > 0)
                advance(float(su.shields.to_double() / su.shields_max.to_double()), status_trail_shield_[su.id], status_rate_shield_[su.id]);
        }
        status_opts.life_trail = &status_trail_life_;
        status_opts.shield_trail = &status_trail_shield_;
    }
    ov_lap.lap("ov.status_opts");
    static std::vector<std::uint8_t> units;
    static int units_y0 = 0, units_y1 = -1;
    unit_overlay_.render_status_into(units, units_y0, units_y1, world_, camctl_.view_proj(aspect), view_w, view_h, status_opts);
    if (keep_status_layer_) status_layer_ = units;
    ov_lap.lap("ov.status");

    { OMC_PROF_SCOPE("ov.clear"); out.assign(std::size_t(view_w) * view_h * 4, 0); }

    if (!ground_fog_ && !reveal_map_ && cfg_.human_player >= 0 && map_w_ > 0.0f && map_h_ > 0.0f && !minimap_fog_.empty()) {
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

    ov_lap.lap("ov.fog");
    {
        OMC_PROF_SCOPE("ov.alpha_over");
        if (units_y0 <= units_y1)
            alpha_over(out, units, std::size_t(units_y0) * std::size_t(view_w) * 4, std::size_t(units_y1 + 1) * std::size_t(view_w) * 4);
        alpha_over(out, hud);
    }
    ov_lap.lap("ov.compose");

    if (dragging_)
        draw_box_rgba(out, view_w, view_h, int(drag_x0_), int(drag_y0_), int(mouse_x_),
                      int(mouse_y_), 80, 235, 110);

    struct HarvestLabel { int sx, sy, cur, ideal, res; };
    std::vector<HarvestLabel> harvest_labels;
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
            const float top = nm ? nm->bounds_max[2] * unit_draw_scale(cat_, n.catalog_id)[2] : float(n.radius.to_double());
            float px = 0.0f, py = 0.0f;
            if (!cctrl.project(x, y, cctrl.ground_z(x, y) + top, px, py)) return false;
            sx = int(px);
            sy = int(py);
            return true;
        };
        for (std::size_t pli = 0; pli < layout_.players.size(); ++pli) {
            const MeleePlayerLayout& pl = layout_.players[pli];
            if (cfg_.human_player >= 0 && int(pli) != cfg_.human_player) continue;
            int total = 0, best_sx = 0, best_sy = 1 << 30, live_patches = 0;
            for (std::uint32_t nid : pl.minerals) {
                if (nid >= world_.units().size() || !world_.unit(nid).alive) continue;
                ++live_patches;
                total += assigned[nid];
                int sx, sy;
                if (proj(world_.unit(nid), sx, sy) && sy < best_sy) { best_sy = sy; best_sx = sx; }
            }
            if (live_patches > 0 && best_sy < (1 << 30)) {
                int hx = best_sx, hy = best_sy;
                if (pl.town_hall < world_.units().size() && world_.unit(pl.town_hall).alive) {
                    int sx, sy;
                    if (proj(world_.unit(pl.town_hall), sx, sy)) { hx = sx; hy = sy; }
                }
                harvest_labels.push_back({hx, hy, total, live_patches * 2, 0});
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
                if (proj(g, sx, sy)) harvest_labels.push_back({sx, sy, assigned[gid], 3, 1});
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

        if (pending_build_ || pending_refinery_ || ghost_.active) {
            const float wx = ghost_.active ? ghost_.x : float(build_px_.to_double());
            const float wy = ghost_.active ? ghost_.y : float(build_py_.to_double());
            const int r = build_valid_ ? 60 : 225, g = build_valid_ ? 215 : 60;
            std::vector<std::pair<int, int>> cells;
            if (cat_ && !build_proto_.catalog_id.empty() && !ghost_.active)
                cells = cat_->unit_pathing_footprint_offsets(build_proto_.catalog_id);
            bool drew = false;
            const bool per_cell = ghost_.active && !ghost_.cells.empty();
            if (per_cell) {
                cells.clear();
                for (const std::pair<int, int>& c : ghost_.cells)
                    cells.emplace_back(c.first - int(std::floor(wx)), c.second - int(std::floor(wy)));
            }
            if (!cells.empty()) {
                const float ox = std::floor(wx), oy = std::floor(wy);
                for (std::size_t ck = 0; ck < cells.size(); ++ck) {
                    const std::pair<int, int>& c = cells[ck];
                    const bool cell_green = per_cell ? ghost_.cell_ok[ck] != 0 : build_valid_;
                    const int cr = cell_green ? 60 : 225, cgr = cell_green ? 215 : 60;
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
                        for (int x = xs; x <= xe; ++x) putpx(x, y, cr, cgr, 60, 70);
                    }
                    for (int k = 0; k < 4; ++k)
                        line(q[k][0], q[k][1], q[(k + 1) % 4][0], q[(k + 1) % 4][1], cr, cgr, 60, 255);
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

        static const bool kWaypointPathOld = std::getenv("OMC_WAYPOINT_PATH_OLD") != nullptr;
        if (!kWaypointPathOld && cfg_.human_player >= 0) draw_order_paths(view_w, view_h, out, ovctrl, clip_console);
        if (kWaypointPathOld && cfg_.human_player >= 0)
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

        if (kWaypointPathOld && cfg_.human_player >= 0 && cat_)
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
                const IconAtlas::Icon* wpl = (ui_store_ && !std::getenv("OMC_RALLY_LINE_PLAIN")) ? &icons_.get(*ui_store_, "Assets/Textures/waypointline.dds") : nullptr;
                const double seg_len = std::hypot(double(tx - ux), double(ty - uy));
                if (wpl && wpl->ok() && seg_len > 1.0) {
                    const double dxn = double(tx - ux) / seg_len, dyn = double(ty - uy) / seg_len;
                    const double half = 2.0, tile = 26.0, strength = 0.6;
                    const int bx0 = std::max(0, std::min(ux, tx) - 3), bx1 = std::min(int(view_w) - 1, std::max(ux, tx) + 3);
                    const int by0 = std::max(0, std::min(uy, ty) - 3), by1 = std::min(int(view_h) - 1, std::max(uy, ty) + 3);
                    for (int y = by0; y <= by1; ++y)
                        for (int x = bx0; x <= bx1; ++x) {
                            const double px_ = double(x - ux), py_ = double(y - uy);
                            const double along = px_ * dxn + py_ * dyn, across = -px_ * dyn + py_ * dxn;
                            if (along < 0.0 || along > seg_len || across < -half || across >= half) continue;
                            const int sx = std::min(int(wpl->w) - 1, int((across + half) / (2.0 * half) * double(wpl->w)));
                            const double v = std::fmod(seg_len - along, tile) / tile;
                            const int sy = std::min(int(wpl->h) - 1, int(v * double(wpl->h)));
                            const std::size_t si = (std::size_t(sy) * wpl->w + std::size_t(sx)) * 4;
                            const double a = strength * double(wpl->rgba[si + 3]) / 255.0;
                            if (a <= 0.0) continue;
                            const std::size_t di = (std::size_t(y) * view_w + std::size_t(x)) * 4;
                            const int tint[3] = {rr, rg, rb};
                            if (out[di + 3] == 0) {
                                for (int k = 0; k < 3; ++k)
                                    out[di + k] = std::uint8_t(std::min(255.0, double(wpl->rgba[si + k]) / 255.0 * double(tint[k])) + 0.5);
                                out[di + 3] = std::uint8_t(a * 255.0 + 0.5);
                                continue;
                            }
                            for (int k = 0; k < 3; ++k) {
                                const double src = double(wpl->rgba[si + k]) / 255.0 * double(tint[k]);
                                out[di + k] = std::uint8_t(std::min(255.0, src * a + double(out[di + k])) + 0.5);
                            }
                            out[di + 3] = std::uint8_t(std::max(double(out[di + 3]), a * 255.0));
                        }
                } else {
                    line(ux, uy, tx, ty, rr, rg, rb, 170);
                }
                if (std::getenv("OMC_RALLY_LINE_PLAIN")) marker(tx, ty, rr, rg, rb);
            }

    }

    ov_lap.lap("ov.labels");
    static const bool kIconUncached = std::getenv("OMC_HUD_ICON_UNCACHED") != nullptr;
    auto blit_icon_tinted = [&](const IconAtlas::Icon& ic, int dx0, int dy0, int dx1, int dy1,
                                float tr, float tg, float tb) {
        if (!ic.ok() || dx1 <= dx0 || dy1 <= dy0) return;
        const int dw = dx1 - dx0, dh = dy1 - dy0;
        const float tint[3] = {tr, tg, tb};
        static const bool tint_nearest_all = std::getenv("OMC_HUD_ICON_NEAREST") != nullptr;
        if (!kIconUncached) {
            const std::vector<float>& lay = icon_resample_.box4(ic, dw, dh, tint_nearest_all);
            for (int y = 0; y < dh; ++y)
                for (int x = 0; x < dw; ++x) {
                    const float* p = &lay[(std::size_t(y) * std::size_t(dw) + std::size_t(x)) * 4];
                    const float a = p[3];
                    if (a <= 0.0f) continue;
                    const int px = dx0 + x, py = dy0 + y;
                    if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
                    const std::size_t di = (std::size_t(py) * view_w + px) * 4;
                    for (int c = 0; c < 3; ++c) {
                        const float src = p[c] * tint[c];
                        out[di + c] = std::uint8_t(src * a + out[di + c] * (1 - a));
                    }
                    const float da = out[di + 3] / 255.0f;
                    out[di + 3] = std::uint8_t((a + da * (1.0f - a)) * 255.0f + 0.5f);
                }
            return;
        }
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x) {
                static const bool tint_nearest = std::getenv("OMC_HUD_ICON_NEAREST") != nullptr;
                float a = 0.0f, rgb[3] = {0.0f, 0.0f, 0.0f};
                if (tint_nearest || (int(ic.w) <= dw && int(ic.h) <= dh)) {
                    const int sx = x * int(ic.w) / dw, sy = y * int(ic.h) / dh;
                    const std::size_t si = (std::size_t(sy) * ic.w + sx) * 4;
                    a = ic.rgba[si + 3] / 255.0f;
                    for (int c = 0; c < 3; ++c) rgb[c] = float(ic.rgba[si + c]);
                } else {
                    float wa = 0.0f;
                    for (int j = 0; j < 4; ++j)
                        for (int k = 0; k < 4; ++k) {
                            const int sx = std::min(int(ic.w) - 1, int((float(x) + (float(k) + 0.5f) * 0.25f) * float(ic.w) / float(dw)));
                            const int sy = std::min(int(ic.h) - 1, int((float(y) + (float(j) + 0.5f) * 0.25f) * float(ic.h) / float(dh)));
                            const std::size_t si = (std::size_t(sy) * ic.w + std::size_t(sx)) * 4;
                            const float sa = ic.rgba[si + 3] / 255.0f;
                            wa += sa;
                            for (int c = 0; c < 3; ++c) rgb[c] += float(ic.rgba[si + c]) * sa;
                        }
                    a = wa / 16.0f;
                    if (wa > 0.0f) for (float& v : rgb) v /= wa;
                }
                if (a <= 0.0f) continue;
                const int px = dx0 + x, py = dy0 + y;
                if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
                const std::size_t di = (std::size_t(py) * view_w + px) * 4;
                for (int c = 0; c < 3; ++c) {
                    const float src = rgb[c] * tint[c];
                    out[di + c] = std::uint8_t(src * a + out[di + c] * (1 - a));
                }
                const float da = out[di + 3] / 255.0f;
                out[di + 3] = std::uint8_t((a + da * (1.0f - a)) * 255.0f + 0.5f);
            }
    };

    auto blit_icon_add = [&](const IconAtlas::Icon& ic, int dx0, int dy0, int dx1, int dy1, float strength, float tr, float tg, float tb) {
        if (!ic.ok() || dx1 <= dx0 || dy1 <= dy0 || strength <= 0.0f) return;
        const int dw = dx1 - dx0, dh = dy1 - dy0;
        const float tint[3] = {tr, tg, tb};
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x) {
                const int px = dx0 + x, py = dy0 + y;
                if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
                const std::size_t si = (std::size_t(y * int(ic.h) / dh) * ic.w + std::size_t(x * int(ic.w) / dw)) * 4;
                const float a = ic.rgba[si + 3] / 255.0f * strength;
                if (a <= 0.0f) continue;
                const std::size_t di = (std::size_t(py) * view_w + px) * 4;
                for (int c = 0; c < 3; ++c) out[di + c] = std::uint8_t(std::min(255.0f, float(out[di + c]) + float(ic.rgba[si + c]) * tint[c] * a));
            }
    };

    auto blit_icon = [&](const IconAtlas::Icon& ic, int dx0, int dy0, int dx1, int dy1) {
        if (!ic.ok() || dx1 <= dx0 || dy1 <= dy0) return;
        const int dw = dx1 - dx0, dh = dy1 - dy0;
        static const bool nearest_only = std::getenv("OMC_HUD_ICON_NEAREST") != nullptr;
        if (!nearest_only && (int(ic.w) > dw || int(ic.h) > dh) && !kIconUncached) {
            const std::vector<double>& lay = icon_resample_.area(ic, dw, dh);
            for (int y = 0; y < dh; ++y)
                for (int x = 0; x < dw; ++x) {
                    const int px = dx0 + x, py = dy0 + y;
                    if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
                    const double* p = &lay[(std::size_t(y) * std::size_t(dw) + std::size_t(x)) * 4];
                    if (p[3] < 0.0) continue;
                    const float a = float(p[3]);
                    const std::size_t di = (std::size_t(py) * view_w + px) * 4;
                    for (int c = 0; c < 3; ++c) out[di + c] = std::uint8_t(std::min(255.0, p[c] * a + out[di + c] * (1.0 - a) + 0.5));
                    const float da = out[di + 3] / 255.0f;
                    out[di + 3] = std::uint8_t((a + da * (1.0f - a)) * 255.0f + 0.5f);
                }
            return;
        }
        if (!nearest_only && (int(ic.w) > dw || int(ic.h) > dh)) {
            for (int y = 0; y < dh; ++y)
                for (int x = 0; x < dw; ++x) {
                    const int px = dx0 + x, py = dy0 + y;
                    if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
                    const double fx0 = double(x) * ic.w / dw, fx1 = double(x + 1) * ic.w / dw;
                    const double fy0 = double(y) * ic.h / dh, fy1 = double(y + 1) * ic.h / dh;
                    double acc[3] = {0, 0, 0}, wa = 0.0, wsum = 0.0;
                    for (int sy = int(fy0); sy < int(ic.h) && double(sy) < fy1; ++sy) {
                        const double wy = std::min(fy1, double(sy + 1)) - std::max(fy0, double(sy));
                        for (int sx = int(fx0); sx < int(ic.w) && double(sx) < fx1; ++sx) {
                            const double w = wy * (std::min(fx1, double(sx + 1)) - std::max(fx0, double(sx)));
                            if (w <= 0.0) continue;
                            const std::size_t si = (std::size_t(sy) * ic.w + std::size_t(sx)) * 4;
                            const double ta = ic.rgba[si + 3] / 255.0;
                            for (int c = 0; c < 3; ++c) acc[c] += ic.rgba[si + c] * ta * w;
                            wa += ta * w;
                            wsum += w;
                        }
                    }
                    if (wsum <= 0.0 || wa <= 0.0) continue;
                    const float a = float(wa / wsum);
                    const std::size_t di = (std::size_t(py) * view_w + px) * 4;
                    for (int c = 0; c < 3; ++c) out[di + c] = std::uint8_t(std::min(255.0, acc[c] / wa * a + out[di + c] * (1.0 - a) + 0.5));
                    const float da = out[di + 3] / 255.0f;
                    out[di + 3] = std::uint8_t((a + da * (1.0f - a)) * 255.0f + 0.5f);
                }
            return;
        }
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

    auto blit_nine = [&](const IconAtlas::Icon& t9, double fx0, double fy0, double fx1, double fy1, double corner, float mr, float mg, float mb, float add) {
        if (!t9.ok() || fx1 <= fx0 || fy1 <= fy0 || corner <= 0.0) return;
        const int x0 = int(std::floor(fx0)), y0 = int(std::floor(fy0)), x1 = int(std::ceil(fx1)), y1 = int(std::ceil(fy1));
        const double w = fx1 - fx0, h = fy1 - fy0, hw = double(t9.w) * 0.5, hh = double(t9.h) * 0.5;
        const double cw = std::min(corner, w * 0.5), ch = std::min(corner, h * 0.5);
        auto src = [](double d, double len, double c, double half) {
            if (d < c) return d * half / c;
            if (d >= len - c) return 2.0 * half - (len - d) * half / c;
            return half;
        };
        const float mul[3] = {mr, mg, mb};
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                if (x < 0 || y < 0 || x >= int(view_w) || y >= int(view_h)) continue;
                float rgb[3] = {0.0f, 0.0f, 0.0f}, a = 0.0f;
                for (int sy = 0; sy < 6; ++sy)
                    for (int sx = 0; sx < 6; ++sx) {
                        const double dx = double(x) + (sx + 0.5) / 6.0 - fx0, dy = double(y) + (sy + 0.5) / 6.0 - fy0;
                        if (dx < 0.0 || dy < 0.0 || dx >= w || dy >= h) continue;
                        const int u = std::max(0, std::min(int(t9.w) - 1, int(src(dx, w, cw, hw))));
                        const int v = std::max(0, std::min(int(t9.h) - 1, int(src(dy, h, ch, hh))));
                        const std::size_t si = (std::size_t(v) * t9.w + std::size_t(u)) * 4;
                        const float ta = t9.rgba[si + 3] / 255.0f;
                        for (int c = 0; c < 3; ++c) rgb[c] += float(t9.rgba[si + c]) * ta;
                        a += ta;
                    }
                if (a <= 0.0f) continue;
                for (int c = 0; c < 3; ++c) rgb[c] /= a;
                a /= 36.0f;
                const std::size_t di = (std::size_t(y) * view_w + std::size_t(x)) * 4;
                for (int c = 0; c < 3; ++c) out[di + c] = std::uint8_t(std::min(255.0f, (rgb[c] * mul[c] + add) * a + out[di + c] * (1.0f - a)));
                const float da = out[di + 3] / 255.0f;
                out[di + 3] = std::uint8_t((a + da * (1.0f - a)) * 255.0f + 0.5f);
            }
    };

    auto draw_portrait = [&](const std::string& pid) {
        if (models_ && ui_store_ && console_panel.authored && console_panel.authored->portrait.ok()) {
            const double sc = double(view_h) / double(kRefHeight);
            const HudAuthoredRects::R& pr = console_panel.authored->portrait;
            const MeleeModels::Portrait* const p =
                portrait_async_ && !warming_ && !models_->prefetch_portrait(*ui_store_, pid) ? nullptr : models_->portrait(*ui_store_, pid);
            ov_lap.lap("ov.portrait_load");
            if (p) {
                HudPortrait req;
                req.unit = pid;
                req.path = p->path;
                req.fov_deg = p->fov_rad * 180.0f / 3.14159265f;
                req.rect = {int(pr.x0 * sc + 0.5), int(pr.y0 * sc + 0.5), int(pr.x1 * sc + 0.5), int(pr.y1 * sc + 0.5)};
                const std::uint32_t pw = std::uint32_t(std::max(1, req.rect.w())), ph = std::uint32_t(std::max(1, req.rect.h()));
                if (portrait_renderer_) {
                    std::vector<std::uint8_t> img;
                    static std::uint32_t portrait_lead = 0xffffffffu;
                    static double portrait_start_ms = 0.0;
                    const std::uint32_t lead_now = selection_.empty() ? 0xffffffffu : selection_[0];
                    if (lead_now != portrait_lead) {
                        portrait_lead = lead_now;
                        portrait_start_ms = double(clock_ms_);
                    }
                    static const bool match_clock = std::getenv("OMC_PORTRAIT_FROM_SELECTION") == nullptr;
                    portrait_time_ms() = std::getenv("OMC_PORTRAIT_STILL") ? 0.0f
                                         : match_clock ? float(clock_ms_) : float(std::max(1.0, double(clock_ms_) - portrait_start_ms));
                    {
                        PortraitDof& pd = portrait_dof();
                        pd.focus = p->dof_focus;
                        pd.start = p->dof_start;
                        pd.end = p->dof_end;
                        pd.amount = p->dof_amount;
                    }
                    const double pnow = double(clock_ms_);
                    const bool preuse = portrait_hz_ > 0.0f && !warming_ && portrait_cache_.path == p->path && portrait_cache_.w == pw &&
                                        portrait_cache_.h == ph && !portrait_cache_.img.empty() && pnow >= portrait_cache_.at_ms &&
                                        pnow - portrait_cache_.at_ms < 1000.0 / double(portrait_hz_) - 0.5;
                    last_portrait_w_ = pw;
                    last_portrait_h_ = ph;
                    {
                        static std::uint32_t told_w = 0, told_h = 0;
                        if (std::getenv("OMC_MEM_TRACE") && (pw != told_w || ph != told_h)) {
                            told_w = pw;
                            told_h = ph;
                            std::printf("overmindcore: portrait pane %ux%u px at a %ux%u view\n", pw, ph, view_w, view_h);
                        }
                    }
                    if (preuse) {
                        img = portrait_cache_.img;
                    } else {
                        portrait_load_mode() = portrait_async_ && !warming_ ? 1 : 0;
                        try { img = portrait_renderer_(*ui_store_, p->model, p->fov_rad, p->cam_bone, p->znear, p->zfar, pw, ph, p->light.found ? &p->light : nullptr); } catch (const std::exception& e) {
                            if (std::getenv("OMC_PORTRAIT_TRACE")) std::fprintf(stderr, "portrait: render threw %s\n", e.what());
                        }
                        portrait_load_mode() = 0;
                        if (portrait_hz_ > 0.0f && !warming_) {
                            portrait_cache_.path = p->path;
                            portrait_cache_.w = pw;
                            portrait_cache_.h = ph;
                            portrait_cache_.at_ms = pnow;
                            portrait_cache_.img = img;
                        }
                    }
                    render::frame_prof_count("portrait.reused", preuse ? 1.0 : 0.0);
                    ov_lap.lap("ov.portrait_render");
                    if (std::getenv("OMC_PORTRAIT_TRACE")) {
                        std::size_t covered = 0;
                        for (std::size_t q = 3; q < img.size(); q += 4) covered += img[q] != 0;
                        std::fprintf(stderr, "portrait: %s %ux%u -> %zu bytes, %zu covered, fov %.3f cam bone %d near %.3f far %.3f verts %u\n", p->path.c_str(), pw, ph,
                                     img.size(), covered, double(p->fov_rad), p->cam_bone, double(p->znear), double(p->zfar), unsigned(p->model.vertex_count));
                        std::fprintf(stderr, "portrait: light '%s' found %d exposure %.3f white %.3f ambient %.3f %.3f %.3f key %d (%.3f %.3f %.3f x %.3f) fill %d back %d materials %zu regions %zu\n",
                                     p->light.light_id.c_str(), int(p->light.found), double(p->light.hdr_exposure), double(p->light.hdr_white_point),
                                     double(p->light.ambient_color[0]), double(p->light.ambient_color[1]), double(p->light.ambient_color[2]), int(p->light.key.present),
                                     double(p->light.key.color[0]), double(p->light.key.color[1]), double(p->light.key.color[2]), double(p->light.key.color_multiplier),
                                     int(p->light.fill.present), int(p->light.back.present), p->model.materials.size(), p->model.regions.size());
                    }
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
                        if (std::getenv("OMC_PORTRAIT_TRACE"))
                            std::fprintf(stderr, "portrait: console picture ok %d %ux%u view %ux%u\n", int(console_tex_.ok()), unsigned(console_tex_.w), unsigned(console_tex_.h), view_w, view_h);
                        if (console_tex_.ok() && console_tex_.w == view_w && console_tex_.h <= view_h && !std::getenv("OMC_PORTRAIT_OVER_CONSOLE")) {
                            const int coy = int(view_h) - int(console_tex_.h);
                            const int ccx = (req.rect.x0 + req.rect.x1) / 2, ccy = (req.rect.y0 + req.rect.y1) / 2 - coy;
                            const bool well_open = ccx >= 0 && ccy >= 0 && ccx < int(view_w) && ccy < int(console_tex_.h);
                            const std::size_t wc = well_open ? (std::size_t(ccy) * view_w + std::size_t(ccx)) * 4 : 0;
                            const int well_rgb[3] = {well_open ? int(console_tex_.rgba[wc]) : 0, well_open ? int(console_tex_.rgba[wc + 1]) : 0,
                                                     well_open ? int(console_tex_.rgba[wc + 2]) : 0};
                            if (std::getenv("OMC_PORTRAIT_TRACE")) {
                                std::size_t clear = 0, total = 0;
                                for (int dy = std::max(coy, req.rect.y0); dy < std::min(int(view_h), req.rect.y1); ++dy)
                                    for (int dx = std::max(0, req.rect.x0); dx < std::min(int(view_w), req.rect.x1); ++dx) {
                                        ++total;
                                        clear += console_tex_.rgba[(std::size_t(dy - coy) * view_w + std::size_t(dx)) * 4 + 3] < 128;
                                    }
                                const std::size_t cc = (std::size_t(std::max(0, ccy)) * view_w + std::size_t(std::max(0, ccx))) * 4;
                                std::fprintf(stderr, "portrait: console over it, well open %d, clear %zu of %zu, centre rgba %u %u %u %u\n", int(well_open), clear, total,
                                             unsigned(console_tex_.rgba[cc]), unsigned(console_tex_.rgba[cc + 1]), unsigned(console_tex_.rgba[cc + 2]), unsigned(console_tex_.rgba[cc + 3]));
                            }
                            if (well_open)
                                for (int dy = std::max(coy, req.rect.y0); dy < std::min(int(view_h), req.rect.y1); ++dy)
                                    for (int dx = std::max(0, req.rect.x0); dx < std::min(int(view_w), req.rect.x1); ++dx) {
                                        const std::size_t ci = (std::size_t(dy - coy) * view_w + std::size_t(dx)) * 4;
                                        const std::size_t oi = (std::size_t(dy) * view_w + std::size_t(dx)) * 4;
                                        const unsigned ca = console_tex_.rgba[ci + 3];
                                        if (!ca) continue;
                                        int far = 0;
                                        for (int k = 0; k < 3; ++k) far = std::max(far, std::abs(int(console_tex_.rgba[ci + k]) - well_rgb[k]));
                                        if (far <= 10) continue;
                                        for (int k = 0; k < 3; ++k) out[oi + k] = std::uint8_t((console_tex_.rgba[ci + k] * ca + out[oi + k] * (255 - ca)) / 255);
                                    }
                        }
                    }
                }
                hud_portrait_ = std::move(req);
            }
        }
    };
    if (!unit_pane && cfg_.human_player >= 0 && selection_.size() > 1 && selection_[0] < world_.units().size() &&
        world_.unit(selection_[0]).alive && world_.unit(selection_[0]).player == cfg_.human_player &&
        MatchResult::banner(outcome_)[0] == '\0' && !std::getenv("OMC_NO_GROUP_PORTRAIT"))
        draw_portrait(world_.unit(selection_[0]).catalog_id);
    ov_lap.lap("ov.portrait");
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
        if (!wire_name.empty() && !unit_pane)
            draw_text_rgba(out, view_w, view_h, px0 + 3, py0 + 2, wire_name.c_str(), 1, 200, 208, 220);
        if (console_panel.authored) {
            const HudAuthoredRects* ar = console_panel.authored;
            const LabelLook vl = resolve_label_style(hud_style_set(), ar->style_vital, human_race());
            struct Row { const HudAuthoredRects::R& rect; double cur, max; };
            const Row rows[3] = {{ar->vital_shield, su.shields.to_double(), su.shields_max.to_double()},
                                 {ar->vital_life, su.hp.to_double(), su.hp_max.to_double()},
                                 {ar->vital_energy, su.energy.to_double(), su.energy_max.to_double()}};
            const bool vital_shuffle = !std::getenv("OMC_VITAL_ROWS_FIXED");
            int vital_slot = 0;
            for (const Row& row : rows) {
                if (row.max <= 0.0 || !row.rect.ok()) continue;
                const HudAuthoredRects::R& place = (vital_shuffle && rows[vital_slot].rect.ok()) ? rows[vital_slot].rect : row.rect;
                ++vital_slot;
                const double vsc = double(view_h) / double(kRefHeight);
                const HudRect lr{int(place.x0 * vsc + 0.5), int(place.y0 * vsc + 0.5),
                                 int(place.x1 * vsc + 0.5), int(place.y1 * vsc + 0.5)};
                if (lr.w() <= 0 || lr.h() <= 0) continue;
                char vt[48];
                static const bool vital_no_gap = std::getenv("OMC_VITAL_NO_GAP") != nullptr;
                std::snprintf(vt, sizeof(vt), vital_no_gap ? "%d/%d" : "%d/ %d", int(row.cur + 0.5), int(row.max + 0.5));
                static const bool vital_whole_px = std::getenv("OMC_VITAL_WHOLE_PX") != nullptr;
                draw::set_forced_font_px(vital_whole_px ? int(14.0 * vsc + 0.5) : render::OverlayFont::fractional(14.0 * vsc));
                const FaceScope vital_face(style_face(vl.font));
                const int vw = text_w_rgba(vt, 1);
                const int vx = (!vl.set || vl.centred) ? lr.x0 + (lr.w() - vw) / 2 : lr.x0;
                const int vy = lr.y0 + std::max(0, (lr.h() - ui::kFontH) / 2);
                if (std::getenv("OMC_LABEL_FACE_TRACE") && &row == &rows[1])
                    std::fprintf(stderr, "[labelface] vital rect %d %d %d %d text y %d px %d\n", lr.x0, lr.y0, lr.x1, lr.y1, vy, int(14.0 * vsc + 0.5));
                int vr = vl.set ? vl.r : 220, vg = vl.set ? vl.g : 226, vb = vl.set ? vl.b : 235;
                if (&row == &rows[1]) {
                    const double lf = row.max > 0.0 ? row.cur / row.max : 1.0;
                    if (std::getenv("OMC_VITAL_ROWS_FIXED")) hc(float(lf), vr, vg, vb);
                    else if (lf >= 0.75) { vr = 22; vg = 229; vb = 0; }
                    else if (lf >= 0.5) { vr = 229; vg = 221; vb = 0; }
                    else if (lf >= 0.25) { vr = 229; vg = 129; vb = 0; }
                    else { vr = 208; vg = 34; vb = 0; }
                }
                draw::text_shadow_off() = !vital_whole_px;
                draw_text_rgba(out, view_w, view_h, vx, vy, vt, 1, vr, vg, vb);
                draw::text_shadow_off() = false;
                draw::set_forced_font_px(forced_px);
            }
        }

        if (unit_pane) {
            const double sc = double(view_h) / double(kRefHeight);
            const HudAuthoredRects::R& ip = console_panel.authored->info;
            HudUnitPane pane;
            pane.name = wire_name;
            pane.pane = {int(ip.x0 * sc + 0.5), int(ip.y0 * sc + 0.5), int(ip.x1 * sc + 0.5), int(ip.y1 * sc + 0.5)};
            const int name_y = int((ip.y0 + 10.0f) * sc + 0.5);
            const LabelLook nl = resolve_label_style(hud_style_set(), hud_authored_
                                                         ? hud_authored_->style_unit_name
                                                         : std::string(),
                                                     human_race());
            const int unit_x0 = int((ip.x1 - 402.0f) * sc + 0.5), unit_x1 = pane.pane.x1;
            draw::set_forced_font_px(int(24.0 * sc + 0.5));
            {
                static const bool kNameFaceOld = std::getenv("OMC_NAME_LABEL_FACE_OLD") != nullptr;
                const FaceScope name_face(kNameFaceOld ? nullptr : style_face(nl.font));
                const ShadowScope name_shadow(nl, sc);
                const int name_w = text_w_rgba(pane.name, 1);
                const int name_x = std::max(unit_x0, (unit_x0 + unit_x1) / 2 - name_w / 2);
                draw_text_rgba(out, view_w, view_h, name_x, name_y, pane.name.c_str(), 1,
                               nl.set ? nl.r : 230, nl.set ? nl.g : 232, nl.set ? nl.b : 236);
            }
            draw::set_forced_font_px(forced_px);
            const float bar_top = ip.y0 + 10.0f + 26.0f;
            pane.life_bar = {int(ip.x0 * sc + 0.5), int(bar_top * sc + 0.5), int((ip.x0 + 128.0f) * sc + 0.5), int((bar_top + 16.0f) * sc + 0.5)};
            pane.life_frac = hpf;
            if (std::getenv("OMC_PANE_LIFE_BAR")) {
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
            const int armor_v = int(su.armor.to_double() + 0.5);
            const int armor_l = int(su.armor_level.to_double() + 0.5);
            std::string armor_word = "Armor";
            if (cat_)
                if (auto an = cat_->unit_life_armor_name(su.catalog_id)) armor_word = std::move(*an);
            if (armor_l > 0)
                std::snprintf(info, sizeof info, "%s %d (+%d)", armor_word.c_str(), armor_v, armor_l);
            else
                std::snprintf(info, sizeof info, "%s %d", armor_word.c_str(), armor_v);
            pane.info = info;
            const LabelLook fl = resolve_label_style(
                hud_style_set(), hud_authored_ ? hud_authored_->style_unit_info : std::string(),
                human_race());
            if (std::getenv("OMC_PANE_LIFE_BAR"))
                draw_text_rgba(out, view_w, view_h, pane.life_bar.x1 + 8, pane.life_bar.y0,
                               pane.info.c_str(), 1, fl.set ? fl.r : 210, fl.set ? fl.g : 214,
                               fl.set ? fl.b : 220);
            const bool queue_pane = !pane_queue(selection_[0]).empty();
            if (!(su.attributes & sim::kAttrStructure) && cat_ && !queue_pane) {
                const std::string* ks = cat_->strings().find("e_gameUIStringKillsColon");
                const std::string* rs = cat_->strings().find("e_gameUIStringRankColon");
                const char* rsfx = race == 1 ? "Prot" : race == 2 ? "Zerg" : "Terr";
                const std::string* rn = app::kill_rank_name(*cat_, su.catalog_id, content.lead_kills, rsfx);
                const std::string lab[2] = {ks ? *ks : std::string("Kills: "), rs ? *rs : std::string("Rank: ")};
                const std::string val[2] = {std::to_string(content.lead_kills), rn ? *rn : std::string()};
                const FaceScope info_scope(style_face(fl.font));
                const ShadowScope info_shadow(fl, sc);
                const int ifpx = std::max(6, int((fl.height > 0 ? double(fl.height) : 16.0) * sc + 0.5));
                draw::set_forced_font_px(ifpx);
                const int bottom = int((ip.y0 + 110.0f) * sc + 0.5);
                for (int li = 0; li < 2; ++li) {
                    if (li == 1 && val[1].empty()) continue;
                    const int lw = text_w_rgba(lab[li], 1), vw = text_w_rgba(val[li], 1);
                    const int lx = (unit_x0 + unit_x1) / 2 - (lw + vw) / 2;
                    static const bool info_low = std::getenv("OMC_PANE_INFO_LOW") != nullptr;
                    const int ly = bottom - ifpx - (info_low ? 2 : 2 + int(5.0 * sc + 0.5)) - (li == 0 ? int(20.0 * sc + 0.5) : 0);
                    draw_text_rgba(out, view_w, view_h, lx, ly, lab[li].c_str(), 1, race == 1 ? 110 : race == 2 ? 245 : 155,
                                   race == 1 ? 170 : race == 2 ? 140 : 255, race == 1 ? 255 : race == 2 ? 70 : 190);
                    const int vg = info_low ? 236 : 255;
                    draw_text_rgba(out, view_w, view_h, lx + lw, ly, val[li].c_str(), 1, vg, info_low ? 238 : 255, info_low ? 240 : 255);
                }
                draw::set_forced_font_px(forced_px);
            }
            static const std::pair<std::uint32_t, const char*> kAttr[] = {
                {sim::kAttrLight, "Light"}, {sim::kAttrArmored, "Armored"}, {sim::kAttrBiological, "Biological"},
                {sim::kAttrMechanical, "Mechanical"}, {sim::kAttrMassive, "Massive"}, {sim::kAttrPsionic, "Psionic"},
                {sim::kAttrStructure, "Structure"}};
            for (const auto& [bit, label] : kAttr)
                if (su.attributes & bit) {
                    if (!pane.type.empty()) pane.type += " - ";
                    pane.type += label;
                }
            if (!pane.type.empty() && !queue_pane) {
                const LabelLook tl = resolve_label_style(
                    hud_style_set(), hud_authored_ ? hud_authored_->style_unit_type : std::string(),
                    human_race());
                draw::set_forced_font_px(int(18.0 * sc + 0.5));
                const auto* body_face = draw::active_font();
                if (const render::OverlayFont* hf = style_face(tl.font.empty() ? std::string("UI/Fonts/Eurostile-Bol.otf") : tl.font)) draw::set_active_font(hf);
                const ShadowScope type_shadow(tl, sc);
                const int type_w = text_w_rgba(pane.type, 1);
                const int type_x = (tl.set && tl.centred) ? std::max(unit_x0, (unit_x0 + unit_x1) / 2 - type_w / 2) : unit_x0;
                draw_text_rgba(out, view_w, view_h, type_x, int((ip.y0 + 176.0f) * sc + 0.5),
                               pane.type.c_str(), 1, tl.set ? tl.r : 190, tl.set ? tl.g : 196,
                               tl.set ? tl.b : 208);
                draw::set_active_font(body_face);
                draw::set_forced_font_px(forced_px);
            }
            hud_unit_pane_ = std::move(pane);
        }
        if (std::getenv("OMC_PORTRAIT_TRACE"))
            std::fprintf(stderr, "portrait: unit_pane %d models %d store %d rect ok %d (%.1f %.1f %.1f %.1f) renderer %d unit %s\n", int(unit_pane),
                         int(models_ != nullptr), int(ui_store_ != nullptr), int(console_panel.authored && console_panel.authored->portrait.ok()),
                         console_panel.authored ? double(console_panel.authored->portrait.x0) : 0.0, console_panel.authored ? double(console_panel.authored->portrait.y0) : 0.0,
                         console_panel.authored ? double(console_panel.authored->portrait.x1) : 0.0, console_panel.authored ? double(console_panel.authored->portrait.y1) : 0.0,
                         int(bool(portrait_renderer_)), su.catalog_id.c_str());
        if (unit_pane) draw_portrait(su.catalog_id);

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
            const WireMask& msk = wire_mask(wire_mask_cache_, wm_model, MW, MH);
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
            std::string subject;
            if (world_.unit(sel).constructing) {
                frac = float(world_.build_progress(sel).to_double());
                subject = world_.unit(sel).catalog_id;
            } else if (world_.pending_progress(sel, f)) {
                if (std::getenv("OMC_QUEUE_PROGRESS_PANE")) {
                    frac = float(f.to_double());
                    const std::vector<sim::World::QueueSlotView> q = world_.queue_view(sel);
                    if (!q.empty()) subject = q.front().catalog_id;
                }
            } else if (std::getenv("OMC_RESEARCH_PANE_OLD") && world_.research_progress(cfg_.human_player, f, rid)) {
                frac = float(f.to_double());
                subject = rid;
            }
            if (frac >= 0.0f) {
                frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
                static const int kBorder[3][3] = {{40, 125, 75}, {35, 125, 255}, {230, 95, 5}};
                static const int kFill[3][3] = {{155, 255, 190}, {110, 170, 255}, {245, 140, 70}};
                const int ri = race_index(cat_, world_.unit(sel).catalog_id,
                                          sel < layout_.unit_type.size() ? layout_.unit_type[sel]
                                                                         : std::string());
                const int rc = ri < 0 || ri > 2 ? 0 : ri;
                int border[3] = {kBorder[rc][0], kBorder[rc][1], kBorder[rc][2]};
                int fill[3] = {kFill[rc][0], kFill[rc][1], kFill[rc][2]};
                if (ui_store_) {
                    hud_console_color(*ui_store_, rc, "StandardProgressBorder", border);
                    hud_console_color(*ui_store_, rc, "StandardProgressFill", fill);
                }
                for (int y = pb.y0; y < pb.y1 && y < int(view_h); ++y)
                    for (int x = pb.x0; x < pb.x1 && x < int(view_w); ++x) {
                        if (x < 0 || y < 0) continue;
                        const std::size_t i = (std::size_t(y) * view_w + x) * 4;
                        out[i] = std::uint8_t(border[0]);
                        out[i + 1] = std::uint8_t(border[1]);
                        out[i + 2] = std::uint8_t(border[2]);
                        out[i + 3] = 255;
                    }
                if (ui_store_ && cat_ && !subject.empty()) {
                    const HudRect bt = hud_progress_button_rect(view_w, view_h, &console_panel);
                    if (bt.w() > 0 && bt.h() > 0)
                        if (auto ic = cat_->button_icon(subject); ic && !ic->empty())
                            blit_icon(icons_.get(*ui_store_, ic->c_str()), bt.x0, bt.y0, bt.x1, bt.y1);
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
                        out[i] = std::uint8_t(fill[0]);
                        out[i + 1] = std::uint8_t(fill[1]);
                        out[i + 2] = std::uint8_t(fill[2]);
                        out[i + 3] = 255;
                    }
            }
        }
    }

    ov_lap.lap("ov.unit_pane");
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
            std::string path = cat_->resolve_asset(std::string(kTokens[i]) + suffix);
            if (i < 2 && !std::getenv("OMC_RESOURCE_ICON_PLATES")) {
                const std::string bare = cat_->resolve_asset(std::string(i == 0 ? "@@UI/ResourceIconNoBackground0" : "@@UI/ResourceIconNoBackground1") + suffix);
                if (!bare.empty() && bare[0] != '@' && icons_.get(*ui_store_, bare.c_str()).ok()) path = bare;
            }
            if (path.empty() || path[0] == '@') continue;
            blit_icon(icons_.get(*ui_store_, path.c_str()), int(q.x0 * sc + 0.5),
                      int(q.y0 * sc + 0.5), int(q.x1 * sc + 0.5), int(q.y1 * sc + 0.5));
        }
    }

    if (!harvest_labels.empty() && ui_store_ && cat_) {
        const double hsc = double(view_h) / double(kRefHeight);
        const std::string hsfx = ui::layout_race_suffix(console_panel.race);
        const int saved_px = draw::forced_font_px();
        const int fpx = std::max(6, int(16.0 * hsc + 0.5));
        draw::set_forced_font_px(fpx);
        const LabelLook tag_look = resolve_label_style(hud_style_set(), "TextTag", int(console_panel.race));
        const FaceScope harvest_face(OMC_ENV_ONCE("OMC_HARVEST_LABEL_DEFAULT_FACE")
                                         ? nullptr
                                         : style_face(tag_look.set && !tag_look.font.empty() ? tag_look.font : std::string("UI/Fonts/EurostileExt-Reg.otf")));
        const ShadowScope harvest_shadow(tag_look, hsc);
        for (const HarvestLabel& hl : harvest_labels) {
            const char* key = hl.res == 0 ? "UI/HarvesterCountMinerals" : "UI/HarvesterCountVespene";
            std::string text = "Workers: %current%/%ideal%", icon_ref;
            std::size_t value_at = std::string::npos;
            if (const std::string* s = cat_->strings().find(key)) {
                text.clear();
                for (std::size_t i = 0; i < s->size();) {
                    if ((*s)[i] == '<') {
                        const std::size_t e = s->find('>', i);
                        if (e == std::string::npos) break;
                        const std::string tag = s->substr(i, e - i + 1);
                        if (value_at == std::string::npos && tag.compare(0, 3, "<c ") == 0 &&
                            tag.find("%colorValue%") != std::string::npos)
                            value_at = text.size();
                        const std::size_t p = tag.find("path=\"");
                        if (tag.compare(0, 4, "<img") == 0 && p != std::string::npos) {
                            const std::size_t q = tag.find('"', p + 6);
                            if (q != std::string::npos) icon_ref = tag.substr(p + 6, q - p - 6);
                        }
                        i = e + 1;
                    } else {
                        text.push_back((*s)[i++]);
                    }
                }
            }
            auto subst = [&](const char* tok, int v) {
                const std::size_t p = text.find(tok);
                if (p != std::string::npos) text.replace(p, std::strlen(tok), std::to_string(v));
            };
            subst("%current%", hl.cur);
            subst("%ideal%", hl.ideal);
            while (!text.empty() && text.front() == ' ') {
                text.erase(text.begin());
                if (value_at != std::string::npos && value_at > 0) --value_at;
            }
            static const bool kHarvestColorOld = std::getenv("OMC_HARVEST_COLOR_OLD") != nullptr;
            if (kHarvestColorOld || value_at > text.size()) value_at = text.size();
            const std::string prefix = text.substr(0, value_at), value = text.substr(value_at);
            const bool over = hl.cur > hl.ideal && !kHarvestColorOld;
            const std::uint8_t vr = over ? 167 : 232, vg = over ? 57 : 232, vb = over ? 78 : 232;
            std::string icon_path;
            if (!icon_ref.empty()) {
                icon_path = cat_->resolve_asset(icon_ref, hsfx);
                if (!icon_path.empty() && icon_path[0] == '@') icon_path.clear();
            }
            const int isz = icon_path.empty() ? 0 : int(20.0 * hsc + 0.5);
            const int gap = isz > 0 ? int(6.0 * hsc + 0.5) : 0, pad = int(6.0 * hsc + 0.5);
            const int tw = text_w_rgba(text, 1);
            const int w = pad + isz + gap + tw + pad, h = std::max(isz, fpx) + int(8.0 * hsc + 0.5);
            const int x0 = hl.sx - w / 2, y0 = hl.sy + h / 2;
            static const bool kPlateOpaque = std::getenv("OMC_HARVEST_PLATE_OPAQUE") != nullptr;
            for (int y = std::max(0, y0); y < std::min(int(view_h), y0 + h); ++y)
                for (int x = std::max(0, x0); x < std::min(int(view_w), x0 + w); ++x) {
                    if (hud_covers_point(view_w, view_h, &console_panel, float(x), float(y))) continue;
                    const std::size_t di = (std::size_t(y) * view_w + std::size_t(x)) * 4;
                    if (out[di + 3] == 0 && !kPlateOpaque) {
                        out[di] = out[di + 1] = out[di + 2] = 0;
                        out[di + 3] = 102;
                        continue;
                    }
                    for (int c = 0; c < 3; ++c) out[di + c] = std::uint8_t(out[di + c] / 2);
                    out[di + 3] = 255;
                }
            HudHarvestLabel rec;
            rec.resource = hl.res;
            rec.current = hl.cur;
            rec.ideal = hl.ideal;
            rec.prefix = prefix;
            rec.value = value;
            rec.value_rgb[0] = vr;
            rec.value_rgb[1] = vg;
            rec.value_rgb[2] = vb;
            rec.rect = {x0, y0, x0 + w, y0 + h};
            if (y0 >= 0 && y0 + h < int(view_h) && !hud_covers_point(view_w, view_h, &console_panel, float(hl.sx), float(y0 + h))) {
                if (isz > 0)
                    blit_icon(icons_.get(*ui_store_, icon_path.c_str()), x0 + pad, y0 + (h - isz) / 2, x0 + pad + isz, y0 + (h - isz) / 2 + isz);
                const int tx = x0 + pad + isz + gap, ty = y0 + (h - fpx) / 2 - int(2.5 * hsc + 0.5);
                if (!prefix.empty()) draw_text_rgba(out, view_w, view_h, tx, ty, prefix, 1, 232, 232, 232);
                if (!value.empty())
                    draw_text_rgba(out, view_w, view_h, tx + (prefix.empty() ? 0 : text_w_rgba(prefix, 1)), ty, value, 1, vr, vg, vb);
                rec.drawn = true;
            }
            hud_harvest_labels_.push_back(std::move(rec));
        }
        draw::set_forced_font_px(saved_px);
    }

    ov_lap.lap("ov.resources");
    bool console_cell_art = false;
    refresh_hud_buttons(view_w, view_h);
    if (cfg_.human_player >= 0 && ui_store_ && console_panel.authored && !std::getenv("OMC_NO_CONSOLE_WIDGET_ART")) {
        const int hr = human_race();
        const char* skin = hr == 1 ? "defaultprotoss" : hr == 2 ? "defaultzerg" : "defaultterran";
        const char* race3 = hr == 1 ? "prot" : hr == 2 ? "zerg" : "terr";
        const double sc = double(view_h) / double(kRefHeight);
        const double canvas_w = double(kRefHeight) * double(view_w) / double(view_h);
        auto skin_tex = [&](const char* part) {
            return std::string("Assets/Textures/ui_console_") + part + "_" + skin + ".dds";
        };
        auto px = [&](double pt) { return int(pt * sc + 0.5); };
        auto style_font = [&](const char* style, const char* fallback) {
            const LabelLook l = resolve_label_style(hud_style_set(), style, hr);
            return style_face(l.set && !l.font.empty() ? l.font : std::string(fallback));
        };
        console_cell_art = icons_.get(*ui_store_, skin_tex("commandbutton_normal").c_str()).ok();
        {
            const std::string p = skin_tex("minimapbutton_normal");
            const char* kIcons[3] = {"Assets/Textures/btn-minimap-ping.dds", "Assets/Textures/btn-minimap-toggleterrain.dds",
                                     "Assets/Textures/btn-minimap-toggleallies.dds"};
            const HudButtonId kIds[3] = {HudButtonId::MinimapPing, HudButtonId::MinimapTerrain, HudButtonId::MinimapColor};
            const double kTop[3] = {1200.0 - 229.0 - 42.0, 1200.0 - 178.0 - 42.0, 1200.0 - 127.0 - 42.0};
            const double bx = 395.0 - 9.0 - 42.0;
            for (int i = 0; i < 3; ++i) {
                const HudButton* hb = hud_buttons_.find(kIds[i]);
                if (!hb) continue;
                const IconAtlas::Icon& st = icons_.get(*ui_store_, skin_tex((std::string("minimapbutton_") + hud_look_suffix(hud_buttons_.look(*hb))).c_str()).c_str());
                blit_icon(st.ok() ? st : icons_.get(*ui_store_, p.c_str()), hb->rect.x0, hb->rect.y0, hb->rect.x1, hb->rect.y1);
                blit_icon_tinted(icons_.get(*ui_store_, kIcons[i]), px(bx - 3.0), px(kTop[i] - 3.0), px(bx + 45.0), px(kTop[i] + 45.0),
                                 40.0f / 255.0f, 125.0f / 255.0f, 75.0f / 255.0f);
            }
        }
        {
            const std::string bg = skin_tex("helperbutton_background");
            const std::string icon_w = std::string("Assets/Textures/ui_console_helperbutton_icon_worker_") + race3 + ".dds";
            const std::string icon_a = std::string("Assets/Textures/ui_console_helperbutton_icon_army_") + race3 + ".dds";
            const std::string wsfx = ui::layout_race_suffix(hr);
            std::string icon_g = cat_ ? cat_->resolve_asset("@@@UI/WarpButtonIcon", wsfx, wsfx) : std::string();
            if ((icon_g.empty() || icon_g[0] == '@') && cat_) icon_g = cat_->resolve_asset("@@UI/WarpButtonIcon", wsfx);
            if (icon_g.empty() || icon_g[0] == '@') icon_g = "Assets/Textures/ui_console_helperbutton_icon_warpgate_prot.dds";
            for (char& ch : icon_g)
                if (ch == '\\') ch = '/';
            const double kLeft[3] = {0.0, 87.0, 174.0};
            const std::string* kIcon[3] = {&icon_w, &icon_a, &icon_g};
            const char* kKey[3] = {"F1", "F2", ""};
            const double bottom = 1200.0 - 327.0 + 25.0;
            const float kIconTint[3][3] = {{0.0f, 200.0f, 120.0f}, {100.0f, 180.0f, 255.0f}, {255.0f, 120.0f, 20.0f}};
            const float kCountTint[3][3] = {{60.0f, 255.0f, 150.0f}, {40.0f, 100.0f, 255.0f}, {150.0f, 100.0f, 50.0f}};
            const int ri = hr == 1 ? 1 : hr == 2 ? 2 : 0;
            const int kCount[3] = {content.idle_workers, content.army_units, content.warp_gates_ready};
            const IconAtlas::Icon& counter = icons_.get(*ui_store_, "Assets/Textures/ui_console_menubutton_counter.dds");
            static const bool kWarpButtonArtOld = std::getenv("OMC_WARP_BUTTON_ART_OLD") != nullptr;
            const int helpers = kWarpButtonArtOld || content.warp_gates_ready <= 0 ? 2 : 3;
            for (int i = 0; i < helpers; ++i) {
                const bool on = kCount[i] > 0;
                const double top = bottom - 72.0;
                blit_icon(icons_.get(*ui_store_, bg.c_str()), px(kLeft[i]), px(bottom - 62.0), px(kLeft[i] + 92.0), px(bottom));
                const float ig = 98.0f / 255.0f;
                blit_icon_tinted(icons_.get(*ui_store_, kIcon[i]->c_str()), px(kLeft[i] + 9.0), px(bottom - 53.0), px(kLeft[i] + 81.0),
                                 px(bottom - 11.0), on ? kIconTint[ri][0] / 255.0f : ig, on ? kIconTint[ri][1] / 255.0f : ig,
                                 on ? kIconTint[ri][2] / 255.0f : ig);
                if (i == 0 && on && !std::getenv("OMC_NO_HELPER_GLOW")) {
                    const float gl = hud_helper_glow(hud_ui_.selection.glow_t);
                    blit_icon_add(icons_.get(*ui_store_, bg.c_str()), px(kLeft[i]), px(bottom - 62.0), px(kLeft[i] + 92.0), px(bottom), gl, 1.0f, 1.0f, 1.0f);
                    blit_icon_add(icons_.get(*ui_store_, kIcon[i]->c_str()), px(kLeft[i] + 9.0), px(bottom - 53.0), px(kLeft[i] + 81.0), px(bottom - 11.0),
                                  gl * 200.0f / 255.0f, kIconTint[ri][0] / 255.0f, kIconTint[ri][1] / 255.0f, kIconTint[ri][2] / 255.0f);
                }
                if (kKey[i][0]) draw_text_rgba(out, view_w, view_h, px(kLeft[i] + 50.0), px(bottom - 31.0), kKey[i], 1, 235, 240, 236);
                if (counter.ok()) {
                    const int cx0 = px(kLeft[i] + 48.0), cy0 = px(top), cx1 = px(kLeft[i] + 92.0), cy1 = px(top + 28.0);
                    static const bool wide_corner = std::getenv("OMC_HELPER_COUNT_WIDE_CORNER") != nullptr;
                    const int cs = 8, cd = std::max(1, px(8.0));
                    if (!wide_corner) {
                        const float dk = 0.5f;
                        blit_nine(counter, (kLeft[i] + 48.0) * sc, top * sc, (kLeft[i] + 92.0) * sc, (top + 28.0) * sc, double(counter.w) * 0.5 * double(view_h) / 2160.0, on ? kCountTint[ri][0] / 255.0f : dk, on ? kCountTint[ri][1] / 255.0f : dk,
                                  on ? kCountTint[ri][2] / 255.0f : dk, on ? 0.0f : 5.0f);
                    } else
                    for (int y = cy0; y < cy1; ++y)
                        for (int x = cx0; x < cx1; ++x) {
                            if (x < 0 || y < 0 || x >= int(view_w) || y >= int(view_h)) continue;
                            const int lx = x - cx0, ly = y - cy0, w = cx1 - cx0, h = cy1 - cy0;
                            const int sx = lx < cd ? lx * cs / cd : lx >= w - cd ? int(counter.w) - 1 - (w - 1 - lx) * cs / cd : int(counter.w) / 2;
                            const int sy = ly < cd ? ly * cs / cd : ly >= h - cd ? int(counter.h) - 1 - (h - 1 - ly) * cs / cd : int(counter.h) / 2;
                            const std::size_t si = (std::size_t(sy) * counter.w + std::size_t(sx)) * 4;
                            const float a = counter.rgba[si + 3] / 255.0f;
                            if (a <= 0.0f) continue;
                            const std::size_t di = (std::size_t(y) * view_w + std::size_t(x)) * 4;
                            for (int c = 0; c < 3; ++c) {
                                const float src = on ? float(counter.rgba[si + c]) * kCountTint[ri][c] / 255.0f : float(counter.rgba[si + c]) * 0.24f + 5.0f;
                                out[di + c] = std::uint8_t(src * a + out[di + c] * (1.0f - a));
                            }
                            const float da = out[di + 3] / 255.0f;
                            out[di + 3] = std::uint8_t((a + da * (1.0f - a)) * 255.0f + 0.5f);
                        }
                    const std::string n = std::to_string(kCount[i]);
                    const int fpx = std::max(6, px(16.0));
                    const int saved_px = draw::forced_font_px();
                    draw::set_forced_font_px(fpx);
                    const FaceScope count_face(std::getenv("OMC_HELPER_COUNT_DEFAULT_FACE") ? nullptr : style_font("IdleButtonCharge", "UI/Fonts/Eurostile-Reg.otf"));
                    const ShadowScope count_shadow(resolve_label_style(hud_style_set(), "IdleButtonCharge", hr), sc);
                    const int nw = text_w_rgba(n, 1);
                    const int g = on ? 235 : 112;
                    const int dxy = wide_corner ? 1 : -1;
                    draw_text_rgba(out, view_w, view_h, (cx0 + cx1) / 2 + dxy - nw / 2, (cy0 + cy1) / 2 + dxy - fpx / 2, n.c_str(), 1, g, g, g);
                    draw::set_forced_font_px(saved_px);
                }
            }
        }
        {
            const std::string clock = hud_clock_text(content.game_seconds);
            const IconAtlas::Icon& plate = icons_.get(*ui_store_, skin_tex("timer_background").c_str());
            if (!clock.empty()) {
                const int fpx = std::max(6, px(20.0));
                const int saved_px = draw::forced_font_px();
                draw::set_forced_font_px(fpx);
                const FaceScope clock_face(std::getenv("OMC_CLOCK_DEFAULT_FACE") ? nullptr : style_font("MissionTimePanel", "UI/Fonts/EurostileExt-Reg.otf"));
                const ShadowScope clock_shadow(resolve_label_style(hud_style_set(), "MissionTimePanel", hr), sc);
                const int tw_digits = text_w_rgba(clock, 1);
                const int tw = tw_digits + (std::getenv("OMC_CLOCK_LABEL_TIGHT") ? 0 : int(8.0 * sc + 0.5));
                if (std::getenv("OMC_ENV_TRACE")) {
                    static bool once = false;
                    if (!once) {
                        once = true;
                        std::fprintf(stderr, "[clock] '%s' digits %d px, space %d px, em %d px\n", clock.c_str(), tw_digits, text_w_rgba(" ", 1), fpx);
                    }
                }
                const int x0 = px(290.0), y0 = px(1200.0 - 302.0 - 44.0), y1 = px(1200.0 - 302.0);
                const int tx = px(305.0);
                const int x1 = tx + tw + px(10.0);
                if (plate.ok() && plate.w >= 2 && plate.h >= 2) {
                    const int w = x1 - x0, h = y1 - y0;
                    const int half_src = int(plate.w) / 2, half_h = int(plate.h) / 2;
                    const int cap = std::min(px(double(half_src)), w / 2);
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x) {
                            const int dx = x0 + x, dy = y0 + y;
                            if (dx < 0 || dy < 0 || dx >= int(view_w) || dy >= int(view_h)) continue;
                            int sx = 0, sy = y * half_h / h;
                            if (x < cap) sx = int(double(x) / sc);
                            else if (x >= w - cap) sx = int(plate.w) - 1 - int(double(w - 1 - x) / sc);
                            else { sx = half_src; sy += half_h; }
                            sx = std::max(0, std::min(int(plate.w) - 1, sx));
                            const std::size_t si = (std::size_t(sy) * plate.w + std::size_t(sx)) * 4;
                            const float a = plate.rgba[si + 3] / 255.0f;
                            if (a <= 0.0f) continue;
                            const std::size_t di = (std::size_t(dy) * view_w + std::size_t(dx)) * 4;
                            for (int c = 0; c < 3; ++c) out[di + c] = std::uint8_t(plate.rgba[si + c] * a + out[di + c] * (1.0f - a));
                            const float da = out[di + 3] / 255.0f;
                            out[di + 3] = std::uint8_t((a + da * (1.0f - a)) * 255.0f + 0.5f);
                        }
                }
                const int lr = hr == 1 ? 110 : hr == 2 ? 245 : 155, lg = hr == 1 ? 170 : hr == 2 ? 140 : 255, lb = hr == 1 ? 255 : hr == 2 ? 70 : 190;
                const int ink_dx = std::getenv("OMC_CLOCK_LABEL_TIGHT") ? 0 : int(2.0 * sc + 0.5);
                draw_text_rgba(out, view_w, view_h, tx + ink_dx, (px(1200.0 - 302.0 - 37.0) + px(1200.0 - 302.0 - 4.0)) / 2 - fpx / 2 - px(3.0), clock.c_str(), 1, lr, lg, lb);
                draw::set_forced_font_px(saved_px);
            }
        }
        if (!std::getenv("OMC_NO_ALLIANCE_BUTTON")) {
            const int ar = human_race();
            const char* suf = ar == 1 ? "_protoss" : ar == 2 ? "_zerg" : "";
            const HudButton* hb = hud_buttons_.find(HudButtonId::Alliance);
            const HudButton* hov = hud_buttons_.hovered();
            const bool over = hb && hov && hov->id == HudButtonId::Alliance;
            const std::string ap = std::string("Assets/Textures/ui_alliance_button_") + (over ? "normaloverpressedover" : "normalpressed") + suf + ".dds";
            const IconAtlas::Icon& both = icons_.get(*ui_store_, ap.c_str());
            if (hb && both.ok() && both.h >= 2) {
                IconAtlas::Icon half;
                half.w = both.w;
                half.h = both.h / 2;
                const std::ptrdiff_t n = std::ptrdiff_t(std::size_t(half.w) * half.h * 4);
                const std::ptrdiff_t off = hb->toggled ? n : 0;
                half.rgba.assign(both.rgba.begin() + off, both.rgba.begin() + off + n);
                blit_icon(half, hb->rect.x0, hb->rect.y0, hb->rect.x1, hb->rect.y1);
            }
        }
        {
            const std::string p = skin_tex("menubar_background");
            const double bottom = 1200.0 - 267.0;
            const double bx0 = canvas_w - 241.0, by0 = bottom - 56.0;
            blit_icon(icons_.get(*ui_store_, p.c_str()), px(bx0), px(by0), px(canvas_w), px(bottom));
            const std::string nb = skin_tex("menubutton_normal");
            const double kBtnX[4] = {bx0 + 9.0, bx0 + 59.0, bx0 + 110.0, canvas_w - 10.0 - 70.0};
            const double kBtnW[4] = {36.0, 36.0, 36.0, 70.0};
            const char* kBtnIcon[3] = {"Assets/Textures/ui_console_icon_help.dds", "Assets/Textures/ui_console_icon_chat.dds",
                                       "Assets/Textures/ui_console_icon_social.dds"};
            for (int i = 0; i < 4; ++i) {
                const double x0 = kBtnX[i], y0 = by0 + 9.0;
                const HudButtonId kMenuIds[4] = {HudButtonId::MenuHelp, HudButtonId::MenuChat, HudButtonId::MenuSocial, HudButtonId::MenuMenu};
                const HudButton* hb = hud_buttons_.find(kMenuIds[i]);
                const IconAtlas::Icon& mst = icons_.get(*ui_store_, skin_tex((std::string("menubutton_") + hud_look_suffix(hb ? hud_buttons_.look(*hb) : HudLook::Normal)).c_str()).c_str());
                blit_icon(mst.ok() ? mst : icons_.get(*ui_store_, nb.c_str()), px(x0), px(y0), px(x0 + kBtnW[i]), px(y0 + 36.0));
                if (i < 3)
                    blit_icon_tinted(icons_.get(*ui_store_, kBtnIcon[i]), px(x0 + 2.0), px(y0 + 2.0), px(x0 + 34.0), px(y0 + 34.0),
                                     0.0f, 200.0f / 255.0f, 120.0f / 255.0f);
                if (i == 2 && !std::getenv("OMC_NO_SOCIAL_BADGE")) {
                    const IconAtlas::Icon& badge = icons_.get(*ui_store_, "Assets/Textures/ui_console_menubutton_counter.dds");
                    const float kBadgeTint[3][3] = {{60.0f, 255.0f, 150.0f}, {40.0f, 100.0f, 255.0f}, {150.0f, 100.0f, 50.0f}};
                    const int bri = hr == 1 ? 1 : hr == 2 ? 2 : 0;
                    const double ftop = y0 + 36.0 - 51.0;
                    const int cx0 = px(x0 + 50.0 - 2.0 - 28.0), cy0 = px(ftop + 1.0), cx1 = px(x0 + 48.0), cy1 = px(ftop + 26.0);
                    if (badge.ok()) {
                        blit_nine(badge, (x0 + 20.0) * sc, (ftop + 1.0) * sc, (x0 + 48.0) * sc, (ftop + 26.0) * sc, double(badge.w) * 0.5 * double(view_h) / 2160.0, kBadgeTint[bri][0] / 255.0f, kBadgeTint[bri][1] / 255.0f,
                                  kBadgeTint[bri][2] / 255.0f, 0.0f);
                        const int fpx = std::max(6, px(16.0));
                        const int saved_px = draw::forced_font_px();
                        draw::set_forced_font_px(fpx);
                        const FaceScope count_face(style_font("IdleButtonCharge", "UI/Fonts/Eurostile-Reg.otf"));
                        const ShadowScope count_shadow(resolve_label_style(hud_style_set(), "IdleButtonCharge", hr), sc);
                        const int nw = text_w_rgba("0", 1);
                        draw_text_rgba(out, view_w, view_h, (cx0 + cx1) / 2 - 1 - nw / 2, (cy0 + cy1) / 2 - 1 - fpx / 2, "0", 1, 235, 236, 236);
                        draw::set_forced_font_px(saved_px);
                    }
                }
                if (i == 3) {
                    const int fpx = std::max(6, px(16.0));
                    const int saved_px = draw::forced_font_px();
                    draw::set_forced_font_px(fpx);
                    const FaceScope menu_face(std::getenv("OMC_MENU_LABEL_DEFAULT_FACE") ? nullptr : style_font("MenuBarButton", "UI/Fonts/Eurostile-Bol.otf"));
                    const ShadowScope menu_shadow(resolve_label_style(hud_style_set(), "MenuBarButton", hr), sc);
                    const int mw = text_w_rgba("Menu", 1);
                    const int lr = hr == 1 ? 110 : hr == 2 ? 245 : 155, lg = hr == 1 ? 170 : hr == 2 ? 140 : 255, lb = hr == 1 ? 255 : hr == 2 ? 70 : 190;
                    draw_text_rgba(out, view_w, view_h, (px(x0) + px(x0 + kBtnW[i])) / 2 - mw / 2, (px(y0) + px(y0 + 36.0)) / 2 - fpx / 2 - 1, "Menu", 1, lr, lg, lb);
                    draw::set_forced_font_px(saved_px);
                }
            }
        }
    }

    ov_lap.lap("ov.widget_art");
    hud_group_buttons_.clear();
    hud_empty_group_tab_ = HudRect{0, 0, 0, 0};
    if (cfg_.human_player >= 0 && ui_store_ && cat_ && console_panel.authored &&
        console_panel.authored->control_group.ok()) {
        const double sc = double(view_h) / double(kRefHeight);
        const HudAuthoredRects::R& panel = console_panel.authored->control_group;
        const std::string race_sfx = ui::layout_race_suffix(console_panel.race);
        std::string normal =
            cat_->resolve_asset("@@@UI/Game_Console_Buttons_ControlGroup_Normal", race_sfx, race_sfx);
        if (normal.empty() || normal[0] == '@') {
            const int hr2 = human_race();
            const std::string skinned = std::string("Assets/Textures/ui_console_controlgroupbutton_normal_") +
                                        (hr2 == 1 ? "defaultprotoss" : hr2 == 2 ? "defaultzerg" : "defaultterran") + ".dds";
            if (icons_.get(*ui_store_, skinned.c_str()).ok()) normal = skinned;
        }
        if (normal.empty() || normal[0] == '@')
            normal = cat_->resolve_asset("@@UI/ControlGroupButtonNormal", race_sfx);
        const bool have_normal = !normal.empty() && normal[0] != '@';
        const float bw = 82.0f, bh = 64.0f, step = bw - 6.0f;
        float bx = panel.x0;
        const float by = panel.y1 - bh;
        const int order[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0};
        bool any_group = false;
        for (int slot : order)
            if (!groups_.recall(slot).empty()) any_group = true;
        const bool bare_tab = !any_group && !std::getenv("OMC_NO_EMPTY_GROUP_TAB");
        for (int slot : order) {
            const std::vector<std::uint32_t> ids = groups_.recall(slot);
            if (ids.empty() && bare_tab && slot == 1) {
                const int rx0 = int(bx * sc + 0.5), ry0 = int(by * sc + 0.5), rx1 = int((bx + bw) * sc + 0.5), ry1 = int((by + bh) * sc + 0.5);
                if (have_normal) blit_icon(icons_.get(*ui_store_, normal.c_str()), rx0, ry0, rx1, ry1);
                hud_empty_group_tab_ = HudRect{rx0, ry0, rx1, ry1};
                const LabelLook il = resolve_label_style(hud_style_set(), hud_authored_ ? hud_authored_->style_group_index : std::string(), human_race());
                const FaceScope index_face(style_face(il.font));
            const ShadowScope index_shadow(il, sc);
                draw_text_rgba(out, view_w, view_h, int((bx + bw * 0.5f) * sc + 0.5) - text_w_rgba("1", 1) / 2,
                               int((by + bh - 6.0f - 20.0f) * sc + 0.5), "1", 1, il.set ? il.r : 200, il.set ? il.g : 208, il.set ? il.b : 220);
                continue;
            }
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
            const LabelLook cl = resolve_label_style(
                hud_style_set(), hud_authored_ ? hud_authored_->style_group_count : std::string(),
                human_race());
            static const bool kCountStyleOld = std::getenv("OMC_GROUP_COUNT_STYLE_OLD") != nullptr;
            if (kCountStyleOld || !cl.set || cl.height <= 0) {
                draw_text_rgba(out, view_w, view_h, int((bx + 44.0f) * sc + 0.5),
                               int((by + 10.0f) * sc + 0.5), count, 2, cl.set ? cl.r : 230,
                               cl.set ? cl.g : 232, cl.set ? cl.b : 236);
            } else {
                const FaceScope count_face(style_face(cl.font));
                const ShadowScope count_shadow(cl, sc);
                const int saved_cpx = draw::forced_font_px();
                draw::set_forced_font_px(std::max(6, int(double(cl.height) * sc + 0.5)));
                draw_text_rgba(out, view_w, view_h, int((bx + 44.0f) * sc + 0.5), int((by + 10.0f) * sc + 0.5), count, 1, cl.r, cl.g, cl.b);
                draw::set_forced_font_px(saved_cpx);
            }
            char index[4];
            std::snprintf(index, sizeof index, "%d", slot);
            const LabelLook il = resolve_label_style(
                hud_style_set(), hud_authored_ ? hud_authored_->style_group_index : std::string(),
                human_race());
            const FaceScope index_face(style_face(il.font));
            const ShadowScope index_shadow(il, sc);
            draw_text_rgba(out, view_w, view_h,
                           int((bx + bw * 0.5f) * sc + 0.5) - text_w_rgba(index, 1) / 2,
                           int((by + bh - 6.0f - 20.0f) * sc + 0.5), index, 1,
                           il.set ? il.r : 200, il.set ? il.g : 208, il.set ? il.b : 220);
            hud_group_buttons_.push_back(std::move(b));
            bx += step;
        }
    }

    ov_lap.lap("ov.groups");
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
    ov_lap.lap("ov.alerts");

    hud_page_buttons_.clear();
    const int page_buttons = (group_pages < 1 && !group_pane.empty() && !std::getenv("OMC_NO_GROUP_PORTRAIT")) ? 1 : group_pages;
    if ((group_pages > 1 || page_buttons == 1) && ui_store_ && cat_ && console_panel.authored && console_panel.authored->page_buttons_ok) {
        const double sc = double(view_h) / double(kRefHeight);
        const std::string normal = cat_->resolve_asset("@@UI/GroupPageButtonNormal");
        const std::string hover = cat_->resolve_asset("@@UI/GroupPageButtonHover");
        for (int p = 0; p < std::max(group_pages, page_buttons); ++p) {
            const HudAuthoredRects::R& q = console_panel.authored->page_buttons[p];
            HudPageButton pb;
            pb.page = p;
            pb.rect = {int(q.x0 * sc + 0.5), int(q.y0 * sc + 0.5), int(q.x1 * sc + 0.5), int(q.y1 * sc + 0.5)};
            const std::string& img = p == group_page_ ? hover : normal;
            if (!img.empty() && img[0] != '@') {
                const IconAtlas::Icon& tab = icons_.get(*ui_store_, img.c_str());
                if (tab.ok() && tab.h >= 2 && pb.rect.w() > 0 && pb.rect.h() > 0 && !std::getenv("OMC_NO_GROUP_PORTRAIT")) {
                    int tc[3] = {40, 125, 75};
                    hud_console_color(*ui_store_, console_panel.race, "StandardBorder", tc);
                    const int half = int(tab.h) / 2;
                    for (int y = pb.rect.y0; y < pb.rect.y1 && y < int(view_h); ++y)
                        for (int x = pb.rect.x0; x < pb.rect.x1 && x < int(view_w); ++x) {
                            if (x < 0 || y < 0) continue;
                            const int sx = (x - pb.rect.x0) * int(tab.w) / pb.rect.w(), sy = (y - pb.rect.y0) * half / pb.rect.h();
                            const std::size_t si = (std::size_t(sy) * tab.w + std::size_t(sx)) * 4;
                            const float a = tab.rgba[si + 3] / 255.0f;
                            if (a <= 0.0f) continue;
                            const std::size_t di = (std::size_t(y) * view_w + std::size_t(x)) * 4;
                            for (int k = 0; k < 3; ++k)
                                out[di + k] = std::uint8_t(float(tab.rgba[si + k]) / 255.0f * float(tc[k]) * a + out[di + k] * (1.0f - a) + 0.5f);
                            out[di + 3] = 255;
                        }
                } else {
                    blit_icon(tab, pb.rect.x0, pb.rect.y0, pb.rect.x1, pb.rect.y1);
                }
            }
            char label[4];
            std::snprintf(label, sizeof label, "%d", p + 1);
            draw_text_rgba(out, view_w, view_h,
                           (pb.rect.x0 + pb.rect.x1) / 2 - text_w_rgba(label, 1) / 2,
                           (pb.rect.y0 + pb.rect.y1) / 2 - 6, label, 1, 220, 224, 232);
            hud_page_buttons_.push_back(pb);
        }
    }
    ov_lap.lap("ov.alerts_pages");
    if (!group_pane.empty() && ui_store_) {
        for (std::size_t i = 0; i < group_pane.size(); ++i) {
            const HudRect s = hud_group_slot(view_w, view_h, int(i), &console_panel);
            if (s.w() <= 0) continue;
            const std::uint32_t id = group_pane[i];
            const Unit& gu = world_.unit(id);
            if (console_cell_art && cat_) {
                const std::string wf_name = cat_->unit_wireframe_image(gu.catalog_id).value_or(std::string());
                const IconAtlas::Icon& gwf = icons_.get(*ui_store_, wf_name);
                if (!wf_name.empty() && gwf.ok()) {
                    const int ghr = human_race();
                    const std::string cellp = std::string("Assets/Textures/ui_console_commandbutton_normal_") +
                                              (ghr == 1 ? "defaultprotoss" : ghr == 2 ? "defaultzerg" : "defaultterran") + ".dds";
                    bool framed = false;
                    if (!std::getenv("OMC_NO_GROUP_PORTRAIT")) {
                        const std::string fpath = cat_->resolve_asset("@@UI/ButtonBorderQueueActiveNormal", ui::layout_race_suffix(console_panel.race));
                        const IconAtlas::Icon* ft = (!fpath.empty() && fpath[0] != '@') ? &icons_.get(*ui_store_, fpath.c_str()) : nullptr;
                        if (ft && ft->ok() && ft->h >= 2 && s.w() > 0 && s.h() > 0) {
                            int fc[3] = {155, 255, 190};
                            hud_console_color(*ui_store_, console_panel.race, "StandardButtonIcon", fc);
                            const int fhalf = int(ft->h) / 2;
                            for (int y = s.y0; y < s.y1 && y < int(view_h); ++y)
                                for (int x = s.x0; x < s.x1 && x < int(view_w); ++x) {
                                    if (x < 0 || y < 0) continue;
                                    const int sx = (x - s.x0) * int(ft->w) / s.w(), sy = (y - s.y0) * fhalf / s.h();
                                    const std::size_t si = (std::size_t(sy) * ft->w + std::size_t(sx)) * 4;
                                    const float a = ft->rgba[si + 3] / 255.0f;
                                    if (a <= 0.0f) continue;
                                    const std::size_t di = (std::size_t(y) * view_w + std::size_t(x)) * 4;
                                    for (int k = 0; k < 3; ++k)
                                        out[di + k] = std::uint8_t(float(ft->rgba[si + k]) / 255.0f * float(fc[k]) * a + out[di + k] * (1.0f - a) + 0.5f);
                                    out[di + 3] = 255;
                                }
                            framed = true;
                        }
                    }
                    if (!framed) blit_icon(icons_.get(*ui_store_, cellp.c_str()), s.x0, s.y0, s.x1, s.y1);
                    float glf = gu.hp_max.raw() > 0 ? float(gu.hp.to_double() / gu.hp_max.to_double()) : 1.0f;
                    float tr = 0.0f, tg = 1.0f, tb = 0.0f;
                    if (glf <= 0.3f) { tr = 0.88f; tg = 0.29f; tb = 0.24f; }
                    else if (glf <= 0.6f) { tr = 0.90f; tg = 0.82f; tb = 0.27f; }
                    const bool mask = cat_->unit_race_index(gu.catalog_id) != 2;
                    const int inset = framed ? std::max(1, s.w() / 20) : std::max(2, s.w() / 10);
                    blit_icon_tinted(gwf, s.x0 + inset, s.y0 + inset, s.x1 - inset, s.y1 - inset,
                                     mask ? tr : 1.0f, mask ? tg : 1.0f, mask ? tb : 1.0f);
                    continue;
                }
            }
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
    ov_lap.lap("ov.group_pane");

    if (ui_store_ && cat_ && selection_.size() == 1 && selection_[0] < world_.units().size() &&
        world_.unit(selection_[0]).alive) {
        std::vector<std::uint32_t> shown;
        int slot = 0;
        for (const sim::BehaviorInstance& bi : world_.unit(selection_[0]).behaviors) {
            if (slot >= 6) break;
            if (!bi.src || std::find(shown.begin(), shown.end(), bi.src) != shown.end()) continue;
            shown.push_back(bi.src);
            const std::string& bid = behavior_id_of_tag(bi.src);
            if (bid.empty()) continue;
            const auto ic = cat_->behavior_info_icon(bid);
            if (!ic || ic->empty()) continue;
            const HudRect s = hud_behavior_slot(view_w, view_h, slot, &console_panel);
            if (s.w() <= 0) break;
            blit_icon(icons_.get(*ui_store_, ic->c_str()), s.x0, s.y0, s.x1, s.y1);
            ++slot;
        }
    }

    if (ui_store_ && cat_ && selection_.size() == 1 && selection_[0] < world_.units().size() &&
        world_.unit(selection_[0]).alive && world_.unit(selection_[0]).player == cfg_.human_player) {
        const std::uint32_t sel = selection_[0];
        auto slot_backdrop = [&](const HudRect& s) {
            for (int y = s.y0; y < s.y1 && y < int(view_h); ++y)
                for (int x = s.x0; x < s.x1 && x < int(view_w); ++x) {
                    if (x < 0 || y < 0) continue;
                    const std::size_t di = (std::size_t(y) * view_w + x) * 4;
                    out[di] = std::uint8_t(22 * 0.55f + out[di] * 0.45f);
                    out[di + 1] = std::uint8_t(28 * 0.55f + out[di + 1] * 0.45f);
                    out[di + 2] = std::uint8_t(40 * 0.55f + out[di + 2] * 0.45f);
                    out[di + 3] = 255;
                }
        };
        auto slot_bar = [&](const HudRect& s, float f, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            const int bx0 = s.x0 + 3, bx1 = s.x1 - 3, by1 = s.y1 - 3, by0 = by1 - 9;
            f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
            const int fillw = int(float(bx1 - bx0) * f + 0.5f);
            for (int y = by0; y < by1 && y < int(view_h); ++y)
                for (int x = bx0; x < bx1 && x < int(view_w); ++x) {
                    if (x < 0 || y < 0) continue;
                    const std::size_t di = (std::size_t(y) * view_w + x) * 4;
                    const bool on = x < bx0 + fillw;
                    out[di] = on ? r : 12;
                    out[di + 1] = on ? g : 14;
                    out[di + 2] = on ? b : 18;
                    out[di + 3] = 255;
                }
        };
        auto slot_icon = [&](const HudRect& s, const std::string& unit_id) {
            if (unit_id.empty()) return;
            std::optional<std::string> ic = cat_->button_icon(unit_id);
            if ((!ic || ic->empty()) && cat_->find("CUpgrade", unit_id))
                ic = cat_->scalar("CUpgrade", unit_id, "Icon");
            if (ic && !ic->empty()) blit_icon(icons_.get(*ui_store_, ic->c_str()), s.x0 + 1, s.y0 + 1, s.x1 - 1, s.y1 - 1);
        };
        int slot_border[3] = {60, 70, 88};
        if (ui_store_) hud_console_color(*ui_store_, console_panel.race, "StandardButtonBorder", slot_border);
        const std::vector<PaneQueueSlot> queue = pane_queue(sel);
        auto queue_border = [&](const HudRect& s, int slot_no, const int* tint) {
            if (!cat_ || std::getenv("OMC_QUEUE_SLOT_BOXES") || s.w() <= 0 || s.h() <= 0) return false;
            char key[64];
            if (slot_no <= 1) std::snprintf(key, sizeof key, "@@UI/ButtonBorderQueueActiveNormal");
            else std::snprintf(key, sizeof key, "@@UI/ButtonBorderQueueInactive%02dNormal", slot_no);
            const std::string path = cat_->resolve_asset(key, ui::layout_race_suffix(console_panel.race));
            if (path.empty() || path[0] == '@') return false;
            const IconAtlas::Icon& tex = icons_.get(*ui_store_, path.c_str());
            if (!tex.ok() || tex.h < 2) return false;
            const int half = int(tex.h) / 2;
            for (int y = s.y0; y < s.y1 && y < int(view_h); ++y)
                for (int x = s.x0; x < s.x1 && x < int(view_w); ++x) {
                    if (x < 0 || y < 0) continue;
                    const int sx = (x - s.x0) * int(tex.w) / s.w(), sy = (y - s.y0) * half / s.h();
                    const std::size_t si = (std::size_t(sy) * tex.w + std::size_t(sx)) * 4;
                    const float a = tex.rgba[si + 3] / 255.0f;
                    if (a <= 0.0f) continue;
                    const std::size_t di = (std::size_t(y) * view_w + std::size_t(x)) * 4;
                    for (int k = 0; k < 3; ++k)
                        out[di + k] = std::uint8_t(float(tex.rgba[si + k]) / 255.0f * float(tint[k]) * a + out[di + k] * (1.0f - a) + 0.5f);
                    out[di + 3] = 255;
                }
            return true;
        };
        if (!queue.empty() && !std::getenv("OMC_QUEUE_PANE_OLD")) {
            const double qs = double(view_h) / double(kRefHeight);
            int icon_col[3] = {155, 255, 190};
            hud_console_color(*ui_store_, console_panel.race, "StandardButtonIcon", icon_col);
            int pfill[3] = {155, 255, 190}, pborder[3] = {40, 125, 75};
            hud_console_color(*ui_store_, console_panel.race, "StandardProgressFill", pfill);
            hud_console_color(*ui_store_, console_panel.race, "StandardProgressBorder", pborder);
            const int saved_px = draw::forced_font_px();
            for (int q = 1; q <= 4; ++q) {
                if (std::size_t(q) < queue.size()) continue;
                const HudRect s = hud_queue_slot(view_w, view_h, q, &console_panel);
                if (s.w() <= 0) continue;
                slot_backdrop(s);
                if (queue_border(s, q + 1, icon_col)) continue;
                draw_box_rgba(out, view_w, view_h, s.x0, s.y0, s.x1, s.y1, std::uint8_t(icon_col[0]), std::uint8_t(icon_col[1]), std::uint8_t(icon_col[2]));
                draw_box_rgba(out, view_w, view_h, s.x0 + 1, s.y0 + 1, s.x1 - 1, s.y1 - 1, std::uint8_t(icon_col[0]), std::uint8_t(icon_col[1]), std::uint8_t(icon_col[2]));
                const std::string num = std::to_string(q + 1);
                const int npx = std::max(8, int(36.0 * qs + 0.5));
                draw::set_forced_font_px(npx);
                const int nw = text_w_rgba(num, 1);
                draw_text_rgba(out, view_w, view_h, (s.x0 + s.x1) / 2 - nw / 2, (s.y0 + s.y1) / 2 - npx / 2, num.c_str(), 1,
                               std::uint8_t(icon_col[0]), std::uint8_t(icon_col[1]), std::uint8_t(icon_col[2]));
            }
            const HudRect a = hud_queue_slot(view_w, view_h, 0, &console_panel);
            if (a.w() > 0) {
                const int bx0 = a.x1 + int(4.0 * qs + 0.5), bx1 = bx0 + int(128.0 * qs + 0.5);
                const bool old_bar = std::getenv("OMC_QUEUE_SLOT_BOXES") != nullptr;
                const int by1 = a.y1 - int(4.0 * qs + 0.5), by0 = by1 - std::max(3, int((old_bar ? 8.0 : 13.0) * qs + 0.5));
                const int inset = old_bar ? 1 : std::max(1, int(4.0 * qs + 0.5));
                const float pf = queue[0].progress < 0.0f ? 0.0f : (queue[0].progress > 1.0f ? 1.0f : queue[0].progress);
                const int fillx = bx0 + inset + int(float(bx1 - bx0 - 2 * inset) * pf + 0.5f);
                for (int y = by0; y < by1 && y < int(view_h); ++y)
                    for (int x = bx0; x < bx1 && x < int(view_w); ++x) {
                        if (x < 0 || y < 0) continue;
                        const std::size_t di = (std::size_t(y) * view_w + std::size_t(x)) * 4;
                        const bool corner = !old_bar && (x == bx0 || x == bx1 - 1) && (y == by0 || y == by1 - 1);
                        if (corner) continue;
                        const bool edge = y == by0 || y == by1 - 1 || x == bx0 || x == bx1 - 1;
                        const bool on = !edge && x < fillx && x >= bx0 + inset && y >= by0 + inset && y < by1 - inset;
                        out[di] = std::uint8_t(edge ? pborder[0] : on ? pfill[0] : 10);
                        out[di + 1] = std::uint8_t(edge ? pborder[1] : on ? pfill[1] : 14);
                        out[di + 2] = std::uint8_t(edge ? pborder[2] : on ? pfill[2] : 12);
                        out[di + 3] = 255;
                    }
                const std::string& akey = queue[0].activity.empty() ? std::string("UI/Training") : queue[0].activity;
                std::string word = akey == "UI/Training" ? "Training" : akey.substr(akey.rfind('/') + 1);
                if (const std::string* w = cat_->strings().find(akey)) word = *w;
                const LabelLook al = resolve_label_style(hud_style_set(), "QueueActivity", human_race());
                const FaceScope activity_face(old_bar ? nullptr : style_face(al.font));
                const ShadowScope activity_shadow(old_bar ? LabelLook{} : al, qs);
                const int lpx = std::max(6, int((!old_bar && al.height > 0 ? double(al.height) : 14.0) * qs + 0.5));
                draw::set_forced_font_px(lpx);
                const bool styled = al.set && !old_bar;
                draw_text_rgba(out, view_w, view_h, bx0 + (old_bar ? 0 : int(4.0 * qs + 0.5)), by0 - int((old_bar ? 2.0 : 12.0) * qs + 0.5) - lpx - 1, word.c_str(), 1, styled ? al.r : 214,
                               styled ? al.g : 226, styled ? al.b : 220);
            }
            draw::set_forced_font_px(saved_px);
        }
        for (std::size_t q = 0; q < queue.size() && q < 8; ++q) {
            const HudRect s = hud_queue_slot(view_w, view_h, int(q), &console_panel);
            if (s.w() <= 0) continue;
            slot_backdrop(s);
            slot_icon(s, queue[q].subject);
            if (queue[q].progress > 0.0f && std::getenv("OMC_QUEUE_PANE_OLD")) slot_bar(s, queue[q].progress, 0, 200, 200);
            {
                int qcol[3] = {155, 255, 190};
                hud_console_color(*ui_store_, console_panel.race, "StandardButtonIcon", qcol);
                if (queue_border(s, 1, qcol)) continue;
            }
            const int f = q == 0 ? 3 : 2;
            draw_box_rgba(out, view_w, view_h, s.x0, s.y0, s.x1, s.y1,
                          std::uint8_t(slot_border[0] * f / 3), std::uint8_t(slot_border[1] * f / 3),
                          std::uint8_t(slot_border[2] * f / 3));
        }
        for (std::size_t q = 0; q < hud_equipment_.size(); ++q) {
            const HudRect s = hud_equipment_cell(view_w, view_h, int(q), &console_panel, int(hud_equipment_.size()));
            if (s.w() <= 0) continue;
            if (hud_equipment_[q].icon.empty() || !ui_store_) continue;
            const IconAtlas::Icon& ic = icons_.get(*ui_store_, hud_equipment_[q].icon);
            if (!ic.ok()) continue;
            blit_icon(ic, s.x0 + 2, s.y0 + 2, s.x1 - 2, s.y1 - 2);
            if (cat_ && !std::getenv("OMC_NO_EQUIPMENT_BORDER")) {
                const std::string esfx = ui::layout_race_suffix(console_panel.race);
                std::string border = cat_->resolve_asset("@@@UI/ButtonBorderEquipment", esfx, esfx);
                if (border.empty() || border[0] == '@') border = cat_->resolve_asset("@@UI/ButtonBorderEquipment", esfx);
                if (std::getenv("OMC_LABEL_FACE_TRACE") && q == 0) std::fprintf(stderr, "[labelface] equipment border %s\n", border.c_str());
                if (!border.empty() && border[0] != '@') {
                    int bc[3] = {40, 125, 75};
                    hud_console_color(*ui_store_, console_panel.race, "StandardButtonBorder", bc);
                    const IconAtlas::Icon& bi = icons_.get(*ui_store_, border.c_str());
                    if (bi.ok()) blit_icon_tinted(bi, s.x0, s.y0, s.x1, s.y1, float(bc[0]) / 255.0f, float(bc[1]) / 255.0f, float(bc[2]) / 255.0f);
                }
            }
            {
                char lv[8];
                std::snprintf(lv, sizeof lv, "%d", hud_equipment_[q].level);
                if (cat_ && !std::getenv("OMC_NO_EQUIPMENT_BORDER")) {
                    const double esc = double(view_h) / double(kRefHeight);
                    const LabelLook ll = resolve_label_style(hud_style_set(), "EquipmentIconLevel", human_race());
                    const FaceScope level_face(style_face(ll.font));
                    const ShadowScope level_shadow(ll, esc);
                    const int saved_lpx = draw::forced_font_px();
                    const int lpx = std::max(6, int((ll.height > 0 ? double(ll.height) : 16.0) * esc + 0.5));
                    draw::set_forced_font_px(lpx);
                    const int lw2 = text_w_rgba(lv, 1);
                    draw_text_rgba(out, view_w, view_h, s.x1 - int(8.0 * esc + 0.5) - lw2, s.y1 - int(8.0 * esc + 0.5) - lpx, lv, 1,
                                   ll.set ? ll.r : 255, ll.set ? ll.g : 255, ll.set ? ll.b : 255);
                    draw::set_forced_font_px(saved_lpx);
                } else {
                const int lw = text_w_rgba(lv, 1);
                const int bx1 = s.x1 - 2, by1 = s.y1 - 2, bx0 = bx1 - lw - 6, by0 = by1 - ui::kFontH - 2;
                for (int y = std::max(0, by0); y < by1 && y < int(view_h); ++y)
                    for (int x = std::max(0, bx0); x < bx1 && x < int(view_w); ++x) {
                        const std::size_t di = (std::size_t(y) * view_w + std::size_t(x)) * 4;
                        out[di] = 8; out[di + 1] = 10; out[di + 2] = 12; out[di + 3] = 255;
                    }
                draw_text_rgba(out, view_w, view_h, bx0 + 3, by0 + 1, lv, 1, 235, 236, 210);
                }
            }
        }

        const std::vector<std::uint32_t> hold = world_.passengers(sel);
        for (std::size_t c = 0; c < hold.size() && c < 8; ++c) {
            const HudRect s = hud_cargo_slot(view_w, view_h, int(c), &console_panel);
            if (s.w() <= 0) continue;
            const Unit& pu = world_.unit(hold[c]);
            slot_backdrop(s);
            slot_icon(s, pu.catalog_id);
            const float lf = pu.hp_max.raw() > 0 ? float(pu.hp.to_double() / pu.hp_max.to_double()) : 1.0f;
            slot_bar(s, lf, lf <= 0.3f ? 225 : (lf <= 0.6f ? 230 : 70), lf <= 0.3f ? 75 : (lf <= 0.6f ? 210 : 220),
                     lf <= 0.3f ? 60 : (lf <= 0.6f ? 70 : 90));
            draw_box_rgba(out, view_w, view_h, s.x0, s.y0, s.x1, s.y1, std::uint8_t(slot_border[0]),
                          std::uint8_t(slot_border[1]), std::uint8_t(slot_border[2]));
        }
    }
    ov_lap.lap("ov.single_panes");

    if (cfg_.human_player >= 0 && ui_store_) {
        std::vector<BuildCardEntry> ov_builds;
        std::vector<AbilityCardEntry> ov_abils;
        std::vector<TrainCardEntry> ov_trains;
        std::vector<SubmenuCardEntry> ov_submenus;
        CommandCard card = compose_command_card_cached(ov_builds, ov_abils, ov_trains, ov_submenus);
        ov_lap.lap("ov.card_compose");

        for (int i = 0; i < CommandCard::kSlots; ++i) {
            const HudRect c = hud_command_cell(view_w, view_h, i, &console_panel);
            if (c.w() <= 0) continue;
            const CommandButton& btn = card.slots[i];
            std::uint8_t br = 20, bg = 24, bb = 32;
            if (btn.state == ButtonState::Disabled) { br = 40; bg = 22; bb = 22; }
            else if (btn.state != ButtonState::Empty) { br = 26, bg = 34, bb = 48; }
            bool cell_active = false;
            if (console_cell_art && btn.state == ButtonState::Enabled && !btn.icon.empty() && !std::getenv("OMC_NO_ACTIVE_ORDER_CELL")) {
                std::string icn = btn.icon;
                for (char& ch : icn) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                auto has = [&](const char* s) { return icn.find(s) != std::string::npos; };
                for (std::uint32_t sid : selection_) {
                    if (sid >= world_.units().size()) continue;
                    const Unit& sun = world_.unit(sid);
                    if (!sun.alive || sun.orders.empty()) continue;
                    const sim::OrderType ot = sun.orders.front().type;
                    if ((ot == sim::OrderType::Move && has("command-move")) || (ot == sim::OrderType::HoldPosition && has("holdposition")) ||
                        (ot == sim::OrderType::Patrol && has("patrol")) ||
                        ((ot == sim::OrderType::AttackMove || ot == sim::OrderType::Attack) && has("command-attack")) ||
                        (ot == sim::OrderType::Gather && sun.carrying <= 0 && has("gather")) ||
                        (ot == sim::OrderType::Gather && sun.carrying > 0 && has("returncargo")) ||
                        (ot == sim::OrderType::Repair && has("repair"))) {
                        cell_active = true;
                        break;
                    }
                }
            }
            if (console_cell_art && btn.state != ButtonState::Empty) {
                const int chr = human_race();
                const std::string cellp = std::string(cell_active ? "Assets/Textures/ui_console_commandbutton_highlighted_" : "Assets/Textures/ui_console_commandbutton_normal_") +
                                          (chr == 1 ? "defaultprotoss" : chr == 2 ? "defaultzerg" : "defaultterran") + ".dds";
                blit_icon(icons_.get(*ui_store_, cellp.c_str()), c.x0, c.y0, c.x1, c.y1);
            } else if (console_cell_art) {
                const int chr = human_race();
                const std::string cellp = std::string("Assets/Textures/ui_console_commandbutton_normal_") +
                                          (chr == 1 ? "defaultprotoss" : chr == 2 ? "defaultzerg" : "defaultterran") + ".dds";
                const IconAtlas::Icon& ghost = icons_.get(*ui_store_, cellp.c_str());
                if (ghost.ok() && c.x1 > c.x0 && c.y1 > c.y0)
                    for (int y = c.y0; y < c.y1 && y < int(view_h); ++y)
                        for (int x = c.x0; x < c.x1 && x < int(view_w); ++x) {
                            if (x < 0 || y < 0) continue;
                            const int sx = (x - c.x0) * int(ghost.w) / (c.x1 - c.x0), sy = (y - c.y0) * int(ghost.h) / (c.y1 - c.y0);
                            const std::size_t si = (std::size_t(sy) * ghost.w + std::size_t(sx)) * 4;
                            const float a = 0.04f * ghost.rgba[si + 3] / 255.0f;
                            const float l = 0.299f * ghost.rgba[si] + 0.587f * ghost.rgba[si + 1] + 0.114f * ghost.rgba[si + 2];
                            const std::size_t di = (std::size_t(y) * view_w + std::size_t(x)) * 4;
                            for (int k = 0; k < 3; ++k) out[di + k] = std::uint8_t(l * a + out[di + k] * (1.0f - a) + 0.5f);
                        }
            }
            const bool grey_disabled = console_cell_art && !std::getenv("OMC_DISABLED_CELL_DARK");
            if (!console_cell_art || (btn.state == ButtonState::Disabled && !grey_disabled))
            for (int y = c.y0; y < c.y1 && y < int(view_h); ++y)
                for (int x = c.x0; x < c.x1 && x < int(view_w); ++x) {
                    if (x < 0 || y < 0) continue;
                    const std::size_t di = (std::size_t(y) * view_w + x) * 4;
                    out[di] = std::uint8_t(br * 0.55f + out[di] * 0.45f);
                    out[di + 1] = std::uint8_t(bg * 0.55f + out[di + 1] * 0.45f);
                    out[di + 2] = std::uint8_t(bb * 0.55f + out[di + 2] * 0.45f);
                    out[di + 3] = 255;
                }
            bool slot_autocast = btn.autocast;
            float slot_cooldown = btn.cooldown;
            if (cast_policy_ && cat_ && btn.state != ButtonState::Empty)
                for (const AbilityCardEntry& ab : ov_abils) {
                    if (ab.slot != i || ab.ability.empty()) continue;
                    if (hangar_auto_build_shown(ab.ability)) slot_autocast = true;
                    if (cat_->ability_autocast(ab.ability).offered)
                        for (std::uint32_t sid : selection_)
                            if (autocast_state(sid, ab.ability)) { slot_autocast = true; break; }
                    const float shown = card_cooldown_shown(ab.ability);
                    if (shown > slot_cooldown) slot_cooldown = shown;
                    break;
                }
            if (slot_autocast && btn.state != ButtonState::Empty)
                for (int y = c.y0; y < c.y1 && y < int(view_h); ++y)
                    for (int x = c.x0; x < c.x1 && x < int(view_w); ++x) {
                        if (x < 0 || y < 0) continue;
                        if (x > c.x0 + 1 && x < c.x1 - 2 && y > c.y0 + 1 && y < c.y1 - 2) continue;
                        const std::size_t di = (std::size_t(y) * view_w + x) * 4;
                        out[di] = 40; out[di + 1] = 220; out[di + 2] = 90; out[di + 3] = 255;
                    }
            if (btn.state != ButtonState::Empty && !btn.icon.empty()) {
                {
                    const IconAtlas::Icon& card_ic = icons_.get(*ui_store_, btn.icon.c_str());
                    bool white_art = card_ic.ok();
                    for (std::size_t q = 0; white_art && q + 3 < card_ic.rgba.size(); q += 4 * 7)
                        if (card_ic.rgba[q + 3] > 64) {
                            const int mx = std::max({int(card_ic.rgba[q]), int(card_ic.rgba[q + 1]), int(card_ic.rgba[q + 2])});
                            const int mn = std::min({int(card_ic.rgba[q]), int(card_ic.rgba[q + 1]), int(card_ic.rgba[q + 2])});
                            if (mx - mn > 24) white_art = false;
                        }
                    const bool terran_card = console_cell_art && human_race() == 0 && white_art && !cell_active;
                    float tr = 1.0f, tg = 1.0f, tb = 1.0f;
                    if (std::getenv("OMC_CARD_TINT_OLD")) {
                        if (terran_card) { tr = 136.0f / 255.0f; tg = 227.0f / 255.0f; tb = 162.0f / 255.0f; }
                    } else if (console_cell_art && white_art && !cell_active) {
                        int ic3[3] = {155, 255, 190};
                        hud_console_color(*ui_store_, console_panel.race, "StandardButtonIcon", ic3);
                        tr = float(ic3[0]) / 255.0f; tg = float(ic3[1]) / 255.0f; tb = float(ic3[2]) / 255.0f;
                    }
                    static const int kIconInset = std::getenv("OMC_CARD_ICON_INSET") ? std::atoi(std::getenv("OMC_CARD_ICON_INSET")) : 0;
                    blit_icon_tinted(card_ic, c.x0 + kIconInset, c.y0 + kIconInset, c.x1 - kIconInset, c.y1 - kIconInset, tr, tg, tb);
                    const std::string cell_key = !btn.hotkey.empty() ? btn.hotkey : (btn.is_cancel && !std::getenv("OMC_CANCEL_RED_BOX") ? std::string("Esc") : std::string());
                    static const bool kLetterPlate = std::getenv("OMC_CARD_LETTER_BARE") == nullptr && std::getenv("OMC_CARD_TINT_OLD") == nullptr;
                    if (kLetterPlate && console_cell_art && !cell_key.empty() && cell_key.size() <= 3) {
                        const int tx = c.x0 + 5, ty = c.y0 + 5;
                        const int pw = text_w_rgba(cell_key, 1) + 3;
                        for (int y = ty; y < ty + 12 && y < int(view_h); ++y)
                            for (int x = tx; x < tx + pw && x < int(view_w); ++x) {
                                if (x < 0 || y < 0) continue;
                                const std::size_t di = (std::size_t(y) * view_w + std::size_t(x)) * 4;
                                for (int k = 0; k < 3; ++k) out[di + k] = std::uint8_t(out[di + k] * 0.53f + 0.5f);
                            }
                        draw_text_rgba(out, view_w, view_h, tx, ty, cell_key.c_str(), 1, 240, 250, 242);
                    } else
                    if (console_cell_art && !cell_key.empty() && cell_key.size() <= 3)
                        draw_text_rgba(out, view_w, view_h, c.x0 + (std::getenv("OMC_CARD_TINT_OLD") ? 3 : 5), c.y0 + (std::getenv("OMC_CARD_TINT_OLD") ? 2 : 4), cell_key.c_str(), 1, 240, 250, 242);
                }
                if (btn.charges >= 0) {
                    const LabelLook cl = resolve_label_style(hud_style_set(), "GameButtonCharge", int(console_panel.race));
                    const double bsc = double(c.w()) / 76.0;
                    draw::set_forced_font_px(render::OverlayFont::fractional(double(cl.set && cl.height > 0 ? cl.height : 16) * bsc));
                    {
                        const FaceScope charge_face(style_face(cl.set && !cl.font.empty() ? cl.font : std::string("UI/Fonts/Eurostile-Bol.otf")));
                        const ShadowScope charge_shadow(cl, bsc);
                        const std::string n = std::to_string(btn.charges);
                        const int nw = text_w_rgba(n, 1);
                        const int nh = int(double(cl.set && cl.height > 0 ? cl.height : 16) * bsc + 0.5);
                        const int off = int(8.0 * bsc + 0.5);
                        draw_text_rgba(out, view_w, view_h, c.x1 - off - nw, c.y1 - off - nh, n.c_str(), 1, cl.r, cl.g, cl.b);
                    }
                    draw::set_forced_font_px(0);
                }
                if (slot_cooldown > 0.0f) {
                    const float ccx = 0.5f * float(c.x0 + c.x1), ccy = 0.5f * float(c.y0 + c.y1);
                    for (int y = c.y0; y < c.y1 && y < int(view_h); ++y)
                        for (int x = c.x0; x < c.x1 && x < int(view_w); ++x) {
                            if (x < 0 || y < 0) continue;
                            if (!cooldown_sweep_covers(ccx, ccy, float(x) + 0.5f, float(y) + 0.5f,
                                                       slot_cooldown))
                                continue;
                            const std::size_t di = (std::size_t(y) * view_w + x) * 4;
                            out[di] = std::uint8_t(out[di] * 0.35f);
                            out[di + 1] = std::uint8_t(out[di + 1] * 0.35f);
                            out[di + 2] = std::uint8_t(out[di + 2] * 0.35f);
                        }
                }
                if (btn.state == ButtonState::Disabled && grey_disabled) {
                    for (int y = c.y0; y < c.y1 && y < int(view_h); ++y)
                        for (int x = c.x0; x < c.x1 && x < int(view_w); ++x) {
                            if (x < 0 || y < 0) continue;
                            const std::size_t di = (std::size_t(y) * view_w + x) * 4;
                            const float l = 0.58f * (0.299f * out[di] + 0.587f * out[di + 1] + 0.114f * out[di + 2]);
                            out[di] = out[di + 1] = out[di + 2] = std::uint8_t(l + 0.5f);
                        }
                    if (!btn.hotkey.empty() && btn.hotkey.size() <= 2)
                        draw_text_rgba(out, view_w, view_h, c.x0 + (std::getenv("OMC_CARD_TINT_OLD") ? 3 : 5), c.y0 + (std::getenv("OMC_CARD_TINT_OLD") ? 2 : 4), btn.hotkey.c_str(), 1, 240, 250, 242);
                } else if (btn.state == ButtonState::Disabled)
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
                if (!console_cell_art || (btn.is_cancel && std::getenv("OMC_CANCEL_RED_BOX"))) draw_box_rgba(out, view_w, view_h, c.x0, c.y0, c.x1, c.y1, lr, lg, lb);
            } else if (!console_cell_art) {
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
            } else if (const HudButton* hb = hud_buttons_.hovered(); hb && cat_ && !hb->tooltip_key.empty()) {
                std::vector<std::string> raw = hud_tooltip_lines(cat_->strings(), cat_->hotkeys(), hb->tooltip_key);
                if (!raw.empty()) {
                    const double tsc = double(view_h) / double(kRefHeight);
                    const bool mm = hb->id == HudButtonId::MinimapPing || hb->id == HudButtonId::MinimapTerrain || hb->id == HudButtonId::MinimapColor;
                    const LabelLook tl = resolve_label_style(hud_style_set(), "@@@StandardTooltip", human_race());
                    const int fpx = std::max(6, int(double(tl.height > 0 ? tl.height : 20) * tsc + 0.5));
                    const int saved_px = draw::forced_font_px();
                    draw::set_forced_font_px(fpx);
                    const FaceScope tip_face(style_face(tl.font));
                    const ShadowScope tip_shadow(tl, tsc);
                    const int wrap_w = int((mm ? 320.0 : 450.0) * tsc + 0.5), pad = int(15.0 * tsc + 0.5);
                    std::vector<std::string> lines;
                    for (const std::string& ln : raw) {
                        std::string cur, word;
                        auto flush_word = [&]() {
                            if (word.empty()) return;
                            const std::string trial = cur.empty() ? word : cur + " " + word;
                            if (!cur.empty() && text_w_rgba(trial, 1) > wrap_w) {
                                lines.push_back(cur);
                                cur = word;
                            } else {
                                cur = trial;
                            }
                            word.clear();
                        };
                        for (char ch : ln) {
                            if (ch == ' ') flush_word();
                            else word.push_back(ch);
                        }
                        flush_word();
                        lines.push_back(cur);
                    }
                    const int lh = fpx + std::max(1, fpx / 5);
                    int tw = 0;
                    for (const std::string& ln : lines) tw = std::max(tw, text_w_rgba(ln, 1));
                    const int pw = tw + pad * 2, ph = int(lines.size()) * lh + pad * 2;
                    int x0 = hb->rect.x0, y0 = hb->rect.y0 - ph - 2;
                    if (y0 < 0) y0 = hb->rect.y1 + 2;
                    if (x0 + pw > int(view_w)) x0 = int(view_w) - pw;
                    if (y0 + ph > int(view_h)) y0 = int(view_h) - ph;
                    if (x0 < 0) x0 = 0;
                    if (y0 < 0) y0 = 0;
                    draw::fill(out, view_w, view_h, x0, y0, x0 + pw, y0 + ph, 12, 16, 26, 235);
                    draw::box(out, view_w, view_h, x0, y0, x0 + pw, y0 + ph, 90, 120, 170);
                    for (std::size_t li = 0; li < lines.size(); ++li)
                        draw_text_rgba(out, view_w, view_h, x0 + pad, y0 + pad + int(li) * lh, lines[li], 1, tl.set ? tl.r : 132, tl.set ? tl.g : 210,
                                       tl.set ? tl.b : 255);
                    draw::set_forced_font_px(saved_px);
                }
            }
        }
    }
    ov_lap.lap("ov.command_card");
    if (!warming_) render::mem_lap_marks() = false;

    static const bool kShowFps = std::getenv("OMC_SHOW_FPS") != nullptr;
    if (kShowFps) {
        char fps[24];
        std::snprintf(fps, sizeof(fps), "FPS %d", int(fps_smooth_ + 0.5f));
        draw_text_rgba(out, view_w, view_h, 6, 6, fps, 2, 80, 255, 120);
    }

    if (replay_drive_) {
        char rb[40];
        std::snprintf(rb, sizeof(rb), "REPLAY  %d/%d", world_.tick(), replay_total_);
        const int tw = text_w_rgba(rb, 2);
        draw_text_rgba(out, view_w, view_h, (int(view_w) - tw) / 2, 6, rb, 2, 255, 210, 90);
    }

    hud_menu_draw(view_w, view_h, out);
    if (pause_menu_open_) {
        draw::fill_a(out, view_w, view_h, 0, 0, int(view_w), int(view_h), 0, 0, 0, theme::kAlphaScrim);
        PauseRect resume, quit;
        pause_menu_rects(view_w, view_h, resume, quit);
        const PauseRects all = pause_rects_all(view_w, view_h);
        const int top_row = all.has_settings ? std::min(resume.y0, all.settings.y0) : resume.y0;
        int px0 = resume.x0 - 30, py0 = top_row - 90, px1 = resume.x1 + 30, py1 = quit.y1 + 30;
        if (ui_store_ && view_w && view_h)
            if (const HudPauseRects* pr = hud_pause_rects(*ui_store_, float(view_w) / float(view_h))) {
                const double sc = double(view_h) / double(kRefHeight);
                px0 = int(pr->panel.x0 * sc + 0.5); py0 = int(pr->panel.y0 * sc + 0.5);
                px1 = int(pr->panel.x1 * sc + 0.5); py1 = int(pr->panel.y1 * sc + 0.5);
                if (all.has_settings && all.settings.y0 < py0 + 60) py0 = all.settings.y0 - 90;
            }
        draw::vgrad(out, view_w, view_h, px0, py0, px1, py1, theme::kSurfaceRaised.r, theme::kSurfaceRaised.g, theme::kSurfaceRaised.b,
                    theme::kSurface.r, theme::kSurface.g, theme::kSurface.b, 255, 255);
        draw::box_a(out, view_w, view_h, px0, py0, px1, py1, theme::kBorderStrong.r, theme::kBorderStrong.g, theme::kBorderStrong.b, 255);
        const std::string title = "GAME MENU";
        static const bool text_leak = std::getenv("OMC_PAUSE_TEXT_LEAK") != nullptr;
        const bool sized = ui_store_ && !text_leak;
        const int saved_pause_px = draw::forced_font_px();
        const double psc = double(view_h) / double(kRefHeight);
        const int label_px = sized ? std::max(6, int(20.0 * psc + 0.5)) : draw::font_px_for(2);
        const int title_px = sized ? std::max(6, int(24.0 * psc + 0.5)) : draw::font_px_for(3);
        int first_row_y = top_row;
        if (sized) {
            first_row_y = std::min(top_row, quit.y0);
            if (all.has_load) first_row_y = std::min(first_row_y, all.load.y0);
            if (all.has_save) first_row_y = std::min(first_row_y, all.save.y0);
            draw::set_forced_font_px(title_px);
        }
        draw::text(out, view_w, view_h, (px0 + px1 - draw::text_w(title, 3)) / 2,
                   sized ? py0 + std::max(4, (first_row_y - py0 - title_px) / 2) : py0 + std::max(12, (top_row - py0 - ui::kFontH * 3) / 2),
                   title, 3, theme::kTextHi.r, theme::kTextHi.g, theme::kTextHi.b);
        if (sized) draw::set_forced_font_px(label_px);
        const PauseRect* rects[5] = {&all.load, &all.save, &all.settings, &resume, &quit};
        const char* labels[5] = {"LOAD GAME", "SAVE GAME", "SETTINGS", "RESUME GAME", "QUIT GAME"};
        for (int i = 0; i < 5; ++i) {
            if ((i == 0 && !all.has_load) || (i == 1 && !all.has_save) || (i == 2 && !all.has_settings)) continue;
            const PauseRect& r = *rects[i];
            const bool hot = mouse_x_ >= float(r.x0) && mouse_x_ < float(r.x1) &&
                             mouse_y_ >= float(r.y0) && mouse_y_ < float(r.y1);
            const bool is_quit = i == 4;
            draw::vgrad(out, view_w, view_h, r.x0, r.y0, r.x1, r.y1, theme::kMenuBtnTop.r, theme::kMenuBtnTop.g, theme::kMenuBtnTop.b,
                        theme::kMenuBtnBottom.r, theme::kMenuBtnBottom.g, theme::kMenuBtnBottom.b, 255, 255);
            const theme::Rgb edge = hot ? theme::kAccent : is_quit ? theme::kBorderQuit : theme::kBorderCtl;
            draw::box_a(out, view_w, view_h, r.x0, r.y0, r.x1, r.y1, edge.r, edge.g, edge.b, 255);
            const theme::Rgb ink = hot ? theme::kTextHi : theme::kTextBody;
            draw::text(out, view_w, view_h, r.x0 + (r.x1 - r.x0 - draw::text_w(labels[i], 2)) / 2,
                       r.y0 + (r.y1 - r.y0 - (sized ? label_px : ui::kFontH * 2)) / 2, labels[i], 2, ink.r, ink.g, ink.b);
        }
        draw::set_forced_font_px(saved_pause_px);
    }
}

}
