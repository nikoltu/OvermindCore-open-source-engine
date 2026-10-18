#include <set>
#include <tuple>
#include "sc2/render/frame_prof.hpp"
#include "sc2/render/scene_renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

#include "sc2/casc/storage.hpp"
#include "sc2/data/environment.hpp"
#include "sc2/render/shadow_fit.hpp"
#include "sc2/formats/dds.hpp"
#include "sc2/render/anim.hpp"
#include "sc2/render/terrain_tex.hpp"

static const uint32_t kSceneVertSpv[] =
#include "scene.vert.spv.h"
    ;
static const uint32_t kSceneFragSpv[] =
#include "scene.frag.spv.h"
    ;
static const uint32_t kShadowVertSpv[] =
#include "shadow.vert.spv.h"
    ;
static const uint32_t kShadowFragSpv[] =
#include "shadow.frag.spv.h"
    ;
static const uint32_t kParticleVertSpv[] =
#include "particle.vert.spv.h"
    ;
static const uint32_t kParticleFragSpv[] =
#include "particle.frag.spv.h"
    ;
static const uint32_t kOverlayVertSpv[] =
#include "overlay.vert.spv.h"
    ;
static const uint32_t kTonemapFragSpv[] =
#include "tonemap.frag.spv.h"
    ;
static const uint32_t kSsaoFragSpv[] =
#include "ssao.frag.spv.h"
    ;
static const uint32_t kBloomFragSpv[] =
#include "bloom.frag.spv.h"
    ;
static const uint32_t kTerrainSplatFragSpv[] =
#include "terrain_splat.frag.spv.h"
    ;

namespace sc2::render {

namespace {
struct PoseKey {
    const void* model = nullptr;
    int seq = -1;
    std::uint32_t time_bits = 0;
    int turret_bone = -1;
    std::uint32_t yaw_bits = 0;
    bool operator<(const PoseKey& o) const {
        if (model != o.model) return model < o.model;
        if (seq != o.seq) return seq < o.seq;
        if (time_bits != o.time_bits) return time_bits < o.time_bits;
        if (turret_bone != o.turret_bone) return turret_bone < o.turret_bone;
        return yaw_bits < o.yaw_bits;
    }
};
inline std::uint32_t float_bits(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    return u;
}
}

namespace {
Vec3 mul_point(const Mat4& m, const Vec3& p) {
    return {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
            m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
            m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}
struct TonemapPC {
    float exposure;
    int32_t curve;
    int32_t keep_alpha;
    float white_point;
    float col_blend, col_contrast, col_hue, col_saturation, col_lightness;
    float bloom_threshold, bloom_gain, bloom_scale;
    int32_t hdr_operator;
    int32_t fog_on;
    int32_t pad_[2];
    float fog_from_ndc[16];
};
static bool material_hidden(const formats::Model& m, int mi) {
    static const char* kHide = std::getenv("SC2_HIDE_MATERIAL");
    if (!kHide || mi < 0 || std::size_t(mi) >= m.materials.size()) return false;
    const std::string& nm = m.materials[std::size_t(mi)].name;
    const std::string list = std::string(",") + kHide + ",";
    return list.find("," + nm + ",") != std::string::npos;
}

static bool material_unshadowed(const formats::Model& m, int mi) {
    static const char* kList = std::getenv("SC2_NO_SHADOW_MATERIAL");
    if (!kList || mi < 0 || std::size_t(mi) >= m.materials.size()) return false;
    const std::string list = std::string(",") + kList + ",";
    return list.find("," + m.materials[std::size_t(mi)].name + ",") != std::string::npos;
}

static bool fog_in_scene() {
    static const bool on = std::getenv("SC2_FOG_IN_SCENE") != nullptr;
    return on;
}
static bool invert4(const float* m, float* out) {
    double a[16], inv[16];
    for (int i = 0; i < 16; ++i) a[i] = double(m[i]);
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < 1e-30) return false;
    for (int i = 0; i < 16; ++i) out[i] = float(inv[i] / det);
    return true;
}
static_assert(sizeof(TonemapPC) == 128 && offsetof(TonemapPC, fog_from_ndc) == 64, "tonemap.frag push-constant block");
constexpr std::size_t kMaxFrameLights = 64;
struct GlobalsUBO {
    float viewProj[16];
    float lightVP[16];
    float shadowParams[4];
    float eyePos[4];
    float lightDir[4];
    float terrainExtent[4];
    float rigAmbient[4];
    float rigDir[3][4];
    float rigColor[3][4];
    float rigParams[4];
    float fogExtent[4];
    float lightPos[kMaxFrameLights][4];
    float lightCol[kMaxFrameLights][4];
    float lightMeta[4];
    float screen[4];
    float shadowExtra[4];
    float heightFog[4];
    float heightFogColor[4];
    float creepParams[4];
    float terrainColorize[4];
    float terrainColorize2[4];
    float keySpec[4];
    float lightSpot[kMaxFrameLights][4];
    float lightCone[kMaxFrameLights][4];
    float terrainHeight[8][4];
    float rigSpec[3][4];
};

std::vector<Mat4> instance_bones_world(const SceneInstance& inst, const std::vector<Mat4>& bw) {
    std::vector<Mat4> out(bw.size());
    for (size_t i = 0; i < bw.size(); ++i) out[i] = inst.world * bw[i];
    const formats::Model& m = *inst.model;
    if (inst.beam_bone >= 0 && size_t(inst.beam_bone) < out.size() && m.bones.size() == out.size()) {
        const Mat4& a = out[size_t(inst.beam_bone)];
        const float d[3] = {inst.beam_target[0] - a.m[12], inst.beam_target[1] - a.m[13], inst.beam_target[2] - a.m[14]};
        for (size_t i = 0; i < out.size(); ++i) {
            int p = int(i);
            for (size_t guard = 0; p >= 0 && guard <= out.size(); ++guard) {
                if (p == inst.beam_bone) {
                    out[i].m[12] += d[0]; out[i].m[13] += d[1]; out[i].m[14] += d[2];
                    break;
                }
                p = size_t(p) < m.bones.size() ? m.bones[size_t(p)].parent : -1;
            }
        }
    }
    return out;
}

void particle_rng(uint32_t emitter, int particle, float& u1, float& u2, float& u3) {
    uint64_t h = uint64_t(emitter) * 0x9E3779B97F4A7C15ull ^
                 (uint64_t(uint32_t(particle)) + 0x632BE59BD9B4E019ull);
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    u1 = float(h & 0xFFFFF) / 1048576.0f;
    u2 = float((h >> 20) & 0xFFFFF) / 1048576.0f;
    u3 = float((h >> 40) & 0xFFFFF) / 1048576.0f;
}

}

void SceneRenderer::apply_environment(Tunables& tun, const data::EnvLight& env) {
    if (env.has_key_dir) {
        const float fx = std::getenv("SC2_KEY_FLIPXY") ? -1.0f : 1.0f;
        const Vec3 d{fx * float(env.key_dir[0]), fx * float(env.key_dir[1]), float(env.key_dir[2])};
        if (d.x * d.x + d.y * d.y + d.z * d.z > 1e-9f) tun.light_dir = normalize(d);
    }
    tun.gamma_space = env.has_key_dir && !std::getenv("SC2_LINEAR_PIPELINE");
    if (env.has_hdr && env.hdr_exposure > 0.0) tun.exposure = tun.gamma_space ? float(env.hdr_exposure) : 1.0f;
    if (const char* ex = std::getenv("SC2_EXPOSURE")) tun.exposure = float(std::atof(ex));
    static const bool kUnauthoredLinear = std::getenv("SC2_HDR_UNAUTHORED_LINEAR") != nullptr;
    tun.hdr_operator = (tun.gamma_space && kUnauthoredLinear) ? 3 : 0;
    if (tun.gamma_space && !kUnauthoredLinear && !std::getenv("SC2_HDR_UNAUTHORED_REINHARD")) tun.hdr_operator = 1;
    const bool operator_authored = env.params.find("OperatorHDR") != env.params.end();
    if (auto it = env.params.find("OperatorHDR"); it != env.params.end()) tun.hdr_operator = int32_t(it->second + 0.5);
    if (const char* op = std::getenv("SC2_HDR_OPERATOR")) tun.hdr_operator = std::atoi(op);
    const bool exposure_as_authored = env.params.count("ExposureAsAuthored") != 0;
    if (env.params.count("NoShadows") != 0 && !std::getenv("SC2_CONSOLE_SHADOWS")) tun.shadow_strength = 0.0f;
    tun.spec_no_energy_dim = env.params.count("SpecNoEnergyDim") != 0 && !std::getenv("SC2_CONSOLE_SPEC_DIM");
    static const double kUnauthoredDiv = std::getenv("SC2_HDR_UNAUTHORED_DIV") ? std::atof(std::getenv("SC2_HDR_UNAUTHORED_DIV")) : 1.06;
    static const double kFilmicDiv = std::getenv("SC2_HDR_FILMIC_DIV") ? std::atof(std::getenv("SC2_HDR_FILMIC_DIV")) : 1.0;
    static const double kExponentialDiv = std::getenv("SC2_HDR_EXPONENTIAL_DIV") ? std::atof(std::getenv("SC2_HDR_EXPONENTIAL_DIV")) : 1.0;
    const bool unauthored_reinhard = !operator_authored && !kUnauthoredLinear && tun.hdr_operator == 0;
    if (tun.gamma_space && env.has_hdr && env.hdr_exposure > 0.0 && (tun.hdr_operator != 0 || unauthored_reinhard) && !std::getenv("SC2_EXPOSURE"))
        tun.exposure = float(exposure_as_authored ? env.hdr_exposure : env.hdr_exposure / (unauthored_reinhard ? kUnauthoredDiv : tun.hdr_operator == 4 ? kFilmicDiv : kExponentialDiv));
    if (const char* es = std::getenv("SC2_EXPOSURE_SCALE")) tun.exposure *= float(std::atof(es));
    if (std::getenv("SC2_LIGHT_TRACE")) {
        std::fprintf(stderr, "env HDRExposure=%.3f -> exposure %.3f white_point=%.3f\n", env.hdr_exposure, double(tun.exposure), env.hdr_white_point);
        for (const char* k : {"ColorizationBlend", "ColorizationContrast", "ColorizationHue", "ColorizationSaturation", "ColorizationLightness", "HDRGain", "HDRBloomThreshold", "HDRScale"})
            if (auto it = env.params.find(k); it != env.params.end()) std::fprintf(stderr, "env %s=%.4f\n", k, double(it->second));
    }
    if (env.has_hdr && env.hdr_white_point > 0.0) tun.white_point = float(env.hdr_white_point);
    auto param = [&](const char* key, float& dst) {
        auto it = env.params.find(key);
        if (it != env.params.end()) dst = float(it->second);
    };
    if (auto it = env.params.find("Colorize"); it != env.params.end() && it->second > 0.5 && tun.gamma_space) tun.col_blend = 0.5f;
    param("ColorizationBlend", tun.col_blend);
    param("ColorizationContrast", tun.col_contrast);
    param("ColorizationHue", tun.col_hue);
    if (env.has_key_dir) tun.col_saturation = tun.gamma_space ? 0.0f : -100.0f;
    param("ColorizationSaturation", tun.col_saturation);
    param("ColorizationLightness", tun.col_lightness);
    if (auto it = env.params.find("Colorize"); it != env.params.end() && it->second > 0.5 && tun.gamma_space) tun.col_levels = true;
    param("ColorizationBrightness", tun.col_brightness);
    param("ColorizationInputLow", tun.col_input_low);
    param("ColorizationInputHigh", tun.col_input_high);
    param("ColorizationInputGamma", tun.col_input_gamma);
    if (const char* v = std::getenv("SC2_COL_BLEND")) tun.col_blend = float(std::atof(v));
    if (const char* v = std::getenv("SC2_COL_SAT")) tun.col_saturation = float(std::atof(v));
    if (const char* v = std::getenv("SC2_COL_HUE")) tun.col_hue = float(std::atof(v));
    if (!exposure_as_authored && tun.gamma_space && !std::getenv("SC2_SHADOW_HARD")) tun.shadow_penumbra = 0.05f;
    param("ShadowPenumbraSize", tun.shadow_penumbra);
    tun.bloom_threshold = 1.5f;
    if (!exposure_as_authored && tun.gamma_space && !std::getenv("SC2_BLOOM_OLD_DEFAULTS")) {
        tun.bloom_threshold = 1.0f;
        tun.bloom_gain = 1.0f;
        tun.bloom_scale = 1.0f;
    }
    param("HDRBloomThreshold", tun.bloom_threshold);
    param("HDRGain", tun.bloom_gain);
    param("HDRScale", tun.bloom_scale);
    if (const char* v = std::getenv("SC2_BLOOM_GAIN")) tun.bloom_gain = float(std::atof(v));
    if (const char* v = std::getenv("SC2_BLOOM_SCALE")) tun.bloom_scale = float(std::atof(v));
    if (const char* v = std::getenv("SC2_BLOOM_THRESHOLD")) tun.bloom_threshold = float(std::atof(v));
    param("CreepHDRSpecMultiplier", tun.creep_spec_mult);
    param("CreepSpecularExp", tun.creep_spec_exp);
    if (env.has_key_dir) tun.terrain_spec = 1.0f;
    if (!tun.gamma_space || !std::getenv("SC2_NO_TERRAIN_DIFFUSE")) param("TerrainHDRDiffuse", tun.terrain_diffuse);
    if (const char* v = std::getenv("SC2_TERRAIN_DIFFUSE")) tun.terrain_diffuse = float(std::atof(v));
    param("TerrainHDRSpecular", tun.terrain_spec);
    param("TerrainSpecularExp", tun.terrain_spec_exp);
    param("HDRAmbientMultiplier", tun.hdr_ambient_mult);
    if (env.has_ambient && tun.gamma_space) {
        const float top = float(std::max(env.ambient_color[0], std::max(env.ambient_color[1], env.ambient_color[2])));
        if (!std::getenv("SC2_AMBIENT_MULT_AUTHORED") || (tun.hdr_ambient_mult > 1.0f && top * tun.hdr_ambient_mult > 1.0f)) tun.hdr_ambient_mult = 1.0f;
    }
    if (const char* v = std::getenv("SC2_AMBIENT_MULT")) tun.hdr_ambient_mult = float(std::atof(v));
    param("HDRDiffuseMultiplier", tun.hdr_diffuse_mult);
    if (!exposure_as_authored && !std::getenv("SC2_MODEL_HDR_DIFFUSE") && !std::getenv("SC2_HDR_DIFFUSE_ALL")) tun.hdr_diffuse_mult = 1.0f;
    if (const char* v = std::getenv("SC2_HDR_DIFFUSE_MULT")) tun.hdr_diffuse_mult = float(std::atof(v));
    param("HDREmisMultiplier", tun.hdr_emis_mult);
    param("AOOcclusionRadius", tun.ao_radius);
    param("AOOcclusionPower", tun.ao_power);
    param("AONoOcclusion", tun.ao_no_occlusion);
    param("AODetailOcclusionRadius", tun.ao_detail_radius);
    param("AODetailOcclusionPower", tun.ao_detail_power);
    param("AODetailNoOcclusion", tun.ao_detail_no_occlusion);
    param("HDRSpecMultiplier", tun.hdr_spec_mult);
    if (const char* v = std::getenv("SC2_EMIS_MULT")) tun.hdr_emis_mult = float(std::atof(v));
    {
        float en = 0.0f, rgb[3] = {0, 0, 0};
        param("TerrainFogEnabled", en);
        param("TerrainFogColorR", rgb[0]);
        param("TerrainFogColorG", rgb[1]);
        param("TerrainFogColorB", rgb[2]);
        param("TerrainFogDensity", tun.fog_density);
        param("TerrainFogFalloff", tun.fog_falloff);
        param("TerrainFogStartingHeight", tun.fog_start);
        {
            float zoff = 0.0f;
            param("EngineZOffset", zoff);
            tun.fog_start -= zoff;
            const char* v = std::getenv("SC2_FOG_START_ADD");
            tun.fog_start += v ? float(std::atof(v)) : 0.0f;
            if (const char* v = std::getenv("SC2_FOG_DENSITY_MULT")) tun.fog_density *= float(std::atof(v));
            if (const char* v = std::getenv("SC2_FOG_FALLOFF_MULT")) tun.fog_falloff *= float(std::atof(v));
            if (std::getenv("SC2_LIGHT_TRACE"))
                std::fprintf(stderr, "env fog: start %.3f (engine z, offset %.3f) density %.4f falloff %.3f\n", double(tun.fog_start), double(zoff),
                             double(tun.fog_density), double(tun.fog_falloff));
        }
        tun.height_fog = en > 0.5f && tun.fog_density > 0.0f && !std::getenv("SC2_NO_HEIGHT_FOG");
        for (int k = 0; k < 3; ++k) {
            const float c = std::max(0.0f, rgb[k]) / 255.0f;
            tun.fog_color[k] = tun.gamma_space ? c : std::pow(c, 2.2f);
        }
        if (std::getenv("SC2_LIGHT_TRACE"))
        std::fprintf(stderr, "tun colorize blend=%.3f contrast=%.3f hue=%.3f saturation=%.3f lightness=%.3f\n", double(tun.col_blend), double(tun.col_contrast),
                     double(tun.col_hue), double(tun.col_saturation), double(tun.col_lightness));
    frame_prof_count("fog.enabled", tun.height_fog ? 1.0 : 0.0);
        frame_prof_count("fog.density", double(tun.fog_density));
    }
    const LightRig r = rig_from_env(env);
    if (r.present) tun.rig = r;
    frame_prof_count("light.rig_present", r.present ? 1.0 : 0.0);
}

void SceneRenderer::set_environment(const data::EnvLight& env) { apply_environment(tun_, env); }

void SceneRenderer::one_time(const std::function<void(VkCommandBuffer)>& rec) {
    VkCommandBufferAllocateInfo ab{};
    ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ab.commandPool = cmdPool_.get();
    ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ab.commandBufferCount = 1;
    VkCommandBuffer cb;
    check(vkAllocateCommandBuffers(dev_, &ab, &cb), "vkAllocateCommandBuffers(one_time)");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer(one_time)");
    rec(cb);
    check(vkEndCommandBuffer(cb), "vkEndCommandBuffer(one_time)");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    check(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit(one_time)");
    check(vkQueueWaitIdle(queue_), "vkQueueWaitIdle(one_time)");
    vkFreeCommandBuffers(dev_, cmdPool_.get(), 1, &cb);
}

void SceneRenderer::create_targets() {
    VkDevice dev = dev_;
    hdrColor_ = make_image(pd_, dev_, kHdr,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, width_, height_);
    static const bool kHalfCopy = std::getenv("SC2_SCENE_COPY_HALF") != nullptr;
    copy_w_ = kHalfCopy ? (width_ > 1 ? width_ / 2 : 1) : width_;
    copy_h_ = kHalfCopy ? (height_ > 1 ? height_ / 2 : 1) : height_;
    sceneCopy_ = make_image(pd_, dev_, kHdr, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, copy_w_, copy_h_);
    one_time([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.image = sceneCopy_.img.get();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
    if (global_set_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo sci{fogSampler_.get(), sceneCopy_.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet sw = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        sw.dstSet = global_set_;
        sw.dstBinding = 4;
        sw.descriptorCount = 1;
        sw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sw.pImageInfo = &sci;
        vkUpdateDescriptorSets(dev_, 1, &sw, 0, nullptr);
    }
    ldrColor_ = make_image(pd_, dev_, ldr_format_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, width_, height_);
    depth_ = make_image(pd_, dev_, kDepthF, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT, width_, height_);
    depthCopy_ = make_image(pd_, dev_, kDepthF, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, width_, height_);
    one_time([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = depthCopy_.img.get();
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
    if (global_set_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo dci{fogSampler_.get(), depthCopy_.view.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet dw = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        dw.dstSet = global_set_;
        dw.dstBinding = 6;
        dw.descriptorCount = 1;
        dw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dw.pImageInfo = &dci;
        vkUpdateDescriptorSets(dev_, 1, &dw, 0, nullptr);
    }
    {
        VkImageView fbViews[2] = {hdrColor_.view.get(), depth_.view.get()};
        VkFramebufferCreateInfo fbi{};
        fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbi.renderPass = litPass_.get();
        fbi.attachmentCount = 2;
        fbi.pAttachments = fbViews;
        fbi.width = width_;
        fbi.height = height_;
        fbi.layers = 1;
        VkFramebuffer rawFb = VK_NULL_HANDLE;
        check(vkCreateFramebuffer(dev_, &fbi, nullptr, &rawFb), "vkCreateFramebuffer(lit)");
        litFb_ = Unique<VkFramebuffer>(rawFb, [dev](VkFramebuffer f) { vkDestroyFramebuffer(dev, f, nullptr); });
    }
    {
        VkImageView tmView = ldrColor_.view.get();
        VkFramebufferCreateInfo tmfbi{};
        tmfbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        tmfbi.renderPass = tmPass_.get();
        tmfbi.attachmentCount = 1;
        tmfbi.pAttachments = &tmView;
        tmfbi.width = width_;
        tmfbi.height = height_;
        tmfbi.layers = 1;
        VkFramebuffer rawTmFb = VK_NULL_HANDLE;
        check(vkCreateFramebuffer(dev_, &tmfbi, nullptr, &rawTmFb), "vkCreateFramebuffer(tonemap)");
        tmFb_ = Unique<VkFramebuffer>(rawTmFb, [dev](VkFramebuffer f) { vkDestroyFramebuffer(dev, f, nullptr); });
    }
    {
        VkImageView aoView = hdrColor_.view.get();
        VkFramebufferCreateInfo aofbi{};
        aofbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        aofbi.renderPass = aoPass_.get();
        aofbi.attachmentCount = 1;
        aofbi.pAttachments = &aoView;
        aofbi.width = width_;
        aofbi.height = height_;
        aofbi.layers = 1;
        VkFramebuffer rawAoFb = VK_NULL_HANDLE;
        check(vkCreateFramebuffer(dev_, &aofbi, nullptr, &rawAoFb), "vkCreateFramebuffer(ssao)");
        aoFb_ = Unique<VkFramebuffer>(rawAoFb, [dev](VkFramebuffer f) { vkDestroyFramebuffer(dev, f, nullptr); });
    }
    {
        bloom_w_ = width_ >= 4 ? width_ / 4 : 1;
        bloom_h_ = height_ >= 4 ? height_ / 4 : 1;
        bloomA_ = make_image(pd_, dev_, kHdr, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, bloom_w_, bloom_h_);
        bloomB_ = make_image(pd_, dev_, kHdr, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, bloom_w_, bloom_h_);
        one_time([&](VkCommandBuffer cb) {
            VkImageMemoryBarrier b[2]{};
            for (int k = 0; k < 2; ++k) {
                b[k].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b[k].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                b[k].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b[k].srcQueueFamilyIndex = b[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b[k].image = k == 0 ? bloomA_.img.get() : bloomB_.img.get();
                b[k].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                b[k].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            }
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
        });
        for (int k = 0; k < 2; ++k) {
            VkImageView bv = k == 0 ? bloomA_.view.get() : bloomB_.view.get();
            VkFramebufferCreateInfo bfi{};
            bfi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            bfi.renderPass = bloomPass_.get();
            bfi.attachmentCount = 1;
            bfi.pAttachments = &bv;
            bfi.width = bloom_w_;
            bfi.height = bloom_h_;
            bfi.layers = 1;
            VkFramebuffer rawBf = VK_NULL_HANDLE;
            check(vkCreateFramebuffer(dev_, &bfi, nullptr, &rawBf), "vkCreateFramebuffer(bloom)");
            (k == 0 ? bloomFbA_ : bloomFbB_) = Unique<VkFramebuffer>(rawBf, [dev](VkFramebuffer f) { vkDestroyFramebuffer(dev, f, nullptr); });
        }
        const VkImageView srcv[3] = {hdrColor_.view.get(), bloomA_.view.get(), bloomB_.view.get()};
        for (int k = 0; k < 3; ++k) {
            VkDescriptorImageInfo bi{fogSampler_.get(), srcv[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet bw = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            bw.dstSet = bloom_set_[k];
            bw.dstBinding = 0;
            bw.descriptorCount = 1;
            bw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bw.pImageInfo = &bi;
            vkUpdateDescriptorSets(dev_, 1, &bw, 0, nullptr);
        }
        VkDescriptorImageInfo ti{fogSampler_.get(), bloomA_.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet tw = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        tw.dstSet = tm_set_;
        tw.dstBinding = 3;
        tw.descriptorCount = 1;
        tw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tw.pImageInfo = &ti;
        vkUpdateDescriptorSets(dev_, 1, &tw, 0, nullptr);
    }
    VkDescriptorImageInfo dii{sampler_.get(), hdrColor_.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = tm_set_;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkDescriptorImageInfo ddi{sampler_.get(), depth_.view.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet dw = w;
    dw.dstBinding = 1;
    dw.pImageInfo = &ddi;
    vkUpdateDescriptorSets(dev_, 1, &dw, 0, nullptr);
}

void SceneRenderer::resize(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || (w == width_ && h == height_)) return;
    vkDeviceWaitIdle(dev_);
    width_ = w;
    height_ = h;
    create_targets();
}

void SceneRenderer::flush_pending_fog(VkCommandBuffer cmd) {
    if (fog_pending_.empty()) return;
    const uint32_t w = fog_pending_w_, h = fog_pending_h_;
    const bool recreate = !fogImg_.img.get() || w != fog_w_ || h != fog_h_;
    if (recreate) {
        fogImg_ = make_image(pd_, dev_, kTexColor,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, w, h);
        fog_w_ = w;
        fog_h_ = h;
    }
    fog_stage_ = make_buffer(pd_, dev_, fog_pending_.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             fog_pending_.data());
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image = fogImg_.img.get();
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rgn.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, fog_stage_.buf.get(), fogImg_.img.get(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    if (recreate && global_set_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo fgi{fogSampler_.get(), fogImg_.view.get(),
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr{};
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = global_set_;
        wr.dstBinding = 2;
        wr.descriptorCount = 1;
        if (tm_set_ != VK_NULL_HANDLE) {
            VkWriteDescriptorSet tw{};
            tw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            tw.dstSet = tm_set_;
            tw.dstBinding = 2;
            tw.descriptorCount = 1;
            tw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            tw.pImageInfo = &fgi;
            vkUpdateDescriptorSets(dev_, 1, &tw, 0, nullptr);
        }
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &fgi;
        vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
    }
    fog_pending_.clear();
}

void SceneRenderer::upload_fog_image(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    const bool recreate = !fogImg_.img.get() || w != fog_w_ || h != fog_h_;
    if (recreate) {
        fogImg_ = make_image(pd_, dev_, kTexColor, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, w, h);
        fog_w_ = w;
        fog_h_ = h;
    }
    Buffer stg = make_buffer(pd_, dev_, rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, const_cast<uint8_t*>(rgba.data()));
    one_time([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = fogImg_.img.get();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy rgn{};
        rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rgn.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cb, stg.buf.get(), fogImg_.img.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
    if (recreate && global_set_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo fgi{fogSampler_.get(), fogImg_.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet gw = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        gw.dstSet = global_set_;
        gw.dstBinding = 2;
        gw.descriptorCount = 1;
        gw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        gw.pImageInfo = &fgi;
        vkUpdateDescriptorSets(dev_, 1, &gw, 0, nullptr);
        if (tm_set_ != VK_NULL_HANDLE) {
            gw.dstSet = tm_set_;
            vkUpdateDescriptorSets(dev_, 1, &gw, 0, nullptr);
        }
    }
}

void SceneRenderer::ensure_fx_capacity(uint32_t records) {
    if (records <= fx_cap_ && fxBuf_.buf.get()) return;
    uint32_t cap = fx_cap_ ? fx_cap_ : 64;
    while (cap < records) cap *= 2;
    std::vector<FxRecord> zero(cap, FxRecord{});
    fxBuf_ = make_buffer(pd_, dev_, VkDeviceSize(cap) * sizeof(FxRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, zero.data());
    fx_cap_ = cap;
    if (global_set_ != VK_NULL_HANDLE) {
        VkDescriptorBufferInfo fbi{fxBuf_.buf.get(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet fw = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        fw.dstSet = global_set_;
        fw.dstBinding = 3;
        fw.descriptorCount = 1;
        fw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        fw.pBufferInfo = &fbi;
        vkUpdateDescriptorSets(dev_, 1, &fw, 0, nullptr);
    }
}

void SceneRenderer::upload_creep_image(uint32_t w, uint32_t h, const std::vector<uint8_t>& r8) {
    const bool recreate = !creepImg_.img.get() || w != creep_w_ || h != creep_h_;
    if (recreate) {
        creepImg_ = make_image(pd_, dev_, kTexColor, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, w, h);
        creep_w_ = w;
        creep_h_ = h;
    }
    std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
    for (size_t i = 0; i < r8.size() && i < size_t(w) * h; ++i) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4 + 3] = r8[i];
    Buffer stg = make_buffer(pd_, dev_, rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rgba.data());
    one_time([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = creepImg_.img.get();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy rgn{};
        rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rgn.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cb, stg.buf.get(), creepImg_.img.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
    if (recreate && global_set_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo ci{fogSampler_.get(), creepImg_.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet cw = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        cw.dstSet = global_set_;
        cw.dstBinding = 5;
        cw.descriptorCount = 1;
        cw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        cw.pImageInfo = &ci;
        vkUpdateDescriptorSets(dev_, 1, &cw, 0, nullptr);
    }
}

float SceneRenderer::fog_alpha_at(float x, float y) const {
    if (!fog_in_scene()) return 0.0f;
    if (!fog_present_ || fog_cpu_w_ == 0 || fog_cpu_h_ == 0 || fog_world_w_ <= 0.0f || fog_world_h_ <= 0.0f ||
        fog_last_.size() < std::size_t(fog_cpu_w_) * fog_cpu_h_)
        return 0.0f;
    const float u = std::min(std::max(x / fog_world_w_, 0.0f), 1.0f) * float(fog_cpu_w_) - 0.5f;
    const float v = std::min(std::max(y / fog_world_h_, 0.0f), 1.0f) * float(fog_cpu_h_) - 0.5f;
    const int x0 = std::min(std::max(int(std::floor(u)), 0), int(fog_cpu_w_) - 1), x1 = std::min(x0 + 1, int(fog_cpu_w_) - 1);
    const int y0 = std::min(std::max(int(std::floor(v)), 0), int(fog_cpu_h_) - 1), y1 = std::min(y0 + 1, int(fog_cpu_h_) - 1);
    const float fx = std::min(std::max(u - float(x0), 0.0f), 1.0f), fy = std::min(std::max(v - float(y0), 0.0f), 1.0f);
    auto at = [&](int cx, int cy) { return float(fog_last_[std::size_t(cy) * fog_cpu_w_ + std::size_t(cx)]) / 255.0f; };
    return (at(x0, y0) * (1.0f - fx) + at(x1, y0) * fx) * (1.0f - fy) + (at(x0, y1) * (1.0f - fx) + at(x1, y1) * fx) * fy;
}

void SceneRenderer::set_fog(const FogField& fog) {
    SC2_PROF_SCOPE("set_fog");
    if (!fog.valid()) {
        if (fog_present_) upload_fog_image(1, 1, {0, 0, 0, 0});
        fog_present_ = false;
        fog_last_.clear();
        return;
    }
    fog_world_w_ = fog.world_w;
    fog_world_h_ = fog.world_h;
    fog_present_ = true;
    if (fog.w == fog_w_ && fog.h == fog_h_ && fog.alpha == fog_last_) return;
    fog_last_ = fog.alpha;
    fog_cpu_w_ = fog.w;
    fog_cpu_h_ = fog.h;
    std::vector<uint8_t> rgba(std::size_t(fog.w) * fog.h * 4);
    for (std::size_t i = 0; i < fog.alpha.size(); ++i)
        rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4 + 3] = fog.alpha[i];
    fog_pending_ = std::move(rgba);
    fog_pending_w_ = fog.w;
    fog_pending_h_ = fog.h;
}

int SceneRenderer::upload_texture(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba, bool srgb) {
    if (w == 0 || h == 0 || w > 16384 || h > 16384 || rgba.size() < size_t(w) * h * 4)
        throw std::runtime_error("upload_texture: rgba bytes do not cover w*h*4");
    int idx = int(textures_.size());
    uint32_t mips = 1;
    for (uint32_t d = (w > h ? w : h); d > 1; d >>= 1) ++mips;
    Image img = make_image(pd_, dev_, srgb ? kTexColorSrgb : kTexColor,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, w, h, mips);
    Buffer stg = make_buffer(pd_, dev_, rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             const_cast<uint8_t*>(rgba.data()));
    one_time([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = img.img.get();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy rgn{};
        rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rgn.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cb, stg.buf.get(), img.img.get(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
        int32_t mw = int32_t(w), mh = int32_t(h);
        for (uint32_t lv = 1; lv < mips; ++lv) {
            VkImageMemoryBarrier pb{};
            pb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            pb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            pb.image = img.img.get();
            pb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, lv - 1, 1, 0, 1};
            pb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            pb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &pb);
            const int32_t nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
            VkImageBlit bl{};
            bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv - 1, 0, 1};
            bl.srcOffsets[1] = {mw, mh, 1};
            bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, 1};
            bl.dstOffsets[1] = {nw, nh, 1};
            vkCmdBlitImage(cb, img.img.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.img.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
            pb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            pb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            pb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            pb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &pb);
            mw = nw;
            mh = nh;
        }
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mips - 1, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    });
    textures_.push_back(std::move(img));
    return idx;
}

int SceneRenderer::resolve_texture(const formats::Model& m, int material, const char* usage,
                                   int def) {
    const std::string u = usage ? usage : "";
    static const bool kSpecSrgb = std::getenv("SC2_SPEC_SRGB") != nullptr;
    const bool srgb = u == "Diffuse" || u == "Emissive" || u == "Decal" || u == "LightMap" || (kSpecSrgb && u == "Specular");
    if (const formats::M3Layer* layer = layer_ptr_of(m, material, usage)) {
        if (!layer->inline_rgba.empty() && layer->inline_w && layer->inline_h &&
            layer->inline_w <= 16384 && layer->inline_h <= 16384 &&
            layer->inline_rgba.size() >= size_t(layer->inline_w) * layer->inline_h * 4) {
            auto it = inline_index_.find(layer);
            if (it != inline_index_.end()) return it->second;
            int idx = upload_texture(layer->inline_w, layer->inline_h, layer->inline_rgba, srgb);
            inline_index_[layer] = idx;
            return idx;
        }
    }
    std::string path = layer_of(m, material, usage);
    if (path.empty() || !storage_) return def;
    bool triplanar = false;
    if ((u == "Diffuse" || ((u == "Normal" || u == "Specular") && std::getenv("SC2_TRIPLANAR_NORMAL") && !std::getenv("SC2_TRIPLANAR_DIFFUSE_ONLY"))) && !std::getenv("SC2_NO_TRIPLANAR"))
        if (const formats::M3Layer* tl = layer_ptr_of(m, material, usage))
            triplanar = !tl->raw_record.empty() && tl->word40 >= 16 && tl->word40 <= 18;
    const std::string cache_key = (srgb ? path + "#srgb" : path) + (triplanar ? "#tri" : "");
    auto it = tex_index_.find(cache_key);
    if (it != tex_index_.end()) return it->second;
    int idx = def;
    std::vector<std::string> roots = {texture_root_};
    for (const char* r : {"mods/liberty.sc2mod/base.sc2assets/", "mods/core.sc2mod/base.sc2assets/",
                          "mods/void.sc2mod/base.sc2assets/", "mods/swarm.sc2mod/base.sc2assets/", "",
                          "mods/liberty.sc2mod/enus.sc2assets/", "mods/swarm.sc2mod/enus.sc2assets/",
                          "mods/void.sc2mod/enus.sc2assets/", "mods/core.sc2mod/enus.sc2assets/"})
        roots.push_back(r);
    for (const std::string& root : roots) {
        try {
            auto bytes = storage_->read_by_name(root + path);
            formats::DdsInfo info;
            auto rgba = formats::dds_decode_rgba(bytes, info);
            if (!rgba.empty() && info.width && info.height) {
                if (u == "Normal") dxt5nm_to_rgb(rgba);
                if (triplanar) {
                    std::string sib = path;
                    const std::size_t at = sib.rfind("_00.");
                    bool built = false;
                    if (at != std::string::npos) {
                        sib.replace(at, 3, "_01");
                        try {
                            formats::DdsInfo si;
                            auto top = formats::dds_decode_rgba(storage_->read_by_name(root + sib), si);
                            if (!top.empty() && si.width == info.width && si.height == info.height) {
                                if (u == "Normal") dxt5nm_to_rgb(top);
                                const std::size_t W = info.width, H = info.height;
                                std::vector<uint8_t> atlas(W * 2 * H * 4);
                                for (std::size_t y = 0; y < H; ++y) {
                                    std::memcpy(&atlas[y * W * 2 * 4], &top[y * W * 4], W * 4);
                                    std::memcpy(&atlas[(y * W * 2 + W) * 4], &rgba[y * W * 4], W * 4);
                                }
                                idx = upload_texture(std::uint32_t(W * 2), std::uint32_t(H), atlas, srgb);
                                built = true;
                            }
                        } catch (const std::exception&) {}
                    }
                    if (!built) {
                        const std::size_t W = info.width, H = info.height;
                        std::vector<uint8_t> atlas(W * 2 * H * 4);
                        for (std::size_t y = 0; y < H; ++y) {
                            std::memcpy(&atlas[y * W * 2 * 4], &rgba[y * W * 4], W * 4);
                            std::memcpy(&atlas[(y * W * 2 + W) * 4], &rgba[y * W * 4], W * 4);
                        }
                        idx = upload_texture(std::uint32_t(W * 2), std::uint32_t(H), atlas, srgb);
                    }
                    break;
                }
                idx = upload_texture(info.width, info.height, rgba, srgb);
                break;
            }
        } catch (const std::exception&) {   }
    }
    if (idx == def && std::getenv("SC2_TEX_TRACE")) std::fprintf(stderr, "texmiss %s %s\n", usage ? usage : "", path.c_str());
    tex_index_[cache_key] = idx;
    return idx;
}

int SceneRenderer::resolve_env_texture(const formats::Model& m, int material) {
    const std::string path = layer_of(m, material, "EnvironmentVIO");
    if (path.empty() || !storage_) return kDefaultEmissive;
    const std::string key = path + "#envll";
    if (auto it = tex_index_.find(key); it != tex_index_.end()) return it->second;
    int idx = kDefaultEmissive;
    std::vector<std::string> roots = {texture_root_};
    for (const char* r : {"mods/liberty.sc2mod/base.sc2assets/", "mods/core.sc2mod/base.sc2assets/",
                          "mods/void.sc2mod/base.sc2assets/", "mods/swarm.sc2mod/base.sc2assets/", ""})
        roots.push_back(r);
    for (const std::string& root : roots) {
        try {
            const std::vector<uint8_t> bytes = storage_->read_by_name(root + path);
            if (bytes.size() < 128 + 6) continue;
            const uint32_t caps2 = uint32_t(bytes[112]) | uint32_t(bytes[113]) << 8 | uint32_t(bytes[114]) << 16 | uint32_t(bytes[115]) << 24;
            const bool dx10 = bytes[84] == 'D' && bytes[85] == 'X' && bytes[86] == '1' && bytes[87] == '0';
            if (dx10) break;
            if (!(caps2 & 0x200u)) {
                if (std::getenv("SC2_NO_SPHERE_ENVIO")) break;
                formats::DdsInfo info;
                const std::vector<uint8_t> rgba = formats::dds_decode_rgba(bytes, info);
                if (!rgba.empty() && info.width && info.height) {
                    idx = upload_texture(info.width, info.height, rgba, true);
                    env_sphere_tex_.insert(idx);
                }
                break;
            }
            const size_t face = (bytes.size() - 128) / 6;
            std::vector<uint8_t> faces[6];
            uint32_t fw = 0, fh = 0;
            bool ok = true;
            for (int k = 0; k < 6 && ok; ++k) {
                std::vector<uint8_t> one(128 + face);
                std::memcpy(one.data(), bytes.data(), 128);
                std::memcpy(one.data() + 128, bytes.data() + 128 + size_t(k) * face, face);
                one[112] = one[113] = one[114] = one[115] = 0;
                formats::DdsInfo info;
                faces[k] = formats::dds_decode_rgba(one, info);
                if (faces[k].empty() || info.width == 0 || info.height == 0 || (k && (info.width != fw || info.height != fh))) ok = false;
                fw = info.width;
                fh = info.height;
            }
            if (!ok) break;
            const uint32_t W = 512, H = 256;
            std::vector<uint8_t> ll(size_t(W) * H * 4, 255);
            for (uint32_t j = 0; j < H; ++j)
                for (uint32_t i = 0; i < W; ++i) {
                    const float ph = ((float(i) + 0.5f) / float(W) - 0.5f) * 6.2831853f;
                    const float th = (float(j) + 0.5f) / float(H) * 3.14159265f;
                    const float x = std::sin(th) * std::cos(ph), y = std::sin(th) * std::sin(ph), z = std::cos(th);
                    const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
                    int f = 0;
                    float sc = 0, tc = 0, ma = 1;
                    if (ax >= ay && ax >= az) { f = x > 0 ? 0 : 1; sc = x > 0 ? -z : z; tc = -y; ma = ax; }
                    else if (ay >= az) { f = y > 0 ? 2 : 3; sc = x; tc = y > 0 ? z : -z; ma = ay; }
                    else { f = z > 0 ? 4 : 5; sc = z > 0 ? x : -x; tc = -y; ma = az; }
                    const uint32_t px = std::min(fw - 1, uint32_t(std::max(0.0f, (sc / ma * 0.5f + 0.5f) * float(fw))));
                    const uint32_t py = std::min(fh - 1, uint32_t(std::max(0.0f, (tc / ma * 0.5f + 0.5f) * float(fh))));
                    const uint8_t* s = &faces[f][(size_t(py) * fw + px) * 4];
                    uint8_t* d = &ll[(size_t(j) * W + i) * 4];
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                }
            idx = upload_texture(W, H, ll, true);
            break;
        } catch (const std::exception&) {   }
    }
    if (idx == kDefaultEmissive && std::getenv("SC2_TEX_TRACE")) std::fprintf(stderr, "envmiss %s\n", path.c_str());
    tex_index_[key] = idx;
    return idx;
}

static float diffuse_layer_packed(const formats::Model& m, int mi, float mult) {
    static const bool kOff = std::getenv("SC2_DIFFUSE_ADD_OFF") != nullptr;
    mult = std::min(std::max(mult, 0.0f), 999.0f);
    if (kOff || mi < 0 || std::size_t(mi) >= m.materials.size()) return mult;
    for (const formats::M3Layer& L : m.materials[std::size_t(mi)].layers)
        if (L.usage == "Diffuse") {
            if (L.raw_record.empty() || L.word44 != 0) return mult;
            const float add = L.anim.midtone_offset;
            const float a = std::min(std::max(mult + add, 0.0f), 1.0f);
            return std::min(mult * a, 999.0f) + 1000.0f * std::floor(std::min(std::max(add * a, 0.0f), 1.0f) * 100.0f + 0.5f);
        }
    return mult;
}

static const ClipLayers* glayers(const SceneInstance& inst) {
    static const bool kOff = std::getenv("SC2_NO_GLOBAL_LAYER_EXTRAS") != nullptr;
    return kOff || inst.global_layers.empty() ? nullptr : &inst.global_layers;
}

static void material_anim(const formats::Model& m, int mi, int seq, float time_ms, float out[4], const ClipLayers* gl = nullptr) {
    out[0] = 0.0f; out[1] = 0.0f; out[2] = 1.0f; out[3] = 1.0f;
    if (mi < 0 || std::size_t(mi) >= m.materials.size()) return;
    const formats::M3Material& mat = m.materials[std::size_t(mi)];
    const uint32_t s = seq >= 0 ? uint32_t(seq) : UINT32_MAX;
    for (const formats::M3Layer& L : mat.layers) {
        const formats::M3Layer::AnimRefs& a = L.anim;
        if (L.usage == "Diffuse") {
            if (seq >= 0) {
                animated_layer_field(m, s, a.uv_offset_interp, a.uv_offset_flags, a.uv_offset_id, time_ms, 2, a.uv_offset, out, gl);
                animated_layer_field(m, s, a.bright_mult_interp, a.bright_mult_flags, a.bright_mult_id, time_ms, 1, &a.bright_mult, out + 3, gl);
            } else {
                out[0] = a.uv_offset[0]; out[1] = a.uv_offset[1]; out[3] = a.bright_mult;
            }
        } else if (L.usage == "Emissive") {
            if (seq >= 0)
                animated_layer_field(m, s, a.bright_mult_interp, a.bright_mult_flags, a.bright_mult_id, time_ms, 1, &a.bright_mult, out + 2, gl);
            else
                out[2] = a.bright_mult;
            if (!L.raw_record.empty() && L.word44 == 0) out[2] *= std::max(out[2] + a.midtone_offset, 0.0f);
            static const bool kTeamClampOld = std::getenv("SC2_EMIS_TEAM_CLAMP_AFTER_MULT") != nullptr;
            if (kTeamClampOld || mat.emis_op != 4 || std::getenv("SC2_NO_EMIS_SELECT"))
                out[2] *= mat.emis_mult;
            static const bool kBrightStatic = std::getenv("SC2_EMIS_BRIGHTNESS_STATIC") != nullptr;
            if (!L.raw_record.empty()) {
                float br = a.brightness;
                if (!kBrightStatic && seq >= 0 && a.brightness_flags) animated_layer_field(m, s, a.brightness_interp, a.brightness_flags, a.brightness_id, time_ms, 1, &a.brightness, &br, gl);
                if (const char* bt = std::getenv("SC2_BRIGHT_TRACE"); bt && mat.name == bt) {
                    static int nb = 0;
                    if (nb++ < 40) std::fprintf(stderr, "[emisbright] %s seq %d t %.0f brightness init %.3f null %.3f now %.3f flags %u | mult init %.3f null %.3f flags %u\n", mat.name.c_str(), seq, double(time_ms), double(a.brightness), double(a.brightness_null), double(br), unsigned(a.brightness_flags), double(a.bright_mult), double(a.bright_mult_null), unsigned(a.bright_mult_flags));
                }
                out[2] *= br;
            }
            static const bool kPosAdd = std::getenv("SC2_NO_EMIS_ADD_POS") == nullptr;
            if (!L.raw_record.empty() && L.word44 == 0 && a.bright_mult > 0.0f && out[2] < 999.0f && !std::getenv("SC2_NO_EMIS_ADD") &&
                (a.midtone_offset < 0.0f || (kPosAdd && a.midtone_offset > 0.0f))) {
                const float k = std::floor(std::max(-400.0f, std::min(100.0f, -a.midtone_offset / a.bright_mult * 100.0f)) + 0.5f);
                out[2] = std::max(out[2], 0.0f) + 1000.0f * k;
            }
        }
    }
    if (const char* bt = std::getenv("SC2_BRIGHT_TRACE"); bt && mat.name == bt) {
        static int n = 0;
        if (n++ < 12)
            for (const formats::M3Layer& L : mat.layers)
                if (L.usage == "Diffuse") {
                    const float none = -777.0f;
                    float got = none;
                    if (seq >= 0) animated_layer_field(m, s, L.anim.bright_mult_interp, L.anim.bright_mult_flags, L.anim.bright_mult_id, time_ms, 1, &none, &got, gl);
                    std::fprintf(stderr, "[layeranim] %s seq %d t %.0f uv %.3f %.3f mult %.3f emissive gain %.3f (op %u, emisMult %.2f) | init mult %.3f flags %u id %08x track %d | uv flags %u\n",
                                 mat.name.c_str(), seq, double(time_ms), double(out[0]), double(out[1]), double(out[3]), double(out[2]), unsigned(mat.emis_op), double(mat.emis_mult),
                                 double(L.anim.bright_mult), unsigned(L.anim.bright_mult_flags), unsigned(L.anim.bright_mult_id), int(got != none), unsigned(L.anim.uv_offset_flags));
                }
    }
    if (std::getenv("SC2_BRIGHT_TRACE") && (out[3] > 1.01f || out[2] > 1.01f))
        std::fprintf(stderr, "bright mat=%s diffuse=%.2f emissive=%.2f\n", mat.name.c_str(), out[3], out[2]);
    if (std::getenv("SC2_NO_BRIGHTMULT")) { out[2] = 1.0f; out[3] = 1.0f; }
}

static bool material_uv_xform(const formats::Model& m, int mi, int seq, float time_ms, float out[3], const ClipLayers* gl = nullptr) {
    out[0] = 1.0f; out[1] = 1.0f; out[2] = 0.0f;
    if (mi < 0 || std::size_t(mi) >= m.materials.size()) return false;
    for (const formats::M3Layer& L : m.materials[std::size_t(mi)].layers) {
        if (L.usage != "Diffuse") continue;
        const formats::M3Layer::AnimRefs& a = L.anim;
        float til[2] = {a.uv_tiling[0], a.uv_tiling[1]};
        float ang[3] = {a.uv_angle[0], a.uv_angle[1], a.uv_angle[2]};
        if (seq >= 0) {
            animated_layer_field(m, uint32_t(seq), a.uv_tiling_interp, a.uv_tiling_flags, a.uv_tiling_id, time_ms, 2, a.uv_tiling, til, gl);
            animated_layer_field(m, uint32_t(seq), a.uv_angle_interp, a.uv_angle_flags, a.uv_angle_id, time_ms, 3, a.uv_angle, ang, gl);
        }
        if (std::isfinite(til[0]) && std::isfinite(til[1]) && (til[0] != 0.0f || til[1] != 0.0f)) { out[0] = til[0]; out[1] = til[1]; }
        if (std::isfinite(ang[2])) out[2] = ang[2];
        break;
    }
    return out[0] != 1.0f || out[1] != 1.0f || out[2] != 0.0f;
}

VkDescriptorSet SceneRenderer::alloc_set(Unique<VkDescriptorPool>& main, std::vector<Unique<VkDescriptorPool>>& extra,
                                         VkDescriptorSetLayout layout, VkDescriptorType type, uint32_t per_set,
                                         uint32_t max_sets, const char* what) {
    VkDescriptorSetAllocateInfo dsa{};
    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    auto try_pool = [&](VkDescriptorPool p) {
        dsa.descriptorPool = p;
        const VkResult r = vkAllocateDescriptorSets(dev_, &dsa, &set);
        if (r == VK_SUCCESS) return true;
        if (r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL) return false;
        check(r, what);
        return false;
    };
    if (try_pool(main.get())) return set;
    for (auto& p : extra)
        if (try_pool(p.get())) return set;
    VkDescriptorPoolSize sz{type, per_set * max_sets};
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = max_sets;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &sz;
    VkDescriptorPool raw = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(dev_, &dpi, nullptr, &raw), "vkCreateDescriptorPool(overflow)");
    const VkDevice dev = dev_;
    extra.emplace_back(raw, [dev](VkDescriptorPool p) { vkDestroyDescriptorPool(dev, p, nullptr); });
    if (!try_pool(raw)) throw std::runtime_error(std::string("vkAllocateDescriptorSets(") + what + "): fresh pool refused");
    return set;
}

SceneRenderer::Mesh& SceneRenderer::ensure_mesh(const formats::Model& m, std::uint64_t version) {
    auto it = mesh_cache_.find(&m);
    if (it != mesh_cache_.end()) {
        if (it->second.version == version) return it->second;
        for (VkDescriptorSet s : it->second.draw_set)
            if (s != VK_NULL_HANDLE) free_mat_sets_.push_back(s);
        mesh_cache_.erase(it);
    }
    Mesh mesh;
    mesh.version = version;
    std::vector<uint32_t> tri = resolve_indices(m, mesh.draws);
    std::vector<Vertex> verts = build_vertices(m, tri);
    mesh.lmin = {1e30f, 1e30f, 1e30f};
    mesh.lmax = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < m.vertex_count; ++i)
        for (int a = 0; a < 3; ++a) {
            const float c = m.positions[i * 3 + a];
            (&mesh.lmin.x)[a] = std::min((&mesh.lmin.x)[a], c);
            (&mesh.lmax.x)[a] = std::max((&mesh.lmax.x)[a], c);
        }
    if (mesh.lmin.x > mesh.lmax.x) { mesh.lmin = {0, 0, 0}; mesh.lmax = {0, 0, 0}; }
    mesh.draw_tex.resize(mesh.draws.size());
    for (size_t i = 0; i < mesh.draws.size(); ++i) {
        int mat = mesh.draws[i].material;
        const bool adds = mat >= 0 && std::size_t(mat) < m.materials.size() && formats::material_is_additive(m.materials[std::size_t(mat)]);
        mesh.draw_tex[i] = {resolve_texture(m, mat, "Diffuse", adds ? kDefaultEmissive : kDefaultWhite),
                            resolve_texture(m, mat, "Normal", kDefaultNormal),
                            resolve_texture(m, mat, "Emissive", kDefaultEmissive),
                            resolve_texture(m, mat, "Specular", kDefaultSpecular),
                            resolve_texture(m, mat, "AlphaMask", kDefaultWhite),
                            resolve_env_texture(m, mat),
                            resolve_texture(m, mat, "EnvironmentVIOMask", kDefaultWhite),
                            std::getenv("SC2_NO_DECAL_LAYER") ? kDefaultClear : resolve_texture(m, mat, "Decal", kDefaultClear),
                            std::getenv("SC2_AO_LAYER") ? resolve_texture(m, mat, "AmbientOcclusion", kDefaultWhite) : kDefaultWhite,
                            resolve_texture(m, mat, "AlphaMask2", kDefaultWhite),
                            std::getenv("SC2_NO_EMISSIVE2") ? kDefaultEmissive : resolve_texture(m, mat, "Emissive2", kDefaultEmissive)};
    }
    mesh.draw_maskx.assign(mesh.draws.size(), {1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    if (!std::getenv("SC2_NO_MASK2"))
        for (size_t i = 0; i < mesh.draws.size(); ++i) {
            const int mat = mesh.draws[i].material;
            if (mat < 0 || std::size_t(mat) >= m.materials.size()) continue;
            std::array<float, 16>& mx = mesh.draw_maskx[i];
            for (const formats::M3Layer& L : m.materials[std::size_t(mat)].layers) {
                if (L.raw_record.empty()) continue;
                const formats::M3Layer::AnimRefs& a = L.anim;
                const bool moved = a.uv_tiling[0] != 1.0f || a.uv_tiling[1] != 1.0f || a.uv_angle[2] != 0.0f;
                static const bool kMaskScreen = std::getenv("SC2_NO_MASK_SCREEN_UV") == nullptr;
                const bool screen = kMaskScreen && L.usage == "AlphaMask" && L.word40 == 15;
                static const bool kMaskClamp = std::getenv("SC2_MASK_ALWAYS_TILED") == nullptr;
                const bool untiled = kMaskClamp && L.usage == "AlphaMask" && !screen && (L.word36 & 0xcu) == 0 && !L.image_path.empty();
                static const bool kMaskOffset = std::getenv("SC2_MASK_NO_OFFSET") == nullptr;
                const bool one_axis = kMaskOffset && L.usage == "AlphaMask" && !screen && !untiled && !L.image_path.empty() && (L.word36 & 0xcu) != 0xcu;
                const bool offset = kMaskOffset && L.usage == "AlphaMask" && !screen && (a.uv_offset[0] != 0.0f || a.uv_offset[1] != 0.0f);
                if (L.usage == "AlphaMask" && (moved || screen || untiled || one_axis || offset) && a.uv_tiling[0] != 0.0f && a.uv_tiling[1] != 0.0f) {
                    mx[0] = a.uv_tiling[0];
                    mx[1] = a.uv_tiling[1];
                    mx[2] = a.uv_angle[2];
                    mx[3] = screen ? 2.0f : untiled ? 3.0f : 1.0f;
                    if (offset) { mx[12] = a.uv_offset[0]; mx[13] = a.uv_offset[1]; }
                    if (one_axis) { mx[14] = (L.word36 & 0x4u) ? 0.0f : 1.0f; mx[15] = (L.word36 & 0x8u) ? 0.0f : 1.0f; }
                    mesh.any_maskx = true;
                }
                if (L.usage == "AlphaMask2" && L.word44 >= 2 && L.word44 <= 5 && mesh.draw_tex[i][9] != kDefaultWhite &&
                    !(a.midtone_offset >= 1.0f && a.bright_mult >= 0.0f)) {
                    mx[4] = a.uv_tiling[0] != 0.0f ? a.uv_tiling[0] : 1.0f;
                    mx[5] = a.uv_tiling[1] != 0.0f ? a.uv_tiling[1] : 1.0f;
                    mx[6] = a.uv_angle[2];
                    mx[7] = float(L.word44 - 1);
                    mx[8] = a.bright_mult;
                    mx[9] = a.midtone_offset;
                    mesh.any_maskx = true;
                }
                static const bool kEmisSrc = std::getenv("SC2_NO_EMIS_UV_SOURCE") == nullptr;
                if (kEmisSrc && L.usage == "Emissive" && a.uv_tiling[0] > 0.0f && a.uv_tiling[1] > 0.0f && a.uv_tiling[0] < 100.0f) {
                    const float plane = L.word40 == 13 ? 1.0f : L.word40 == 14 ? 2.0f : L.word40 == 5 ? 3.0f : 0.0f;
                    const float k = plane > 0.0f ? 0.09375f : 1.0f;
                    if (plane > 0.0f || a.uv_tiling[0] != 1.0f || a.uv_tiling[1] != 1.0f) {
                        mx[10] = a.uv_tiling[0] * k + 16.0f * plane;
                        mx[11] = a.uv_tiling[1] * k;
                        mesh.any_maskx = true;
                    }
                }
            }
        }
    static const bool kFresSE = std::getenv("SC2_NO_LAYER_FRESNEL_SE") == nullptr;
    mesh.draw_fres.assign(mesh.draws.size(), std::array<float, 24>{});
    if (!std::getenv("SC2_NO_LAYER_FRESNEL_DM"))
        for (size_t i = 0; i < mesh.draws.size(); ++i) {
            const int mat = mesh.draws[i].material;
            if (mat < 0 || std::size_t(mat) >= m.materials.size()) continue;
            for (const formats::M3Layer& L : m.materials[std::size_t(mat)].layers) {
                const std::size_t slot = L.usage == "Diffuse" ? 0 : L.usage == "AlphaMask" ? 4 : L.usage == "AlphaMask2" ? 8
                                         : !kFresSE ? 24 : L.usage == "Specular" ? 12 : L.usage == "EnvironmentVIO" ? 16
                                         : L.usage == "EnvironmentVIOMask" ? 20 : 24;
                if (slot > 20 || L.raw_record.empty()) continue;
                std::uint32_t fm = 0;
                float fe = 0.0f, fb = 0.0f, fs = 0.0f;
                if (!formats::layer_fresnel(L, fm, fe, fb, fs)) continue;
                mesh.draw_fres[i][slot] = fe;
                mesh.draw_fres[i][slot + 1] = fb;
                mesh.draw_fres[i][slot + 2] = fs;
                mesh.draw_fres[i][slot + 3] = float(fm);
                mesh.any_fres = true;
            }
        }
    mesh.vbuf = make_buffer(pd_, dev_, sizeof(Vertex) * verts.size(),
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, verts.data());
    mesh.ibuf = make_buffer(pd_, dev_, sizeof(uint32_t) * tri.size(),
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, tri.data());
    mesh.draw_uvx.assign(mesh.draws.size(), 0);
    for (size_t i = 0; i < mesh.draws.size(); ++i) {
        const int mat = mesh.draws[i].material;
        if (mat < 0 || std::size_t(mat) >= m.materials.size()) continue;
        for (const formats::M3Layer& L : m.materials[std::size_t(mat)].layers)
            if (L.usage == "Diffuse") {
                const formats::M3Layer::AnimRefs& a = L.anim;
                const bool stat = a.uv_tiling[0] != 1.0f || a.uv_tiling[1] != 1.0f || a.uv_angle[2] != 0.0f;
                if (stat || a.uv_tiling_flags || a.uv_angle_flags) { mesh.draw_uvx[i] = 1; mesh.any_uvx = true; }
                break;
            }
    }
    mesh.draw_masklv.assign(mesh.draws.size(), {0.0f, 0.0f});
    for (size_t i = 0; i < mesh.draws.size(); ++i) {
        const int mat = mesh.draws[i].material;
        if (mat < 0 || std::size_t(mat) >= m.materials.size()) continue;
        const formats::M3Layer* L = formats::material_alpha_mask(m.materials[std::size_t(mat)]);
        if (!L) L = formats::material_const_alpha_layer(m.materials[std::size_t(mat)]);
        if (L && !L->raw_record.empty()) {
            float mul = L->anim.bright_mult;
            float add = L->anim.midtone_offset;
            static const bool kMaskInvert = std::getenv("SC2_ALPHA_MASK_NO_INVERT") == nullptr;
            if (kMaskInvert && L->usage == "AlphaMask" && (L->word36 & 0x10u) != 0) {
                add = mul + add;
                mul = -mul;
            }
            if (const char* k = std::getenv("SC2_PREPASS_MASK_ADD"); k && (m.materials[std::size_t(mat)].flags & 0x100u)) add *= float(std::atof(k));
            if (mul != 1.0f || add != 0.0f) {
                mesh.draw_masklv[i] = {mul - 1.0f, add};
                mesh.any_masklv = true;
            }
        }
    }
    mesh.draw_env.assign(mesh.draws.size(), {0.0f, 0.0f, 1.0f, 0.0f});
    for (size_t i = 0; i < mesh.draws.size(); ++i) {
        const int mat = mesh.draws[i].material;
        if (mat < 0 || std::size_t(mat) >= m.materials.size()) continue;
        if (mesh.draw_tex[i][5] == kDefaultEmissive) {
            if (std::getenv("SC2_TEX_TRACE") && !layer_of(m, mat, "EnvironmentVIO").empty())
                std::fprintf(stderr, "envnone %s material '%s' (image not bound)\n", layer_of(m, mat, "EnvironmentVIO").c_str(), m.materials[std::size_t(mat)].name.c_str());
            continue;
        }
        const formats::M3Layer* env = nullptr;
        const formats::M3Layer* mask = nullptr;
        for (const formats::M3Layer& L : m.materials[std::size_t(mat)].layers) {
            if (L.usage == "EnvironmentVIO") env = &L;
            if (L.usage == "EnvironmentVIOMask") mask = &L;
        }
        if (!env || env->anim.bright_mult <= 0.0f) continue;
        float code = 0.0f, mul = 1.0f, add = 0.0f;
        if (mask) {
            static const float kCode[6] = {5.0f, 6.0f, 1.0f, 2.0f, 3.0f, 4.0f};
            code = kCode[mask->word44 < 6 ? mask->word44 : 0];
            mul = mask->anim.bright_mult;
            add = mask->anim.midtone_offset;
        }
        const float strength = env->anim.bright_mult * env->anim.brightness;
        if (strength <= 0.0f) continue;
        const float mode = env->word40 == 3 ? 1.0f : env->word40 == 8 ? 2.0f : env->word40 == 7 ? 3.0f : 0.0f;
        const bool sphere_img = env_sphere_tex_.count(mesh.draw_tex[i][5]) != 0;
        if (sphere_img && !(mode == 1.0f || mode == 2.0f)) {
            if (std::getenv("SC2_TEX_TRACE")) std::fprintf(stderr, "envskip %s source %u image 2D\n", env->image_path.c_str(), unsigned(env->word40));
            continue;
        }
        if (sphere_img && std::getenv("SC2_TEX_TRACE"))
            std::fprintf(stderr, "[envll] sphere %s source %u strength %.3f (mult %.3f brightness %.3f add %.3f) w36 %x w44 %u mask %s code %.0f x%.2f +%.2f material '%s' blend %u flags %08x\n",
                         env->image_path.c_str(), unsigned(env->word40), double(strength), double(env->anim.bright_mult), double(env->anim.brightness), double(env->anim.midtone_offset),
                         unsigned(env->word36), unsigned(env->word44), mask ? mask->image_path.c_str() : "-", double(code), double(mul), double(add),
                         m.materials[std::size_t(mat)].name.c_str(), unsigned(m.materials[std::size_t(mat)].blend_mode), unsigned(m.materials[std::size_t(mat)].flags));
        code += 16.0f * (sphere_img ? mode : (mode == 3.0f ? 3.0f : 0.0f));
        if (mask && !std::getenv("SC2_ENV_MASK_NO_BITS")) {
            if (mask->word36 & 0x10u) code += 64.0f;
            if (mask->word36 & 0x20u) code += 128.0f;
        }
        mesh.draw_env[i] = {strength, code, mul, add};
        mesh.any_env = true;
    }
    for (const auto& dr : mesh.draws)
        if (dr.material >= 0 && std::size_t(dr.material) < m.materials.size() && m.materials[std::size_t(dr.material)].displace > 0.0f) mesh.any_displace = true;
    mesh.draw_specm.assign(mesh.draws.size(), 0.0f);
    mesh.draw_specl.assign(mesh.draws.size(), std::array<float, 3>{0.0f, 0.0f, 0.0f});
    mesh.draw_specluma.assign(mesh.draws.size(), 0);
    mesh.draw_uvsrc.assign(mesh.draws.size(), 0);
    if (!std::getenv("SC2_NO_UV_SOURCE"))
        for (size_t i = 0; i < mesh.draws.size(); ++i) {
            const int mat = mesh.draws[i].material;
            if (mat < 0 || std::size_t(mat) >= m.materials.size()) continue;
            for (const formats::M3Layer& L : m.materials[std::size_t(mat)].layers) {
                if (L.raw_record.empty()) continue;
                if (L.usage == "AlphaMask" && L.word40 == 1) mesh.draw_uvsrc[i] |= 1;
                if (L.usage == "Diffuse" && L.word40 == 5) mesh.draw_uvsrc[i] |= 2;
                if (L.usage == "Diffuse" && L.word40 >= 16 && L.word40 <= 18 && L.raw_record.size() >= 396 && !std::getenv("SC2_NO_TRIPLANAR")) {
                    float sc = 0.0f, oz = 0.0f;
                    std::memcpy(&sc, &L.raw_record[384], 4);
                    std::memcpy(&oz, &L.raw_record[356], 4);
                    if (std::isfinite(sc) && sc > 1e-5f && sc < 100.0f && std::isfinite(oz)) {
                        if (mesh.draw_tri.size() != mesh.draws.size()) mesh.draw_tri.assign(mesh.draws.size(), {0.0f, 0.0f, 0.0f});
                        bool tri_normal = false;
                        if (std::getenv("SC2_TRIPLANAR_NORMAL") && !std::getenv("SC2_TRIPLANAR_DIFFUSE_ONLY"))
                            for (const formats::M3Layer& NL : m.materials[std::size_t(mat)].layers)
                                if (NL.usage == "Normal" && !NL.raw_record.empty() && NL.word40 >= 16 && NL.word40 <= 18 && !NL.image_path.empty()) tri_normal = true;
                        if (!std::getenv("SC2_TRIPLANAR_OFFSET_ADD")) oz = -oz;
                        mesh.draw_tri[i] = {sc, oz, float(L.word40) + (tri_normal ? 0.5f : 0.0f) + (std::getenv("SC2_TRIPLANAR_VFLIP") ? 0.25f : 0.0f)};
                        mesh.draw_uvsrc[i] |= 2;
                    }
                }
                static const bool kSpecSel = std::getenv("SC2_NO_SPEC_SELECT") == nullptr;
                if (kSpecSel && L.usage == "Specular" && L.word44 == 2) mesh.draw_uvsrc[i] |= 8;
                static const bool kEmisSel = std::getenv("SC2_NO_EMIS_SELECT") == nullptr;
                if (kEmisSel && L.usage == "Emissive") {
                    if (L.word40 == 1) mesh.draw_uvsrc[i] |= 32;
                    if (L.word44 >= 2 && L.word44 <= 5) mesh.draw_uvsrc[i] |= std::uint16_t((L.word44 - 1) << 7);
                    if ((L.word36 & 0xcu) == 0) mesh.draw_uvsrc[i] |= 1024;
                }
            }
            static const bool kMatDbl = std::getenv("SC2_NO_MAT_DBL_LAMBERT") == nullptr;
            if (kMatDbl && (m.materials[std::size_t(mat)].flags & 0x80000u) != 0) mesh.draw_uvsrc[i] |= 4;
            static const bool kCliffDbl = std::getenv("SC2_NO_CLIFF_DBL_LAMBERT") == nullptr;
            if (kCliffDbl && m.vertex_alpha_coverage) mesh.draw_uvsrc[i] |= 4;
            static const bool kNoLocal = std::getenv("SC2_BLENDED_LOCAL_LIGHTS") == nullptr;
            if (kNoLocal && !m.material_refs.empty() && m.materials[std::size_t(mat)].blend_mode != formats::kBlendOpaque &&
                (m.materials[std::size_t(mat)].flags & 0x20000u) == 0)
                mesh.draw_uvsrc[i] |= 16;
            static const bool kLayerFresnel = std::getenv("SC2_NO_LAYER_FRESNEL") == nullptr;
            if (kLayerFresnel)
                for (const formats::M3Layer& L : m.materials[std::size_t(mat)].layers) {
                    std::uint32_t fm = 0;
                    float fe = 0.0f, fb = 0.0f, fs = 0.0f;
                    if (L.usage == "Emissive" && formats::layer_fresnel(L, fm, fe, fb, fs)) mesh.draw_uvsrc[i] |= 2048;
                }
            static const bool kEmisTeam = std::getenv("SC2_NO_EMIS_SELECT") == nullptr;
            if (kEmisTeam && m.materials[std::size_t(mat)].emis_op == 4) mesh.draw_uvsrc[i] |= 64;
            static const bool kMatUnfogged = std::getenv("SC2_MAT_UNFOGGED") != nullptr;
            if (kMatUnfogged && !m.material_refs.empty() && (m.materials[std::size_t(mat)].flags & 0x4u) != 0) mesh.draw_uvsrc[i] |= 4096;
            if (mesh.draw_uvsrc[i]) mesh.any_uvsrc = true;
        }
    for (size_t i = 0; i < mesh.draws.size(); ++i) {
        const int mat = mesh.draws[i].material;
        if (mat < 0 || std::size_t(mat) >= m.materials.size()) continue;
        float spec_add = 0.0f;
        float spec_layer = 1.0f;
        float spec_alpha = 1.0f;
        static const bool kSpecAlphaOne = std::getenv("SC2_SPEC_RGB_ALPHA_MULT") == nullptr;
        if (!std::getenv("SC2_NO_SPEC_LAYER_BRIGHT"))
            for (const formats::M3Layer& L : m.materials[std::size_t(mat)].layers)
                if (L.usage == "Specular" && !L.raw_record.empty()) {
                    spec_layer = L.anim.bright_mult;
                    spec_add = L.anim.midtone_offset;
                    if (!kSpecAlphaOne && L.word44 == 0 && !L.image_path.empty())
                        spec_alpha = (L.word36 & 0x20u) ? 1.0f : std::max(L.anim.bright_mult + L.anim.midtone_offset, 0.0f);
                    if (spec_alpha != 1.0f && std::getenv("SC2_SPEC_ALPHA_TRACE")) {
                        static std::set<std::string> told;
                        const formats::M3Material& tm = m.materials[std::size_t(mat)];
                        if (told.insert(tm.name).second)
                            std::fprintf(stderr, "[specalpha] '%s' flags %#x spec %.0f specMult %.2f layer x %.3f + %.3f @36 %#x alpha %.3f '%s'\n", tm.name.c_str(), unsigned(tm.flags),
                                         double(tm.specularity), double(tm.spec_mult), double(L.anim.bright_mult), double(L.anim.midtone_offset), unsigned(L.word36), double(spec_alpha), L.image_path.c_str());
                    }
                    break;
                }
        if (m.materials[std::size_t(mat)].spec_mult * spec_layer * spec_alpha != 1.0f) {
            mesh.draw_specm[i] = m.materials[std::size_t(mat)].spec_mult * spec_layer * spec_alpha - 1.0f;
            mesh.any_specm = true;
        }
        if (spec_add != 0.0f && !std::getenv("SC2_NO_SPEC_LAYER_ADD")) {
            mesh.draw_specl[i] = {spec_layer, spec_add, m.materials[std::size_t(mat)].spec_mult * spec_alpha};
            if (mesh.draw_specm[i] == 0.0f) mesh.draw_specm[i] = 1e-6f;
            mesh.any_specm = true;
        }
        if (m.materials[std::size_t(mat)].spec_luma) {
            if (mesh.draw_specl[i][1] == 0.0f) mesh.draw_specl[i] = {spec_layer, 1e-9f, m.materials[std::size_t(mat)].spec_mult};
            mesh.draw_specluma[i] = 1;
            if (mesh.draw_specm[i] == 0.0f) mesh.draw_specm[i] = 1e-6f;
            mesh.any_specm = true;
        }
        static const struct UndimRule {
            bool on = false;
            std::uint32_t mask = 0, value = 0;
            UndimRule() {
                if (const char* e = std::getenv("SC2_SPEC_UNDIM")) {
                    unsigned mk = 0, vl = 0;
                    if (std::sscanf(e, "%x:%x", &mk, &vl) == 2) { on = true; mask = mk; value = vl; }
                }
            }
        } undim;
        static const bool kDimAll = std::getenv("SC2_SPEC_DIM_ALL") != nullptr;
        static const bool kDimFoliage = std::getenv("SC2_SPEC_DIM_FOLIAGE") != nullptr;
        static const bool kUndim800000 = std::getenv("SC2_SPEC_DIM_800000") == nullptr;
        static const bool kDim800000Any = std::getenv("SC2_SPEC_DIM_800000_ANY") != nullptr;
        const std::uint32_t mflags = m.materials[std::size_t(mat)].flags;
        const bool dim800000 = (mflags & 0x800000u) != 0 && (kDim800000Any || (mflags & 0x4u) != 0);
        const bool undim_rule = undim.on ? (mflags & undim.mask) == undim.value
                                         : (!kDimAll && (!kDimFoliage || (mflags & 0x8u) == 0) && (kUndim800000 || !dim800000));
        if ((undim_rule || m.materials[std::size_t(mat)].spec_undimmed) &&
            m.materials[std::size_t(mat)].specularity > 0.0f) {
            const float pw = std::min(512.0f, std::max(1.0f, m.materials[std::size_t(mat)].specularity));
            const float dim = std::min(1.0f, std::max(0.0f, -0.000004444f * pw * pw + 0.004333f * pw + 0.0020834f));
            if (dim > 0.001f) {
                mesh.draw_specm[i] = m.materials[std::size_t(mat)].spec_mult * spec_layer * spec_alpha / dim - 1.0f;
                mesh.any_specm = true;
                static const bool kLevelledDimmed = std::getenv("SC2_SPEC_LEVELLED_DIMMED") != nullptr;
                if (!kLevelledDimmed && i < mesh.draw_specl.size() && mesh.draw_specl[i][1] != 0.0f)
                    mesh.draw_specl[i][2] = m.materials[std::size_t(mat)].spec_mult * spec_alpha / dim;
            }
        }
        if (const char* k = std::getenv("SC2_PREPASS_SPEC_SCALE"); k && (m.materials[std::size_t(mat)].flags & 0x100u)) {
            mesh.draw_specm[i] = m.materials[std::size_t(mat)].spec_mult * spec_layer * float(std::atof(k)) - 1.0f;
            mesh.any_specm = true;
        }
        if (const char* e = std::getenv("SC2_SPEC_SCALE_MATERIAL")) {
            const std::string spec(e);
            const std::size_t colon = spec.rfind(':');
            const formats::M3Material& mm = m.materials[std::size_t(mat)];
            if (colon != std::string::npos && mm.name.find(spec.substr(0, colon)) != std::string::npos) {
                const float k = float(std::atof(spec.c_str() + colon + 1));
                std::printf("[specscale] '%s' flags %#x specularity %.1f specMult %.2f layer x %.2f + %.3f, carried %.3f -> x %.3f\n", mm.name.c_str(), unsigned(mm.flags),
                            double(mm.specularity), double(mm.spec_mult), double(spec_layer), double(spec_add), double(mesh.draw_specm[i] + 1.0f), double(k));
                mesh.draw_specm[i] = (mesh.draw_specm[i] + 1.0f) * k - 1.0f;
                if (i < mesh.draw_specl.size() && mesh.draw_specl[i][1] != 0.0f) mesh.draw_specl[i][2] *= k;
                mesh.any_specm = true;
            }
        }
    }
    mesh.draw_set.assign(mesh.draws.size(), VK_NULL_HANDLE);
    for (size_t d = 0; d < mesh.draws.size(); ++d) {
        if (!free_mat_sets_.empty()) {
            mesh.draw_set[d] = free_mat_sets_.back();
            free_mat_sets_.pop_back();
        } else {
            mesh.draw_set[d] = alloc_set(matPool_, matPoolsExtra_, texLayout_.get(),
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 11, 512, "material");
        }
        VkDescriptorImageInfo dii[11];
        VkWriteDescriptorSet w[11];
        for (int s = 0; s < 11; ++s) {
            dii[s] = {sampler_.get(), textures_[mesh.draw_tex[d][s]].view.get(),
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w[s] = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            w[s].dstSet = mesh.draw_set[d];
            w[s].dstBinding = uint32_t(s);
            w[s].descriptorCount = 1;
            w[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[s].pImageInfo = &dii[s];
        }
        vkUpdateDescriptorSets(dev_, 11, w, 0, nullptr);
    }
    return mesh_cache_.emplace(&m, std::move(mesh)).first->second;
}

static constexpr float kTerrainBumpGain = 1.0f;

SceneRenderer::TerrainSplat* SceneRenderer::ensure_splat(const formats::Model& m) {
    if (!m.has_splat_payload() || !splatPipeline_.get()) return nullptr;
    auto it = splat_cache_.find(&m);
    if (it != splat_cache_.end()) return &it->second;

    constexpr uint32_t kLayers = 8;
    uint32_t tw = 0, th = 0;
    int first_ok = -1;
    for (size_t i = 0; i < m.splat_layers.size(); ++i) {
        const auto& L = m.splat_layers[i];
        if (L.w && L.h && L.rgba.size() >= size_t(L.w) * L.h * 4) {
            if (first_ok < 0) first_ok = int(i);
            tw = std::max(tw, L.w);
            th = std::max(th, L.h);
        }
    }
    if (first_ok < 0) return nullptr;
    if (tw > 1024) tw = 1024;
    if (th > 1024) th = 1024;

    std::vector<uint8_t> diff(size_t(tw) * th * 4 * kLayers);
    for (uint32_t L = 0; L < kLayers; ++L) {
        const auto* src = &m.splat_layers[size_t(first_ok)];
        if (L < m.splat_layers.size()) {
            const auto& cand = m.splat_layers[L];
            if (cand.w && cand.h && cand.rgba.size() >= size_t(cand.w) * cand.h * 4) src = &cand;
        }
        uint8_t* dst = diff.data() + size_t(L) * tw * th * 4;
        for (uint32_t y = 0; y < th; ++y) {
            const uint32_t sy = uint32_t(uint64_t(y) * src->h / th);
            for (uint32_t x = 0; x < tw; ++x) {
                const uint32_t sx = uint32_t(uint64_t(x) * src->w / tw);
                const uint8_t* sp = &src->rgba[(size_t(sy) * src->w + sx) * 4];
                std::memcpy(dst + (size_t(y) * tw + x) * 4, sp, 4);
            }
        }
    }

    TerrainSplat ts;
    ts.extent[0] = m.splat_extent_x;
    ts.extent[1] = m.splat_extent_y;
    auto upload_array = [&](VkFormat fmt, uint32_t w, uint32_t h, uint32_t bpp,
                            const uint8_t* bytes, size_t len, uint32_t layers = 0, bool mipped = false) -> Image {
        if (layers == 0) layers = kLayers;
        uint32_t mips = 1;
        if (mipped)
            for (uint32_t d = std::max(w, h); d > 1; d >>= 1) ++mips;
        Image img = make_image_array(pd_, dev_, fmt,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         (mips > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
                                     w, h, layers, mips);
        Buffer stg = make_buffer(pd_, dev_, len, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 const_cast<uint8_t*>(bytes));
        one_time([&](VkCommandBuffer cb) {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.image = img.img.get();
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy rgn{};
            rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
            rgn.imageExtent = {w, h, 1};
            vkCmdCopyBufferToImage(cb, stg.buf.get(), img.img.get(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
            int32_t mw = int32_t(w), mh = int32_t(h);
            for (uint32_t lv = 1; lv < mips; ++lv) {
                VkImageMemoryBarrier pb{};
                pb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                pb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                pb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                pb.image = img.img.get();
                pb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, lv - 1, 1, 0, layers};
                pb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                pb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &pb);
                const int32_t nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
                VkImageBlit bl{};
                bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv - 1, 0, layers};
                bl.srcOffsets[1] = {mw, mh, 1};
                bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, layers};
                bl.dstOffsets[1] = {nw, nh, 1};
                vkCmdBlitImage(cb, img.img.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.img.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
                pb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                pb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                pb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                pb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &pb);
                mw = nw;
                mh = nh;
            }
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mips - 1, 1, 0, layers};
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
        });
        (void)bpp;
        return img;
    };
    ts.diffuse_arr = upload_array(kTexColorSrgb, tw, th, 4, diff.data(), diff.size(), 0, true);
    ts.weight_arr = upload_array(VK_FORMAT_R8_UNORM, m.splat_w, m.splat_h, 1,
                                 m.splat_weights.data(),
                                 m.splat_weights.size(),
                                 uint32_t(m.splat_weights.size() / (size_t(m.splat_w) * m.splat_h)));
    {
        std::vector<uint8_t> nrm(size_t(tw) * th * 4 * kLayers);
        for (uint32_t L = 0; L < kLayers; ++L) {
            uint8_t* dst = nrm.data() + size_t(L) * tw * th * 4;
            const formats::Model::SplatLayerImage* src = L < m.splat_normals.size() ? &m.splat_normals[L] : nullptr;
            const bool ok = src && src->w && src->h && src->rgba.size() >= size_t(src->w) * src->h * 4;
            for (uint32_t y = 0; y < th; ++y)
                for (uint32_t x = 0; x < tw; ++x) {
                    uint8_t* d = dst + (size_t(y) * tw + x) * 4;
                    if (ok) {
                        const uint32_t sx = uint32_t(uint64_t(x) * src->w / tw), sy = uint32_t(uint64_t(y) * src->h / th);
                        std::memcpy(d, &src->rgba[(size_t(sy) * src->w + sx) * 4], 4);
                    } else {
                        d[0] = 128; d[1] = 128; d[2] = 255; d[3] = 128;
                    }
                }
        }
        ts.normal_arr = upload_array(kTexColor, tw, th, 4, nrm.data(), nrm.size(), 0, true);
    }

    VkDescriptorSetAllocateInfo dsa{};

    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool = splatPool_.get();
    dsa.descriptorSetCount = 1;
    VkDescriptorSetLayout sl = splatLayout_.get();
    dsa.pSetLayouts = &sl;
    check(vkAllocateDescriptorSets(dev_, &dsa, &ts.set), "vkAllocateDescriptorSets(splat)");
    VkDescriptorImageInfo dii[3] = {
        {sampler_.get(), ts.diffuse_arr.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler_.get(), ts.weight_arr.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler_.get(), ts.normal_arr.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[3];
    for (int s = 0; s < 3; ++s) {
        w[s] = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        w[s].dstSet = ts.set;
        w[s].dstBinding = uint32_t(s);
        w[s].descriptorCount = 1;
        w[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[s].pImageInfo = &dii[s];
    }
    vkUpdateDescriptorSets(dev_, 3, w, 0, nullptr);
    return &splat_cache_.emplace(&m, std::move(ts)).first->second;
}

SceneRenderer::SceneRenderer(VkPhysicalDevice pd, VkDevice dev, VkQueue queue,
                             uint32_t gfx_family, uint32_t width, uint32_t height,
                             VkFormat ldr_format, const casc::Storage* storage,
                             std::string texture_root, const Tunables& tun)
    : pd_(pd), dev_(dev), queue_(queue), gfx_family_(gfx_family), width_(width),
      height_(height), ldr_format_(ldr_format), storage_(storage),
      texture_root_(std::move(texture_root)), tun_(tun) {
    tun_.light_dir = normalize(tun_.light_dir);
    if (const char* sd = std::getenv("SC2_SHADOW_DIM")) {
        const int v = std::atoi(sd);
        if (v >= 1024 && v <= 8192) tun_.shadow_dim = uint32_t(v);
    }

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = gfx_family_;
    VkCommandPool rawCmdPool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(dev_, &cpi, nullptr, &rawCmdPool), "vkCreateCommandPool(scene)");
    cmdPool_ = Unique<VkCommandPool>(rawCmdPool, [dev](VkCommandPool p) { vkDestroyCommandPool(dev, p, nullptr); });

    VkPhysicalDeviceFeatures feat{};
    vkGetPhysicalDeviceFeatures(pd_, &feat);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd_, &props);
    VkSamplerCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    smi.magFilter = smi.minFilter = VK_FILTER_LINEAR;
    smi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    smi.addressModeU = smi.addressModeV = smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    smi.maxLod = VK_LOD_CLAMP_NONE;
    smi.anisotropyEnable = feat.samplerAnisotropy ? VK_TRUE : VK_FALSE;
    smi.maxAnisotropy = feat.samplerAnisotropy ? (props.limits.maxSamplerAnisotropy < 16.0f ? props.limits.maxSamplerAnisotropy : 16.0f) : 1.0f;
    VkSampler rawSampler = VK_NULL_HANDLE;
    check(vkCreateSampler(dev_, &smi, nullptr, &rawSampler), "vkCreateSampler");
    sampler_ = Unique<VkSampler>(rawSampler, [dev](VkSampler s) { vkDestroySampler(dev, s, nullptr); });

    VkSamplerCreateInfo ssmi{};

    ssmi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    {
        VkFormatProperties sfp{};
        vkGetPhysicalDeviceFormatProperties(pd_, kDepthF, &sfp);
        const bool linear_ok = (sfp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        ssmi.magFilter = ssmi.minFilter = (linear_ok && !std::getenv("SC2_SHADOW_NEAREST")) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    }
    ssmi.compareEnable = VK_TRUE;
    ssmi.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ssmi.addressModeU = ssmi.addressModeV = ssmi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ssmi.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    ssmi.maxLod = 1.0f;
    VkSampler rawShadowSampler = VK_NULL_HANDLE;
    check(vkCreateSampler(dev_, &ssmi, nullptr, &rawShadowSampler), "vkCreateSampler(shadow)");
    shadowSampler_ = Unique<VkSampler>(rawShadowSampler, [dev](VkSampler s) { vkDestroySampler(dev, s, nullptr); });
    VkSamplerCreateInfo fsmi{};
    fsmi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    fsmi.magFilter = fsmi.minFilter = VK_FILTER_LINEAR;
    fsmi.addressModeU = fsmi.addressModeV = fsmi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    fsmi.maxLod = 1.0f;
    VkSampler rawFogSampler = VK_NULL_HANDLE;
    check(vkCreateSampler(dev_, &fsmi, nullptr, &rawFogSampler), "vkCreateSampler(fog)");
    fogSampler_ = Unique<VkSampler>(rawFogSampler, [dev](VkSampler s) { vkDestroySampler(dev, s, nullptr); });
    upload_fog_image(1, 1, {0, 0, 0, 0});

    VkDescriptorSetLayoutBinding texb[11]{};
    for (int s = 0; s < 11; ++s) {
        texb[s].binding = uint32_t(s);
        texb[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texb[s].descriptorCount = 1;
        texb[s].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo texli{};
    texli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    texli.bindingCount = 11;
    texli.pBindings = texb;
    VkDescriptorSetLayout rawTexLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev_, &texli, nullptr, &rawTexLayout), "vkCreateDescriptorSetLayout(tex)");
    texLayout_ = Unique<VkDescriptorSetLayout>(rawTexLayout, [dev](VkDescriptorSetLayout l) { vkDestroyDescriptorSetLayout(dev, l, nullptr); });

    VkDescriptorSetLayoutBinding boneb{};
    boneb.binding = 0;
    boneb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    boneb.descriptorCount = 1;
    boneb.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo boneli{};
    boneli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    boneli.bindingCount = 1;
    boneli.pBindings = &boneb;
    VkDescriptorSetLayout rawBoneLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev_, &boneli, nullptr, &rawBoneLayout), "vkCreateDescriptorSetLayout(bone)");
    boneLayout_ = Unique<VkDescriptorSetLayout>(rawBoneLayout, [dev](VkDescriptorSetLayout l) { vkDestroyDescriptorSetLayout(dev, l, nullptr); });

    VkDescriptorSetLayoutBinding gb[7]{};
    gb[6].binding = 6;
    gb[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    gb[6].descriptorCount = 1;
    gb[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    gb[0].binding = 0;
    gb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    gb[0].descriptorCount = 1;
    gb[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    gb[1].binding = 1;
    gb[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    gb[1].descriptorCount = 1;
    gb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    gb[2].binding = 2;
    gb[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    gb[2].descriptorCount = 1;
    gb[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    gb[3].binding = 3;
    gb[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    gb[3].descriptorCount = 1;
    gb[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    gb[4].binding = 4;
    gb[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    gb[4].descriptorCount = 1;
    gb[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    gb[5].binding = 5;
    gb[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    gb[5].descriptorCount = 1;
    gb[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo gli{};
    gli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    gli.bindingCount = 7;
    gli.pBindings = gb;
    VkDescriptorSetLayout rawGlobalLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev_, &gli, nullptr, &rawGlobalLayout), "vkCreateDescriptorSetLayout(global)");
    globalLayout_ = Unique<VkDescriptorSetLayout>(rawGlobalLayout, [dev](VkDescriptorSetLayout l) { vkDestroyDescriptorSetLayout(dev, l, nullptr); });

    VkDescriptorSetLayoutBinding pb[5]{};
    for (uint32_t b = 0; b < 5; ++b) {
        pb[b].binding = b;
        pb[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pb[b].descriptorCount = 1;
        pb[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo pli2{};
    pli2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    pli2.bindingCount = 5;
    pli2.pBindings = pb;
    VkDescriptorSetLayout rawPartLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev_, &pli2, nullptr, &rawPartLayout), "vkCreateDescriptorSetLayout(particle)");
    partLayout_ = Unique<VkDescriptorSetLayout>(rawPartLayout, [dev](VkDescriptorSetLayout l) { vkDestroyDescriptorSetLayout(dev, l, nullptr); });

    VkDescriptorSetLayoutBinding tmb[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        tmb[i].binding = i;
        tmb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tmb[i].descriptorCount = 1;
        tmb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo tmli{};
    tmli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    tmli.bindingCount = 4;
    tmli.pBindings = tmb;
    VkDescriptorSetLayout rawTmLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev_, &tmli, nullptr, &rawTmLayout), "vkCreateDescriptorSetLayout(tonemap)");
    tmLayout_ = Unique<VkDescriptorSetLayout>(rawTmLayout, [dev](VkDescriptorSetLayout l) { vkDestroyDescriptorSetLayout(dev, l, nullptr); });

    shadowMap_ = make_image(pd_, dev_, kDepthF,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT, tun_.shadow_dim, tun_.shadow_dim);

    GlobalsUBO zero{};
    globalsBuf_ = make_buffer(pd_, dev_, sizeof(GlobalsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &zero);

    VkDescriptorPoolSize gpsizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5},
                                       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo gdpi{};
    gdpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    gdpi.maxSets = 1;
    gdpi.poolSizeCount = 3;
    gdpi.pPoolSizes = gpsizes;
    VkDescriptorPool rawGlobalPool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(dev_, &gdpi, nullptr, &rawGlobalPool), "vkCreateDescriptorPool(global)");
    globalPool_ = Unique<VkDescriptorPool>(rawGlobalPool, [dev](VkDescriptorPool p) { vkDestroyDescriptorPool(dev, p, nullptr); });
    {
        VkDescriptorSetAllocateInfo gdsa{};
        gdsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        gdsa.descriptorPool = rawGlobalPool;
        gdsa.descriptorSetCount = 1;
        VkDescriptorSetLayout gl = globalLayout_.get();
        gdsa.pSetLayouts = &gl;
        check(vkAllocateDescriptorSets(dev_, &gdsa, &global_set_), "vkAllocateDescriptorSets(global)");
        VkDescriptorBufferInfo ubi{globalsBuf_.buf.get(), 0, sizeof(GlobalsUBO)};
        VkDescriptorImageInfo smi2{shadowSampler_.get(), shadowMap_.view.get(),
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo fgi{fogSampler_.get(), fogImg_.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet gw[3]{};
        gw[0] = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        gw[0].dstSet = global_set_;
        gw[0].dstBinding = 0;
        gw[0].descriptorCount = 1;
        gw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        gw[0].pBufferInfo = &ubi;
        gw[1] = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        gw[1].dstSet = global_set_;
        gw[1].dstBinding = 1;
        gw[1].descriptorCount = 1;
        gw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        gw[1].pImageInfo = &smi2;
        gw[2] = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        gw[2].dstSet = global_set_;
        gw[2].dstBinding = 2;
        gw[2].descriptorCount = 1;
        gw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        gw[2].pImageInfo = &fgi;
        vkUpdateDescriptorSets(dev_, 3, gw, 0, nullptr);
        ensure_fx_capacity(64);
        upload_creep_image(1, 1, {0});
    }

    const uint32_t kMaxMeshSets = 512, kMaxBoneSets = 4096, kMaxPartSets = 256;
    VkDescriptorPoolSize matSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 * kMaxMeshSets};
    VkDescriptorPoolCreateInfo matdpi{};
    matdpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    matdpi.maxSets = kMaxMeshSets;
    matdpi.poolSizeCount = 1;
    matdpi.pPoolSizes = &matSize;
    VkDescriptorPool rawMatPool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(dev_, &matdpi, nullptr, &rawMatPool), "vkCreateDescriptorPool(mat)");
    matPool_ = Unique<VkDescriptorPool>(rawMatPool, [dev](VkDescriptorPool p) { vkDestroyDescriptorPool(dev, p, nullptr); });

    VkDescriptorPoolSize boneSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxBoneSets};
    VkDescriptorPoolCreateInfo bonedpi{};
    bonedpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    bonedpi.maxSets = kMaxBoneSets;
    bonedpi.poolSizeCount = 1;
    bonedpi.pPoolSizes = &boneSize;
    VkDescriptorPool rawBonePool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(dev_, &bonedpi, nullptr, &rawBonePool), "vkCreateDescriptorPool(bone)");
    bonePool_ = Unique<VkDescriptorPool>(rawBonePool, [dev](VkDescriptorPool p) { vkDestroyDescriptorPool(dev, p, nullptr); });

    VkDescriptorPoolSize partSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxPartSets * 5};
    VkDescriptorPoolCreateInfo partdpi{};
    partdpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    partdpi.maxSets = kMaxPartSets;
    partdpi.poolSizeCount = 1;
    partdpi.pPoolSizes = &partSize;
    VkDescriptorPool rawPartPool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(dev_, &partdpi, nullptr, &rawPartPool), "vkCreateDescriptorPool(particle)");
    partPool_ = Unique<VkDescriptorPool>(rawPartPool, [dev](VkDescriptorPool p) { vkDestroyDescriptorPool(dev, p, nullptr); });

    kDefaultWhite = upload_texture(1, 1, {255, 255, 255, 255});
    kDefaultNormal = upload_texture(1, 1, {128, 128, 255, 255});
    kDefaultEmissive = upload_texture(1, 1, {0, 0, 0, 255});
    kDefaultSpecular = upload_texture(1, 1, {0, 0, 0, 255});
    kDefaultClear = upload_texture(1, 1, {0, 0, 0, 0});

    VkAttachmentDescription att[2]{};
    att[0].format = kHdr;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    att[1].format = kDepthF;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subp{};
    subp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subp.colorAttachmentCount = 1;
    subp.pColorAttachments = &colorRef;
    subp.pDepthStencilAttachment = &depthRef;
    VkSubpassDependency sdep{};
    sdep.srcSubpass = 0;
    sdep.dstSubpass = VK_SUBPASS_EXTERNAL;
    sdep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    sdep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    sdep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    sdep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rpi{};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 2;
    rpi.pAttachments = att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &subp;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &sdep;
    VkRenderPass rawPass = VK_NULL_HANDLE;
    check(vkCreateRenderPass(dev_, &rpi, nullptr, &rawPass), "vkCreateRenderPass(lit)");
    litPass_ = Unique<VkRenderPass>(rawPass, [dev](VkRenderPass p) { vkDestroyRenderPass(dev, p, nullptr); });
    {
        VkAttachmentDescription a2[2] = {att[0], att[1]};
        a2[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        a2[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderPassCreateInfo r2 = rpi;
        r2.pAttachments = a2;
        VkRenderPass rawA = VK_NULL_HANDLE;
        check(vkCreateRenderPass(dev_, &r2, nullptr, &rawA), "vkCreateRenderPass(lit A)");
        litPassA_ = Unique<VkRenderPass>(rawA, [dev](VkRenderPass p) { vkDestroyRenderPass(dev, p, nullptr); });
        VkAttachmentDescription b2[2] = {att[0], att[1]};
        b2[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        b2[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b2[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        b2[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        r2.pAttachments = b2;
        VkRenderPass rawB = VK_NULL_HANDLE;
        check(vkCreateRenderPass(dev_, &r2, nullptr, &rawB), "vkCreateRenderPass(lit B)");
        litPassB_ = Unique<VkRenderPass>(rawB, [dev](VkRenderPass p) { vkDestroyRenderPass(dev, p, nullptr); });
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
    VkDescriptorSetLayout set_layouts[3] = {texLayout_.get(), boneLayout_.get(), globalLayout_.get()};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 3;
    pli.pSetLayouts = set_layouts;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VkPipelineLayout rawLayout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(dev_, &pli, nullptr, &rawLayout), "vkCreatePipelineLayout(scene)");
    layout_ = Unique<VkPipelineLayout>(rawLayout, [dev](VkPipelineLayout l) { vkDestroyPipelineLayout(dev, l, nullptr); });

    VkVertexInputBindingDescription vib{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription via[7] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
                                                {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
                                                {4, 0, VK_FORMAT_R32G32B32A32_UINT, 48},
                                                {5, 0, VK_FORMAT_R32G32_SFLOAT, 64},
                                                {6, 0, VK_FORMAT_R8G8B8A8_UNORM, 72}};
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &vib;
    vin.vertexAttributeDescriptionCount = 7;
    vin.pVertexAttributeDescriptions = via;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, float(width_), float(height_), 0, 1};
    VkRect2D scis{{0, 0}, {width_, height_}};
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &scis;
    const VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    vs_ = make_shader(dev_, kSceneVertSpv, sizeof(kSceneVertSpv));
    fs_ = make_shader(dev_, kSceneFragSpv, sizeof(kSceneFragSpv));
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs_.get();
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs_.get();
        stages[1].pName = "main";
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gpi{};
        gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpi.stageCount = 2;
        gpi.pStages = stages;
        gpi.pVertexInputState = &vin;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vps;
        gpi.pDynamicState = &dyn;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;
        gpi.pDepthStencilState = &ds;
        gpi.pColorBlendState = &cb;
        gpi.layout = rawLayout;
        gpi.renderPass = litPass_.get();
        VkPipeline rawPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gpi, nullptr, &rawPipeline), "vkCreateGraphicsPipelines(lit)");
        litPipeline_ = Unique<VkPipeline>(rawPipeline, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });

        ds.depthWriteEnable = VK_FALSE;
        cba.blendEnable = VK_TRUE;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        auto make_blended = [&](VkBlendFactor src, VkBlendFactor dst, const char* what) {
            cba.srcColorBlendFactor = src;
            cba.dstColorBlendFactor = dst;
            VkPipeline p = VK_NULL_HANDLE;
            check(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gpi, nullptr, &p), what);
            return Unique<VkPipeline>(p, [dev](VkPipeline q) { vkDestroyPipeline(dev, q, nullptr); });
        };
        addPipeline_ = make_blended(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, "vkCreateGraphicsPipelines(lit add)");
        alphaAddPipeline_ = make_blended(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE,
                                         "vkCreateGraphicsPipelines(lit alpha-add)");
        alphaBlendPipeline_ = make_blended(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                           "vkCreateGraphicsPipelines(lit alpha-blend)");
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        addGlowPipeline_ = make_blended(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, "vkCreateGraphicsPipelines(lit add glow)");
        alphaAddGlowPipeline_ = make_blended(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE,
                                             "vkCreateGraphicsPipelines(lit alpha-add glow)");
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        alphaBlendEqualPipeline_ = make_blended(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                "vkCreateGraphicsPipelines(lit alpha-blend at prepass depth)");
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        ds.depthWriteEnable = VK_TRUE;
        cba.blendEnable = VK_FALSE;
        cba.colorWriteMask = 0;
        depthPrepassPipeline_ = make_blended(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, "vkCreateGraphicsPipelines(lit depth prepass)");
    }

    {
        VkDescriptorSetLayoutBinding sb[3]{};
        for (int s = 0; s < 3; ++s) {
            sb[s].binding = uint32_t(s);
            sb[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            sb[s].descriptorCount = 1;
            sb[s].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo sli{};
        sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sli.bindingCount = 3;
        sli.pBindings = sb;
        VkDescriptorSetLayout rawSplat = VK_NULL_HANDLE;
        check(vkCreateDescriptorSetLayout(dev_, &sli, nullptr, &rawSplat), "vkCreateDescriptorSetLayout(splat)");
        splatLayout_ = Unique<VkDescriptorSetLayout>(rawSplat, [dev](VkDescriptorSetLayout l) { vkDestroyDescriptorSetLayout(dev, l, nullptr); });

        VkDescriptorSetLayout splat_sets[3] = {splatLayout_.get(), boneLayout_.get(), globalLayout_.get()};
        VkPipelineLayoutCreateInfo spli{};
        spli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        spli.setLayoutCount = 3;
        spli.pSetLayouts = splat_sets;
        spli.pushConstantRangeCount = 1;
        spli.pPushConstantRanges = &pcr;
        VkPipelineLayout rawSpl = VK_NULL_HANDLE;
        check(vkCreatePipelineLayout(dev_, &spli, nullptr, &rawSpl), "vkCreatePipelineLayout(splat)");
        splatPipeLayout_ = Unique<VkPipelineLayout>(rawSpl, [dev](VkPipelineLayout l) { vkDestroyPipelineLayout(dev, l, nullptr); });

        splatfs_ = make_shader(dev_, kTerrainSplatFragSpv, sizeof(kTerrainSplatFragSpv));
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs_.get();
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = splatfs_.get();
        stages[1].pName = "main";
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gpi{};
        gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpi.stageCount = 2;
        gpi.pStages = stages;
        gpi.pVertexInputState = &vin;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vps;
        gpi.pDynamicState = &dyn;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;
        gpi.pDepthStencilState = &ds;
        gpi.pColorBlendState = &cb;
        gpi.layout = rawSpl;
        gpi.renderPass = litPass_.get();
        VkPipeline rawPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gpi, nullptr, &rawPipeline), "vkCreateGraphicsPipelines(splat)");
        splatPipeline_ = Unique<VkPipeline>(rawPipeline, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });

        VkDescriptorPoolSize sp{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12};
        VkDescriptorPoolCreateInfo spci{};
        spci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        spci.maxSets = 4;
        spci.poolSizeCount = 1;
        spci.pPoolSizes = &sp;
        VkDescriptorPool rawPool = VK_NULL_HANDLE;
        check(vkCreateDescriptorPool(dev_, &spci, nullptr, &rawPool), "vkCreateDescriptorPool(splat)");
        splatPool_ = Unique<VkDescriptorPool>(rawPool, [dev](VkDescriptorPool p) { vkDestroyDescriptorPool(dev, p, nullptr); });
    }

    VkAttachmentDescription satt{};
    satt.format = kDepthF;
    satt.samples = VK_SAMPLE_COUNT_1_BIT;
    satt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    satt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    satt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    satt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    satt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    satt.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference sdepthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription ssub{};
    ssub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    ssub.colorAttachmentCount = 0;
    ssub.pDepthStencilAttachment = &sdepthRef;
    VkSubpassDependency ssdep{};
    ssdep.srcSubpass = 0;
    ssdep.dstSubpass = VK_SUBPASS_EXTERNAL;
    ssdep.srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    ssdep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    ssdep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    ssdep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo srpi{};
    srpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    srpi.attachmentCount = 1;
    srpi.pAttachments = &satt;
    srpi.subpassCount = 1;
    srpi.pSubpasses = &ssub;
    srpi.dependencyCount = 1;
    srpi.pDependencies = &ssdep;
    VkRenderPass rawShadowPass = VK_NULL_HANDLE;
    check(vkCreateRenderPass(dev_, &srpi, nullptr, &rawShadowPass), "vkCreateRenderPass(shadow)");
    shadowPass_ = Unique<VkRenderPass>(rawShadowPass, [dev](VkRenderPass p) { vkDestroyRenderPass(dev, p, nullptr); });
    {
        VkImageView sfbView = shadowMap_.view.get();
        VkFramebufferCreateInfo sfbi{};
        sfbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        sfbi.renderPass = rawShadowPass;
        sfbi.attachmentCount = 1;
        sfbi.pAttachments = &sfbView;
        sfbi.width = tun_.shadow_dim;
        sfbi.height = tun_.shadow_dim;
        sfbi.layers = 1;
        VkFramebuffer rawShadowFb = VK_NULL_HANDLE;
        check(vkCreateFramebuffer(dev_, &sfbi, nullptr, &rawShadowFb), "vkCreateFramebuffer(shadow)");
        shadowFb_ = Unique<VkFramebuffer>(rawShadowFb, [dev](VkFramebuffer f) { vkDestroyFramebuffer(dev, f, nullptr); });
    }
    svs_ = make_shader(dev_, kShadowVertSpv, sizeof(kShadowVertSpv));
    {
        VkPipelineShaderStageCreateInfo sstage{};
        sstage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        sstage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        sstage.module = svs_.get();
        sstage.pName = "main";
        VkViewport svp{0, 0, float(tun_.shadow_dim), float(tun_.shadow_dim), 0, 1};
        VkRect2D sscis{{0, 0}, {tun_.shadow_dim, tun_.shadow_dim}};
        VkPipelineViewportStateCreateInfo svps{};
        svps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        svps.viewportCount = 1;
        svps.pViewports = &svp;
        svps.scissorCount = 1;
        svps.pScissors = &sscis;
        VkPipelineRasterizationStateCreateInfo srs{};
        srs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        srs.polygonMode = VK_POLYGON_MODE_FILL;
        srs.cullMode = VK_CULL_MODE_NONE;
        srs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        srs.lineWidth = 1.0f;
        srs.depthBiasEnable = VK_TRUE;
        srs.depthBiasConstantFactor = tun_.raster_const;
        srs.depthBiasSlopeFactor = tun_.raster_slope;
        if (const char* sr = std::getenv("SC2_SHADOW_RASTER")) {
            float rc = tun_.raster_const, rs = tun_.raster_slope;
            if (std::sscanf(sr, "%f:%f", &rc, &rs) >= 1) {
                srs.depthBiasConstantFactor = rc;
                srs.depthBiasSlopeFactor = rs;
            }
        }
        VkPipelineDepthStencilStateCreateInfo sds{};
        sds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        sds.depthTestEnable = VK_TRUE;
        sds.depthWriteEnable = VK_TRUE;
        sds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendStateCreateInfo scb{};
        scb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        scb.attachmentCount = 0;
        VkGraphicsPipelineCreateInfo sgpi{};
        sgpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        sgpi.stageCount = 1;
        sgpi.pStages = &sstage;
        sgpi.pVertexInputState = &vin;
        sgpi.pInputAssemblyState = &ia;
        sgpi.pViewportState = &svps;
        sgpi.pRasterizationState = &srs;
        sgpi.pMultisampleState = &ms;
        sgpi.pDepthStencilState = &sds;
        sgpi.pColorBlendState = &scb;
        sgpi.layout = rawLayout;
        sgpi.renderPass = rawShadowPass;
        VkPipeline rawShadowPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &sgpi, nullptr, &rawShadowPipeline), "vkCreateGraphicsPipelines(shadow)");
        shadowPipeline_ = Unique<VkPipeline>(rawShadowPipeline, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });
        sfs_ = make_shader(dev_, kShadowFragSpv, sizeof(kShadowFragSpv));
        VkPipelineShaderStageCreateInfo cstages[2]{};
        cstages[0] = sstage;
        cstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        cstages[1].module = sfs_.get();
        cstages[1].pName = "main";
        sgpi.stageCount = 2;
        sgpi.pStages = cstages;
        VkPipeline rawCut = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &sgpi, nullptr, &rawCut), "vkCreateGraphicsPipelines(shadow cutout)");
        shadowCutoutPipeline_ = Unique<VkPipeline>(rawCut, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });
    }

    {
        VkPushConstantRange ppcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
        VkPipelineLayoutCreateInfo ppli{};
        ppli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ppli.setLayoutCount = 1;
        VkDescriptorSetLayout ppl = partLayout_.get();
        ppli.pSetLayouts = &ppl;
        ppli.pushConstantRangeCount = 1;
        ppli.pPushConstantRanges = &ppcr;
        VkPipelineLayout rawPartPipeLayout = VK_NULL_HANDLE;
        check(vkCreatePipelineLayout(dev_, &ppli, nullptr, &rawPartPipeLayout), "vkCreatePipelineLayout(particle)");
        partPipeLayout_ = Unique<VkPipelineLayout>(rawPartPipeLayout, [dev](VkPipelineLayout l) { vkDestroyPipelineLayout(dev, l, nullptr); });
        pvs_ = make_shader(dev_, kParticleVertSpv, sizeof(kParticleVertSpv));
        pfs_ = make_shader(dev_, kParticleFragSpv, sizeof(kParticleFragSpv));
        VkPipelineShaderStageCreateInfo pstages[2]{};
        pstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        pstages[0].module = pvs_.get();
        pstages[0].pName = "main";
        pstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        pstages[1].module = pfs_.get();
        pstages[1].pName = "main";
        VkVertexInputBindingDescription pvib{0, sizeof(ParticleVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription pvia[4] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                     {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
                                                     {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20},
                                                     {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 36}};
        VkPipelineVertexInputStateCreateInfo pvin{};
        pvin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        pvin.vertexBindingDescriptionCount = 1;
        pvin.pVertexBindingDescriptions = &pvib;
        pvin.vertexAttributeDescriptionCount = 4;
        pvin.pVertexAttributeDescriptions = pvia;
        VkPipelineColorBlendAttachmentState pcba{};
        pcba.blendEnable = VK_TRUE;
        pcba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        pcba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        pcba.colorBlendOp = VK_BLEND_OP_ADD;
        pcba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        pcba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        pcba.alphaBlendOp = VK_BLEND_OP_ADD;
        pcba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo pcb{};
        pcb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        pcb.attachmentCount = 1;
        pcb.pAttachments = &pcba;
        VkPipelineDepthStencilStateCreateInfo pds{};
        pds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        pds.depthTestEnable = VK_TRUE;
        pds.depthWriteEnable = VK_FALSE;
        pds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkGraphicsPipelineCreateInfo pgpi{};
        pgpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pgpi.stageCount = 2;
        pgpi.pStages = pstages;
        pgpi.pVertexInputState = &pvin;
        pgpi.pInputAssemblyState = &ia;
        pgpi.pViewportState = &vps;
        pgpi.pDynamicState = &dyn;
        pgpi.pRasterizationState = &rs;
        pgpi.pMultisampleState = &ms;
        pgpi.pDepthStencilState = &pds;
        pgpi.pColorBlendState = &pcb;
        pgpi.layout = rawPartPipeLayout;
        pgpi.renderPass = litPass_.get();
        auto make_part = [&](VkBlendFactor src, VkBlendFactor dst, const char* what) {
            pcba.srcColorBlendFactor = src;
            pcba.dstColorBlendFactor = dst;
            VkPipeline raw = VK_NULL_HANDLE;
            check(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &pgpi, nullptr, &raw), what);
            return Unique<VkPipeline>(raw, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });
        };
        partPipeline_ = make_part(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE, "vkCreateGraphicsPipelines(particle)");
        partAddPipeline_ = make_part(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, "vkCreateGraphicsPipelines(particle add)");
        partAlphaBlendPipeline_ = make_part(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                            "vkCreateGraphicsPipelines(particle alpha)");
        pcba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        partGlowPipeline_ = make_part(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE, "vkCreateGraphicsPipelines(particle glow)");
        partGlowAddPipeline_ = make_part(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, "vkCreateGraphicsPipelines(particle glow add)");
        partGlowAlphaBlendPipeline_ = make_part(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                "vkCreateGraphicsPipelines(particle glow alpha)");
    }

    VkAttachmentDescription tmatt{};
    tmatt.format = ldr_format_;
    tmatt.samples = VK_SAMPLE_COUNT_1_BIT;
    tmatt.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    tmatt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    tmatt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    tmatt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    tmatt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    tmatt.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference tmref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription tmsubp{};
    tmsubp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    tmsubp.colorAttachmentCount = 1;
    tmsubp.pColorAttachments = &tmref;
    VkRenderPassCreateInfo tmrpi{};
    tmrpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    tmrpi.attachmentCount = 1;
    tmrpi.pAttachments = &tmatt;
    tmrpi.subpassCount = 1;
    tmrpi.pSubpasses = &tmsubp;
    VkRenderPass rawTmPass = VK_NULL_HANDLE;
    check(vkCreateRenderPass(dev_, &tmrpi, nullptr, &rawTmPass), "vkCreateRenderPass(tonemap)");
    tmPass_ = Unique<VkRenderPass>(rawTmPass, [dev](VkRenderPass p) { vkDestroyRenderPass(dev, p, nullptr); });
    {
        VkDescriptorPoolSize tmpsize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
        VkDescriptorPoolCreateInfo tmdpi{};
        tmdpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        tmdpi.maxSets = 1;
        tmdpi.poolSizeCount = 1;
        tmdpi.pPoolSizes = &tmpsize;
        VkDescriptorPool rawTmPool = VK_NULL_HANDLE;
        check(vkCreateDescriptorPool(dev_, &tmdpi, nullptr, &rawTmPool), "vkCreateDescriptorPool(tonemap)");
        tmPool_ = Unique<VkDescriptorPool>(rawTmPool, [dev](VkDescriptorPool p) { vkDestroyDescriptorPool(dev, p, nullptr); });
        VkDescriptorSetAllocateInfo tmdsa{};
        tmdsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        tmdsa.descriptorPool = rawTmPool;
        tmdsa.descriptorSetCount = 1;
        VkDescriptorSetLayout tml = tmLayout_.get();
        tmdsa.pSetLayouts = &tml;
        check(vkAllocateDescriptorSets(dev_, &tmdsa, &tm_set_), "vkAllocateDescriptorSets(tonemap)");
        VkDescriptorImageInfo tfi{fogSampler_.get(), fogImg_.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet tfw = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        tfw.dstSet = tm_set_;
        tfw.dstBinding = 2;
        tfw.descriptorCount = 1;
        tfw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tfw.pImageInfo = &tfi;
        vkUpdateDescriptorSets(dev_, 1, &tfw, 0, nullptr);
    }
    {
        VkAttachmentDescription aoatt{};
        aoatt.format = kHdr;
        aoatt.samples = VK_SAMPLE_COUNT_1_BIT;
        aoatt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        aoatt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        aoatt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        aoatt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        aoatt.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        aoatt.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference aoref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription aosubp{};
        aosubp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        aosubp.colorAttachmentCount = 1;
        aosubp.pColorAttachments = &aoref;
        VkRenderPassCreateInfo aorpi{};
        aorpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        aorpi.attachmentCount = 1;
        aorpi.pAttachments = &aoatt;
        aorpi.subpassCount = 1;
        aorpi.pSubpasses = &aosubp;
        VkRenderPass rawAoPass = VK_NULL_HANDLE;
        check(vkCreateRenderPass(dev_, &aorpi, nullptr, &rawAoPass), "vkCreateRenderPass(ssao)");
        aoPass_ = Unique<VkRenderPass>(rawAoPass, [dev](VkRenderPass p) { vkDestroyRenderPass(dev, p, nullptr); });
    }
    {
        VkAttachmentDescription blatt{};
        blatt.format = kHdr;
        blatt.samples = VK_SAMPLE_COUNT_1_BIT;
        blatt.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        blatt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        blatt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        blatt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        blatt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        blatt.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference blref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription blsubp{};
        blsubp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        blsubp.colorAttachmentCount = 1;
        blsubp.pColorAttachments = &blref;
        VkSubpassDependency bldep{};
        bldep.srcSubpass = 0;
        bldep.dstSubpass = VK_SUBPASS_EXTERNAL;
        bldep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        bldep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        bldep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bldep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo blrpi{};
        blrpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        blrpi.attachmentCount = 1;
        blrpi.pAttachments = &blatt;
        blrpi.subpassCount = 1;
        blrpi.pSubpasses = &blsubp;
        blrpi.dependencyCount = 1;
        blrpi.pDependencies = &bldep;
        VkRenderPass rawBlPass = VK_NULL_HANDLE;
        check(vkCreateRenderPass(dev_, &blrpi, nullptr, &rawBlPass), "vkCreateRenderPass(bloom)");
        bloomPass_ = Unique<VkRenderPass>(rawBlPass, [dev](VkRenderPass p) { vkDestroyRenderPass(dev, p, nullptr); });
        VkDescriptorPoolSize blsize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12};
        VkDescriptorPoolCreateInfo bldpi{};
        bldpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        bldpi.maxSets = 3;
        bldpi.poolSizeCount = 1;
        bldpi.pPoolSizes = &blsize;
        VkDescriptorPool rawBlPool = VK_NULL_HANDLE;
        check(vkCreateDescriptorPool(dev_, &bldpi, nullptr, &rawBlPool), "vkCreateDescriptorPool(bloom)");
        bloomPool_ = Unique<VkDescriptorPool>(rawBlPool, [dev](VkDescriptorPool p) { vkDestroyDescriptorPool(dev, p, nullptr); });
        VkDescriptorSetLayout bll[3] = {tmLayout_.get(), tmLayout_.get(), tmLayout_.get()};
        VkDescriptorSetAllocateInfo bldsa{};
        bldsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        bldsa.descriptorPool = rawBlPool;
        bldsa.descriptorSetCount = 3;
        bldsa.pSetLayouts = bll;
        check(vkAllocateDescriptorSets(dev_, &bldsa, bloom_set_), "vkAllocateDescriptorSets(bloom)");
    }
    create_targets();
    {
        VkPushConstantRange tmpcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TonemapPC)};
        VkPipelineLayoutCreateInfo tmpli{};
        tmpli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        tmpli.setLayoutCount = 1;
        VkDescriptorSetLayout tml = tmLayout_.get();
        tmpli.pSetLayouts = &tml;
        tmpli.pushConstantRangeCount = 1;
        tmpli.pPushConstantRanges = &tmpcr;
        VkPipelineLayout rawTmPipeLayout = VK_NULL_HANDLE;
        check(vkCreatePipelineLayout(dev_, &tmpli, nullptr, &rawTmPipeLayout), "vkCreatePipelineLayout(tonemap)");
        tmPipeLayout_ = Unique<VkPipelineLayout>(rawTmPipeLayout, [dev](VkPipelineLayout l) { vkDestroyPipelineLayout(dev, l, nullptr); });
        tmvs_ = make_shader(dev_, kOverlayVertSpv, sizeof(kOverlayVertSpv));
        tmfs_ = make_shader(dev_, kTonemapFragSpv, sizeof(kTonemapFragSpv));
        VkPipelineShaderStageCreateInfo tmstages[2]{};
        tmstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        tmstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        tmstages[0].module = tmvs_.get();
        tmstages[0].pName = "main";
        tmstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        tmstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        tmstages[1].module = tmfs_.get();
        tmstages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo tmvin{};
        tmvin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineColorBlendAttachmentState tmcba{};
        tmcba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo tmcb{};
        tmcb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        tmcb.attachmentCount = 1;
        tmcb.pAttachments = &tmcba;
        VkGraphicsPipelineCreateInfo tmgpi{};
        tmgpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        tmgpi.stageCount = 2;
        tmgpi.pStages = tmstages;
        tmgpi.pVertexInputState = &tmvin;
        tmgpi.pInputAssemblyState = &ia;
        tmgpi.pViewportState = &vps;
        tmgpi.pDynamicState = &dyn;
        tmgpi.pRasterizationState = &rs;
        tmgpi.pMultisampleState = &ms;
        tmgpi.pColorBlendState = &tmcb;
        tmgpi.layout = rawTmPipeLayout;
        tmgpi.renderPass = tmPass_.get();
        VkPipeline rawTmPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &tmgpi, nullptr, &rawTmPipeline), "vkCreateGraphicsPipelines(tonemap)");
        tmPipeline_ = Unique<VkPipeline>(rawTmPipeline, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });
        bloomfs_ = make_shader(dev_, kBloomFragSpv, sizeof(kBloomFragSpv));
        tmstages[1].module = bloomfs_.get();
        tmgpi.renderPass = bloomPass_.get();
        VkPipeline rawBloom = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &tmgpi, nullptr, &rawBloom), "vkCreateGraphicsPipelines(bloom)");
        bloomPipeline_ = Unique<VkPipeline>(rawBloom, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });
        aofs_ = make_shader(dev_, kSsaoFragSpv, sizeof(kSsaoFragSpv));
        tmstages[1].module = aofs_.get();
        tmgpi.renderPass = aoPass_.get();
        tmcba.blendEnable = VK_TRUE;
        tmcba.colorBlendOp = VK_BLEND_OP_ADD;
        tmcba.alphaBlendOp = VK_BLEND_OP_ADD;
        tmcba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        tmcba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        tmcba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        tmcba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        tmcba.colorWriteMask = VK_COLOR_COMPONENT_A_BIT;
        VkPipeline rawAo = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &tmgpi, nullptr, &rawAo), "vkCreateGraphicsPipelines(ssao A)");
        aoPipelineA_ = Unique<VkPipeline>(rawAo, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });
        tmcba.dstColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        tmcba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        tmcba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        rawAo = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &tmgpi, nullptr, &rawAo), "vkCreateGraphicsPipelines(ssao B)");
        aoPipelineB_ = Unique<VkPipeline>(rawAo, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });
    }
}

void SceneRenderer::prepare(const std::vector<const SceneInstance*>& live, const std::vector<const SceneInstance*>* light_only) {
    for (const SceneInstance* p : live)
        if (p->fx.kind == FxKind::Creep && p->coverage && p->coverage->w && p->coverage->h &&
            p->coverage->alpha.size() == size_t(p->coverage->w) * p->coverage->h) {
            if (p->coverage->w != creep_w_ || p->coverage->h != creep_h_ || p->coverage->alpha != creep_last_) {
                creep_last_ = p->coverage->alpha;
                upload_creep_image(p->coverage->w, p->coverage->h, creep_last_);
            }
            break;
        }
    frame_lights_.clear();
    std::vector<const SceneInstance*> lit(live.begin(), live.end());
    if (light_only) lit.insert(lit.end(), light_only->begin(), light_only->end());
    for (const SceneInstance* instp : lit) {
        const SceneInstance& inst = *instp;
        if (!inst.model || inst.model->lights.empty()) continue;
        const formats::Model& m = *inst.model;
        const uint32_t seq = inst.seq >= 0 ? uint32_t(inst.seq) : UINT32_MAX;
        std::vector<Mat4> bw;
        bool posed = false;
        for (const formats::M3Light& L : m.lights) {
            if (L.type != formats::kLightPoint && L.type != formats::kLightSpot) continue;
            float intensity = L.intensity;
            if (L.intensity_flags && inst.seq >= 0) {
                formats::M3AnimRef ref;
                ref.interp = L.intensity_interp;
                ref.anim_flags = L.intensity_flags;
                ref.anim_id = L.intensity_id;
                intensity = animated_scalar(m, seq, ref, inst.time_ms, L.intensity, glayers(inst));
            }
            if (std::getenv("SC2_LIGHT_TRACE") && L.intensity_flags && inst.seq >= 0) {
                formats::M3AnimRef ref;
                ref.interp = L.intensity_interp;
                ref.anim_flags = L.intensity_flags;
                ref.anim_id = L.intensity_id;
                float lo = 1e30f, hi = -1e30f, sum = 0.0f;
                for (int k = 0; k < 64; ++k) {
                    const float v = animated_scalar(m, seq, ref, float(k) * 125.0f, L.intensity);
                    lo = std::min(lo, v); hi = std::max(hi, v); sum += v;
                }
                std::fprintf(stderr, "light track init %.2f now %.2f (t %.0f) over 8 s min %.2f max %.2f mean %.2f col=%.2f,%.2f,%.2f\n", double(L.intensity), double(intensity),
                             double(inst.time_ms), double(lo), double(hi), double(sum / 64.0f), double(L.color[0]), double(L.color[1]), double(L.color[2]));
            }
            if (intensity <= 0.0f || L.attenuation_far <= 0.0f) continue;
            if (!posed) {
                bw = instance_bones_world(inst, pose_world(m, seq, inst.seq >= 0 ? inst.time_ms : 0.0f, -1, 0.0f, glayers(inst)));
                posed = true;
            }
            const Mat4 mb = (L.bone >= 0 && size_t(L.bone) < bw.size()) ? bw[size_t(L.bone)] : inst.world;
            FrameLight f;
            f.pos = {mb.m[12], mb.m[13], mb.m[14]};
            static const float kLightGain = std::getenv("SC2_LIGHT_GAIN") ? float(std::atof(std::getenv("SC2_LIGHT_GAIN"))) : 1.0f;
            intensity *= kLightGain;
            f.col = {L.color[0] * intensity, L.color[1] * intensity, L.color[2] * intensity};
            float lscale = 1.0f;
            if (!std::getenv("SC2_LIGHT_NO_SCALE")) {
                const float sx = std::sqrt(mb.m[0] * mb.m[0] + mb.m[1] * mb.m[1] + mb.m[2] * mb.m[2]);
                const float sy = std::sqrt(mb.m[4] * mb.m[4] + mb.m[5] * mb.m[5] + mb.m[6] * mb.m[6]);
                const float sz = std::sqrt(mb.m[8] * mb.m[8] + mb.m[9] * mb.m[9] + mb.m[10] * mb.m[10]);
                const float sm = (sx + sy + sz) / 3.0f;
                if (sm > 1e-4f) lscale = sm;
            }
            f.att_near = L.attenuation_near * lscale;
            f.att_far = L.attenuation_far * lscale;
            if (L.type == formats::kLightSpot && L.falloff > 1e-3f) {
                Vec3 z{-mb.m[8], -mb.m[9], -mb.m[10]};
                const float zl = std::sqrt(z.x * z.x + z.y * z.y + z.z * z.z);
                if (zl > 1e-6f) {
                    f.spot_dir = {z.x / zl, z.y / zl, z.z / zl};
                    static const float kCone = std::getenv("SC2_SPOT_CONE_FACTOR") ? float(std::atof(std::getenv("SC2_SPOT_CONE_FACTOR"))) : 0.5f;
                    f.cos_outer = std::cos(std::min(kCone * L.falloff, 1.5607f));
                    const float ci = std::cos(std::min(kCone * std::min(L.hotspot, L.falloff), 1.5607f));
                    f.cone_scale = 1.0f / std::max(ci - f.cos_outer, 1e-3f);
                }
            }
            if (std::getenv("SC2_LIGHT_TRACE")) std::fprintf(stderr, "light type=%u hot=%.3f fall=%.3f X=(%.2f,%.2f,%.2f) Y=(%.2f,%.2f,%.2f) Z=(%.2f,%.2f,%.2f) flags=%08x col=%.2f,%.2f,%.2f int=%.2f near=%.2f far=%.2f z=%.2f at %.2f %.2f init %.2f animflags %u seq %d t %.0f\n", L.type, double(L.hotspot), double(L.falloff), mb.m[0], mb.m[1], mb.m[2], mb.m[4], mb.m[5], mb.m[6], mb.m[8], mb.m[9], mb.m[10], L.flags, L.color[0], L.color[1], L.color[2], intensity, L.attenuation_near, L.attenuation_far, f.pos.z, double(f.pos.x), double(f.pos.y), double(L.intensity), unsigned(L.intensity_flags), inst.seq, double(inst.time_ms));
                        frame_lights_.push_back(f);
        }
    }
    Vec3 wmn{1e30f, 1e30f, 1e30f}, wmx{-1e30f, -1e30f, -1e30f};
    Vec3 gmn{1e30f, 1e30f, 1e30f}, gmx{-1e30f, -1e30f, -1e30f};
    inst_aabb_.assign(live.size(), {Vec3{0, 0, 0}, Vec3{0, 0, 0}});
    for (size_t li = 0; li < live.size(); ++li) {
        const SceneInstance* inst = live[li];
        const formats::Model& m = *inst->model;
        const Mesh& mesh = ensure_mesh(m, inst->mesh_version);
        const Vec3 lmn = mesh.lmin, lmx = mesh.lmax;
        Vec3 imn{1e30f, 1e30f, 1e30f}, imx{-1e30f, -1e30f, -1e30f};
        for (int cx = 0; cx < 2; ++cx)
            for (int cy = 0; cy < 2; ++cy)
                for (int cz = 0; cz < 2; ++cz) {
                    Vec3 corner{cx ? lmx.x : lmn.x, cy ? lmx.y : lmn.y, cz ? lmx.z : lmn.z};
                    Vec3 w = mul_point(inst->world, corner);
                    for (int a = 0; a < 3; ++a) {
                        (&imn.x)[a] = std::min((&imn.x)[a], (&w.x)[a]);
                        (&imx.x)[a] = std::max((&imx.x)[a], (&w.x)[a]);
                    }
                }
        if (inst->seq >= 0 && !m.bones.empty()) {
            const std::vector<Mat4> bw = pose_world(m, uint32_t(inst->seq), inst->time_ms, -1, 0.0f, glayers(*inst));
            const float dx = lmx.x - lmn.x, dy = lmx.y - lmn.y, dz = lmx.z - lmn.z;
            const float diag = std::sqrt(dx * dx + dy * dy + dz * dz);
            float scale = 0.0f;
            for (int c = 0; c < 3; ++c) {
                const float* col = &inst->world.m[std::size_t(c) * 4];
                scale = std::max(scale, std::sqrt(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]));
            }
            const float r = diag * scale;
            for (const Mat4& b : bw) {
                const Vec3 o = mul_point(inst->world, Vec3{b.m[12], b.m[13], b.m[14]});
                imn = {std::min(imn.x, o.x - r), std::min(imn.y, o.y - r), std::min(imn.z, o.z - r)};
                imx = {std::max(imx.x, o.x + r), std::max(imx.y, o.y + r), std::max(imx.z, o.z + r)};
            }
        }
        inst_aabb_[li] = {imn, imx};
        if (std::getenv("SC2_LIGHT_TRACE") && (imx.x - imn.x > 300.0f || imx.z - imn.z > 300.0f))
            std::fprintf(stderr, "huge instance: %s verts=%u seq=%d local (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) world (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)\n",
                         m.name.c_str(), m.vertex_count, inst->seq, lmn.x, lmn.y, lmn.z, lmx.x, lmx.y, lmx.z, imn.x, imn.y, imn.z, imx.x, imx.y, imx.z);
        for (int a = 0; a < 3; ++a) {
            (&wmn.x)[a] = std::min((&wmn.x)[a], (&imn.x)[a]);
            (&wmx.x)[a] = std::max((&wmx.x)[a], (&imx.x)[a]);
        }
        if (m.has_splat_payload())
            for (int a = 0; a < 3; ++a) {
                (&gmn.x)[a] = std::min((&gmn.x)[a], (&imn.x)[a]);
                (&gmx.x)[a] = std::max((&gmx.x)[a], (&imx.x)[a]);
            }
    }
    if (gmn.x <= gmx.x) {
        wmn = {std::max(wmn.x, gmn.x - 10.0f), std::max(wmn.y, gmn.y - 10.0f), std::max(wmn.z, gmn.z - 5.0f)};
        wmx = {std::min(wmx.x, gmx.x + 10.0f), std::min(wmx.y, gmx.y + 10.0f), std::min(wmx.z, gmx.z + 40.0f)};
    }
    if (wmn.x > wmx.x) { wmn = {0, 0, 0}; wmx = {0, 0, 0}; }
    aabb_min_ = wmn;
    aabb_max_ = wmx;
}

static float cutout_threshold(const formats::M3Material& mm) {
    const float t = float(mm.cutout_thresh) / 255.0f;
    static const bool kRaw = std::getenv("SC2_CUTOUT_RAW_ALPHA") != nullptr;
    if (kRaw) return t;
    const formats::M3Layer* diff = nullptr;
    const formats::M3Layer* mask = nullptr;
    for (const formats::M3Layer& ly : mm.layers) {
        if (ly.usage == "Diffuse") diff = &ly;
        else if (ly.usage == "AlphaMask") mask = &ly;
    }
    static const bool kOwnOnly = std::getenv("SC2_CUTOUT_DIFFUSE_ALPHA") != nullptr;
    if (!diff || !mask || mask->raw_record.empty() || mask->word44 < 2 || mask->word44 > 5 || mask->image_path.empty()) return t;
    if (kOwnOnly) {
        if (mask->word44 != 2 || mask->image_path.size() != diff->image_path.size()) return t;
        for (std::size_t i = 0; i < mask->image_path.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(mask->image_path[i])) != std::tolower(static_cast<unsigned char>(diff->image_path[i]))) return t;
    }
    const float mul = mask->anim.bright_mult, add = mask->anim.midtone_offset;
    if (mul <= 0.0f) return t;
    return std::min(1.0f, std::max(1.0f / 255.0f, (t - add) / mul));
}

static int cutout_mask_channel(const formats::M3Material& mm) {
    static const bool kOld = std::getenv("SC2_CUTOUT_DIFFUSE_ALPHA") != nullptr;
    if (kOld || mm.blend_mode != formats::kBlendOpaque || mm.cutout_thresh == 0) return 0;
    const formats::M3Layer* diff = nullptr;
    const formats::M3Layer* mask = nullptr;
    for (const formats::M3Layer& ly : mm.layers) {
        if (ly.usage == "Diffuse") diff = &ly;
        else if (ly.usage == "AlphaMask") mask = &ly;
    }
    if (!mask || mask->raw_record.empty() || mask->image_path.empty() || mask->word44 < 2 || mask->word44 > 5) return 0;
    bool same = diff && mask->word44 == 2 && mask->image_path.size() == diff->image_path.size();
    for (std::size_t i = 0; same && i < mask->image_path.size(); ++i)
        same = std::tolower(static_cast<unsigned char>(mask->image_path[i])) == std::tolower(static_cast<unsigned char>(diff->image_path[i]));
    static const bool kTrace = std::getenv("SC2_CUTOUT_TRACE") != nullptr;
    if (kTrace && !same) std::fprintf(stderr, "[cutmask] '%s' channel %d thresh %u mask '%s'\n", mm.name.c_str(), int(mask->word44) - 1, unsigned(mm.cutout_thresh), mask->image_path.c_str());
    return same ? 0 : int(mask->word44) - 1;
}

void SceneRenderer::record(VkCommandBuffer cmd, const std::vector<const SceneInstance*>& live,
                           const Camera& cam, const std::vector<ParticleVertex>& pverts,
                           const std::vector<ParticleGroup>& pgroups) {
    SC2_PROF_SCOPE("SceneRenderer::record");
    flush_pending_fog(cmd);
    Mat4 view_proj = cam.view_proj(float(width_) / float(height_));
    static const bool kSsaoEnv = std::getenv("SC2_SSAO") != nullptr;
    const bool ssao_on = kSsaoEnv && tun_.ao_radius > 0.0f && !cam.transparent_clear && !cam.orthographic && tun_.gamma_space && tun_.rig.present;

    Vec3 shadow_dir = tun_.light_dir;
    const bool rig_swap = std::getenv("SC2_RIG_SWAP") && tun_.rig.present && tun_.rig.fill.present;
    if (rig_swap) {
        const float sgn = dot(tun_.light_dir, tun_.rig.key.dir) < 0.0f ? -1.0f : 1.0f;
        shadow_dir = normalize(Vec3{sgn * tun_.rig.fill.dir.x, sgn * tun_.rig.fill.dir.y, sgn * tun_.rig.fill.dir.z});
    }
    const ShadowBox sbox =
        fit_shadow_ortho(aabb_min_, aabb_max_, cam, float(width_) / float(height_), shadow_dir,
                         tun_.fit_pad, tun_.shadow_dim, tun_.shadow_clip, tun_.camera_fit_shadow);
    if (std::getenv("SC2_LIGHT_TRACE"))
        std::fprintf(stderr, "shadow fit: aabb (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) box x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f] texel %.3f clip %.1f camfit %d\n",
                     aabb_min_.x, aabb_min_.y, aabb_min_.z, aabb_max_.x, aabb_max_.y, aabb_max_.z, sbox.xmn, sbox.xmx, sbox.ymn, sbox.ymx,
                     sbox.znear, sbox.zfar, double(sbox.texel(tun_.shadow_dim)), double(tun_.shadow_clip), int(tun_.camera_fit_shadow));
    Mat4 lview = look_at(sbox.eye, sbox.center, sbox.up);
    Mat4 lproj = ortho(sbox.xmn, sbox.xmx, sbox.ymn, sbox.ymx, sbox.znear, sbox.zfar);
    Mat4 lightVP = lproj * lview;

    GlobalsUBO globals{};
    std::memcpy(globals.viewProj, view_proj.m.data(), 64);
    std::memcpy(globals.lightVP, lightVP.m.data(), 64);
    globals.shadowParams[0] = 1.0f / float(tun_.shadow_dim);
    globals.shadowParams[1] = tun_.depth_bias;
    if (const char* sb = std::getenv("SC2_SHADOW_BIAS")) globals.shadowParams[1] = float(std::atof(sb));
    {
        const char* sw = std::getenv("SC2_SHADOW_BIAS_WORLD");
        const float world_bias = sw ? float(std::atof(sw)) : (tun_.camera_fit_shadow ? 0.04f : 0.0f);
        if (world_bias > 0.0f && sbox.zfar > sbox.znear && !std::getenv("SC2_SHADOW_BIAS")) globals.shadowParams[1] = world_bias / (sbox.zfar - sbox.znear);
    }
    globals.shadowParams[2] = std::getenv("SC2_NO_SHADOW") ? 0.0f : tun_.shadow_strength;
    globals.shadowParams[3] = tun_.gloss;
    globals.eyePos[0] = cam.eye.x;
    globals.eyePos[1] = cam.eye.y;
    globals.eyePos[2] = cam.eye.z;
    globals.eyePos[3] = std::getenv("SC2_NO_SPEC") ? 0.0f : tun_.spec_strength;
    globals.lightDir[0] = shadow_dir.x;
    globals.lightDir[1] = shadow_dir.y;
    globals.lightDir[2] = shadow_dir.z;
    globals.lightDir[3] = 0.0f;
    globals.lightDir[3] = tun_.gamma_space ? 1.0f : 0.0f;
    if (const char* dl = std::getenv("SC2_DBL_LAMBERT")) globals.lightDir[3] = std::string(dl) == "all" ? 2.0f : std::string(dl) == "off" ? 0.0f : 1.0f;
    if (std::getenv("SC2_SPEC_GATE_NL0")) globals.lightDir[3] += 0.25f;
    if (std::getenv("SC2_PROJ_MODEL_LIGHT")) globals.lightDir[3] += 0.5f;
    if (std::getenv("SC2_SPEC_LAYER_ALPHA")) globals.lightDir[3] += 0.125f;
    globals.rigAmbient[0] = tun_.rig.ambient[0];
    globals.rigAmbient[1] = tun_.rig.ambient[1];
    globals.rigAmbient[2] = tun_.rig.ambient[2];
    globals.rigAmbient[3] = tun_.rig.present ? 1.0f : 0.0f;
    if (std::getenv("SC2_LIGHT_TRACE"))
        std::fprintf(stderr, "rig ambient=(%.3f,%.3f,%.3f) HDRAmbientMultiplier %.3f bloom threshold %.3f gain %.3f scale %.3f\n", double(tun_.rig.ambient[0]),
                     double(tun_.rig.ambient[1]), double(tun_.rig.ambient[2]), double(tun_.hdr_ambient_mult), double(tun_.bloom_threshold), double(tun_.bloom_gain), double(tun_.bloom_scale));
    {
        const RigLight* ls[3] = {&tun_.rig.key, &tun_.rig.fill, &tun_.rig.back};
        if (rig_swap) std::swap(ls[0], ls[1]);
        for (int i = 0; i < 3; ++i) {
            globals.rigDir[i][0] = ls[i]->dir.x;
            globals.rigDir[i][1] = ls[i]->dir.y;
            globals.rigDir[i][2] = ls[i]->dir.z;
            globals.rigDir[i][3] = (ls[i]->present && !(i > 0 && std::getenv("SC2_NO_FILLBACK"))) ? 1.0f : 0.0f;
            for (int c = 0; c < 3; ++c) globals.rigColor[i][c] = ls[i]->color[c];
            if (const char* fb = i > 0 ? std::getenv("SC2_FILLBACK") : nullptr) {
                const std::string mode(fb);
                if (mode == "neg" || mode == "negxy") { globals.rigDir[i][0] = -ls[i]->dir.x; globals.rigDir[i][1] = -ls[i]->dir.y; }
                if (mode == "neg" || mode == "negz") globals.rigDir[i][2] = -ls[i]->dir.z;
                if (mode.find_first_not_of("0123456789.") == std::string::npos)
                    for (int c = 0; c < 3; ++c) globals.rigColor[i][c] *= float(std::atof(fb));
            }
            globals.rigColor[i][3] = 0.0f;
            if (std::getenv("SC2_LIGHT_TRACE"))
                std::fprintf(stderr, "rig[%d] present=%d travel=(%.3f,%.3f,%.3f) colour=(%.3f,%.3f,%.3f)\n", i, int(ls[i]->present), ls[i]->dir.x, ls[i]->dir.y,
                             ls[i]->dir.z, ls[i]->color[0], ls[i]->color[1], ls[i]->color[2]);
        }
        globals.rigParams[0] = std::getenv("SC2_SHADOW_FLOOR") ? float(std::atof(std::getenv("SC2_SHADOW_FLOOR"))) : tun_.rig_shadow_floor;
        globals.rigParams[1] = tun_.hdr_ambient_mult;
        globals.rigParams[2] = tun_.hdr_diffuse_mult;
        globals.rigParams[3] = tun_.hdr_emis_mult;
        for (int c = 0; c < 3; ++c) globals.keySpec[c] = tun_.rig.key.spec[c] * (tun_.gamma_space ? tun_.hdr_spec_mult : 1.0f);
        globals.keySpec[3] = (tun_.gamma_space && tun_.rig.present) ? 1.0f : 0.0f;
        for (int i = 0; i < 3; ++i) {
            for (int c = 0; c < 3; ++c) globals.rigSpec[i][c] = ls[i]->spec[c] * (tun_.gamma_space ? tun_.hdr_spec_mult : 1.0f);
            globals.rigSpec[i][3] = 0.0f;
            if (std::getenv("SC2_LIGHT_TRACE"))
                std::fprintf(stderr, "rig[%d] spec=(%.3f,%.3f,%.3f)\n", i, globals.rigSpec[i][0], globals.rigSpec[i][1], globals.rigSpec[i][2]);
        }
        static const bool spec_all_lights = std::getenv("SC2_SPEC_ALL_LIGHTS") != nullptr;
        globals.rigSpec[0][3] = spec_all_lights ? 1.0f : 0.0f;
    }
    for (const SceneInstance* p : live)
        if (p->model && p->model->has_splat_payload()) {
            if (TerrainSplat* ts = ensure_splat(*p->model)) {
                globals.terrainExtent[0] = ts->extent[0];
                globals.terrainExtent[1] = ts->extent[1];
            }
            bool any_height = false;
            for (size_t L = 0; L < 8 && L < p->model->splat_height.size(); ++L) {
                for (int k = 0; k < 3; ++k) globals.terrainHeight[L][k] = p->model->splat_height[L][size_t(k)];
                any_height = any_height || p->model->splat_height[L][2] > 0.5f;
            }
            globals.terrainHeight[0][3] = any_height ? 1.0f : 0.0f;
            break;
        }
    globals.fogExtent[0] = fog_world_w_;
    globals.fogExtent[1] = fog_world_h_;
    globals.fogExtent[2] = fog_present_ && fog_in_scene() ? 1.0f : 0.0f;
    static const float kTeamPow = std::getenv("SC2_TEAM_COLOR_POW") ? float(std::atof(std::getenv("SC2_TEAM_COLOR_POW"))) : 0.0f;
    globals.fogExtent[3] = kTeamPow;
    {
        std::vector<FrameLight> fl = frame_lights_;
        std::sort(fl.begin(), fl.end(), [&](const FrameLight& a, const FrameLight& b) {
            const float da = (a.pos.x - cam.center.x) * (a.pos.x - cam.center.x) + (a.pos.y - cam.center.y) * (a.pos.y - cam.center.y) + (a.pos.z - cam.center.z) * (a.pos.z - cam.center.z);
            const float db = (b.pos.x - cam.center.x) * (b.pos.x - cam.center.x) + (b.pos.y - cam.center.y) * (b.pos.y - cam.center.y) + (b.pos.z - cam.center.z) * (b.pos.z - cam.center.z);
            return std::sqrt(da) - a.att_far < std::sqrt(db) - b.att_far;
        });
        const size_t n = std::min<size_t>(fl.size(), kMaxFrameLights);
        if (std::getenv("SC2_LIGHT_TRACE")) {
            const auto reach = [&](const FrameLight& a) {
                return std::sqrt((a.pos.x - cam.center.x) * (a.pos.x - cam.center.x) + (a.pos.y - cam.center.y) * (a.pos.y - cam.center.y)) - a.att_far;
            };
            std::fprintf(stderr, "frame lights kept %zu of %zu, last kept ground gap %.2f, first dropped %.2f\n", n, fl.size(), n ? double(reach(fl[n - 1])) : 0.0,
                         n < fl.size() ? double(reach(fl[n])) : 0.0);
        }
        for (size_t i = 0; i < n; ++i) {
            globals.lightPos[i][0] = fl[i].pos.x; globals.lightPos[i][1] = fl[i].pos.y; globals.lightPos[i][2] = fl[i].pos.z; globals.lightPos[i][3] = fl[i].att_far;
            globals.lightCol[i][0] = fl[i].col.x; globals.lightCol[i][1] = fl[i].col.y; globals.lightCol[i][2] = fl[i].col.z; globals.lightCol[i][3] = fl[i].att_near;
            globals.lightSpot[i][0] = fl[i].spot_dir.x; globals.lightSpot[i][1] = fl[i].spot_dir.y; globals.lightSpot[i][2] = fl[i].spot_dir.z;
            globals.lightSpot[i][3] = fl[i].cos_outer;
            globals.lightCone[i][0] = fl[i].cone_scale;
            globals.lightCone[i][1] = fl[i].cone_scale > 0.0f && !std::getenv("SC2_NO_SPOT_CONE") ? 1.0f : 0.0f;
            globals.lightCone[i][2] = globals.lightCone[i][3] = 0.0f;
        }
        globals.lightMeta[0] = std::getenv("SC2_NO_POINTLIGHTS") ? 0.0f : float(n);
        globals.lightMeta[1] = std::getenv("SC2_LIGHT_FALLOFF") ? float(std::atoi(std::getenv("SC2_LIGHT_FALLOFF"))) : 0.0f;
        globals.lightMeta[2] = std::getenv("SC2_EMIS_LINEAR") ? 0.0f : 1.0f;
        globals.lightMeta[3] = std::getenv("SC2_POINT_SPEC") ? float(std::atof(std::getenv("SC2_POINT_SPEC"))) : 0.0f;
        lights_drawn_ = n;
        lights_seen_ = frame_lights_.size();
    }
    globals.screen[0] = float(width_);
    globals.screen[1] = float(height_);
    globals.screen[2] = (!cam.transparent_clear && !std::getenv("SC2_NO_GLOW_MASK") && std::getenv("SC2_EMISSIVE_SHARE")) ? 1.0f : 0.0f;
    if (const char* nv = std::getenv("SC2_SHOW_NV")) globals.screen[2] = std::atoi(nv) == 2 ? 8.0f : 7.0f;
    if (std::getenv("SC2_SHOW_COVER")) globals.screen[2] = 5.0f;
    globals.screen[3] = ssao_on ? 1.0f : 0.0f;
    globals.creepParams[2] = std::getenv("SC2_WATER_NO_DEPTH_TINT") ? 0.0f : cam.znear;
    globals.creepParams[3] = cam.zfar;
    {
        const float world_per_texel = (sbox.xmx - sbox.xmn) / float(tun_.shadow_dim);
        static const float kPenScale = std::getenv("SC2_SHADOW_PENUMBRA_SCALE") ? float(std::atof(std::getenv("SC2_SHADOW_PENUMBRA_SCALE"))) : 1.0f;
        float r = (tun_.shadow_penumbra > 0.0f && world_per_texel > 1e-6f) ? kPenScale * tun_.shadow_penumbra / world_per_texel : 0.0f;
        if (r > 0.0f) r = std::min(32.0f, std::max(1.0f, r));
        if (const char* sp = std::getenv("SC2_SHADOW_PCF")) r = float(std::atof(sp));
        if (const char* sw = std::getenv("SC2_SHADOW_PENUMBRA_WORLD"); sw && world_per_texel > 1e-6f) r = float(std::atof(sw)) / world_per_texel;
        if (std::getenv("SC2_LIGHT_TRACE")) {
            static int told = 0;
            if (told++ < 2)
                std::fprintf(stderr, "shadow kernel: authored penumbra %.4f, world per texel %.5f (box %.2f over %u), radius %.2f texels = %.3f world\n",
                             double(tun_.shadow_penumbra), double(world_per_texel), double(sbox.xmx - sbox.xmn), unsigned(tun_.shadow_dim), double(r), double(r * world_per_texel));
        }
        globals.shadowExtra[0] = r;
        globals.shadowExtra[1] = std::getenv("SC2_SHADOW_NO_SLOPE_BIAS") ? 0.0f : 1.0f;
        globals.shadowExtra[2] = std::getenv("SC2_TEAM_EMIS_POW") ? float(std::atof(std::getenv("SC2_TEAM_EMIS_POW"))) : 0.0f;
        globals.heightFog[0] = tun_.fog_density;
        globals.heightFog[1] = tun_.fog_falloff;
        globals.heightFog[2] = tun_.fog_start;
        globals.heightFog[3] = tun_.height_fog ? (tun_.gamma_space ? 2.0f : 1.0f) : 0.0f;
        for (int k = 0; k < 3; ++k) globals.heightFogColor[k] = tun_.fog_color[k];
        globals.creepParams[0] = tun_.creep_spec_mult;
        globals.creepParams[1] = tun_.creep_spec_exp;
        globals.terrainColorize[0] = tun_.terrain_diffuse * (std::getenv("SC2_HDR_DIFFUSE_ALL") ? tun_.hdr_diffuse_mult : 1.0f);
        globals.terrainColorize[1] = std::getenv("SC2_NO_SPEC") ? 0.0f : tun_.terrain_spec;
        if (const char* tg = std::getenv("SC2_TERRAIN_SPEC_GAIN")) globals.terrainColorize[1] *= float(std::atof(tg));
    if (const char* ts = std::getenv("SC2_TERRAIN_SPEC")) {
        const std::string mode(ts);
        if (mode == "off") {
            globals.terrainColorize[1] = 0.0f;
        } else if (mode == "nohdr" && tun_.gamma_space && tun_.hdr_spec_mult > 0.0f) {
            globals.terrainColorize[1] = tun_.terrain_spec / tun_.hdr_spec_mult;
        }
    }
    if (tun_.gamma_space && tun_.hdr_spec_mult > 0.0f && globals.terrainColorize[1] != 0.0f &&
        !(std::getenv("SC2_TERRAIN_SPEC") && (std::string(std::getenv("SC2_TERRAIN_SPEC")) == "hdr" || std::string(std::getenv("SC2_TERRAIN_SPEC")) == "nohdr")))
        globals.terrainColorize[1] /= tun_.hdr_spec_mult;
    const bool terrain_spec_undimmed = !(std::getenv("SC2_TERRAIN_SPEC") && std::string(std::getenv("SC2_TERRAIN_SPEC")) == "dimmed");
    if (std::getenv("SC2_LIGHT_TRACE")) {
        std::fprintf(stderr, "terrain: HDRDiffuse %.3f HDRSpecular %.3f SpecularExp %.2f | light HDRSpecMultiplier %.3f HDRDiffuseMultiplier %.3f\n", tun_.terrain_diffuse,
                     tun_.terrain_spec, tun_.terrain_spec_exp, tun_.hdr_spec_mult, tun_.hdr_diffuse_mult);
    }
        globals.terrainColorize[2] = tun_.terrain_spec_exp;
        if (const char* te = std::getenv("SC2_TERRAIN_SPEC_EXP_GAIN")) globals.terrainColorize[2] *= float(std::atof(te));
        globals.terrainColorize[3] = terrain_spec_undimmed ? 1.0f : 0.0f;
        globals.terrainColorize2[0] = tun_.col_contrast;
        globals.terrainColorize2[1] = tun_.col_blend > 0.0f ? 1.0f : 0.0f;
        globals.terrainColorize2[2] = std::getenv("SC2_TERRAIN_BUMP") ? float(std::atof(std::getenv("SC2_TERRAIN_BUMP"))) : kTerrainBumpGain;
        globals.terrainColorize2[3] = std::getenv("SC2_TERRAIN_NORMAL_LOD") ? float(std::atof(std::getenv("SC2_TERRAIN_NORMAL_LOD"))) : 0.0f;
    }
    std::vector<FxRecord> fxrecs(1, FxRecord{});
    std::vector<uint32_t> fx_index(live.size(), 0u);
    bool need_copy = false;
    bool water_depth = false;
    static const bool kWaterDepthTint = std::getenv("SC2_WATER_NO_DEPTH_TINT") == nullptr;
    for (size_t i = 0; i < live.size(); ++i) {
        const InstanceFX& f = live[i]->fx;
        if (!need_copy && live[i]->model && live[i]->model->vertex_count && ensure_mesh(*live[i]->model, live[i]->mesh_version).any_displace) need_copy = true;
        if (f.is_default()) continue;
        fx_index[i] = uint32_t(fxrecs.size());
        fxrecs.push_back(fx_record_of(f));
        if (f.kind == FxKind::Cloak || f.kind == FxKind::Water) need_copy = true;
        if (f.kind == FxKind::Water && kWaterDepthTint) water_depth = true;
    }
    std::vector<std::vector<uint32_t>> draw_fx(live.size());
    for (size_t i = 0; i < live.size(); ++i) {
        const SceneInstance& inst = *live[i];
        if (!inst.model || (inst.model->vertex_count == 0)) continue;
        const Mesh& mesh = ensure_mesh(*inst.model, inst.mesh_version);
        if (!mesh.any_uvx && !mesh.any_masklv && !mesh.any_specm && !mesh.any_env && !mesh.any_uvsrc) continue;
        const bool ext_free = inst.fx.kind != FxKind::Water && inst.fx.kind != FxKind::Creep;
        for (size_t d = 0; d < mesh.draws.size(); ++d) {
            float uvx[3] = {1.0f, 1.0f, 0.0f};
            const bool has_uvx = mesh.any_uvx && mesh.draw_uvx[d] &&
                                 material_uv_xform(*inst.model, mesh.draws[d].material, inst.seq, inst.time_ms, uvx, &inst.global_layers);
            const bool has_lv = ext_free && mesh.any_masklv && d < mesh.draw_masklv.size() &&
                                (mesh.draw_masklv[d][0] != 0.0f || mesh.draw_masklv[d][1] != 0.0f);
            const bool has_sm = ext_free && mesh.any_specm && d < mesh.draw_specm.size() && mesh.draw_specm[d] != 0.0f;
            if (const char* st = std::getenv("SC2_SPECM_TRACE"); st && mesh.draws[d].material >= 0 &&
                inst.model->materials[std::size_t(mesh.draws[d].material)].name == st) {
                static int nst = 0;
                if (nst++ < 200)
                    std::fprintf(stderr, "[specm] '%s' verts %u draw %zu specm %.4f has_sm %d ext_free %d any_specm %d\n", st, inst.model->vertex_count, d,
                                 d < mesh.draw_specm.size() ? double(mesh.draw_specm[d]) : -9.0, int(has_sm), int(ext_free), int(mesh.any_specm));
            }
            const bool has_env = ext_free && mesh.any_env && d < mesh.draw_env.size() && mesh.draw_env[d][0] > 0.0f &&
                                 !std::getenv("SC2_NO_ENVIO");
            const std::uint16_t uvsrc = mesh.any_uvsrc && d < mesh.draw_uvsrc.size() ? mesh.draw_uvsrc[d] : std::uint16_t(0);
            const bool has_mx = mesh.any_maskx && d < mesh.draw_maskx.size() && (mesh.draw_maskx[d][3] > 0.5f || mesh.draw_maskx[d][7] > 0.5f || mesh.draw_maskx[d][10] > 0.0f);
            const bool has_fr = mesh.any_fres && d < mesh.draw_fres.size() &&
                                (mesh.draw_fres[d][3] > 0.5f || mesh.draw_fres[d][7] > 0.5f || mesh.draw_fres[d][11] > 0.5f ||
                                 mesh.draw_fres[d][15] > 0.5f || mesh.draw_fres[d][19] > 0.5f || mesh.draw_fres[d][23] > 0.5f);
            float fb_code = 0.0f, fb_frame = 0.0f;
            {
                const int fmi = mesh.draws[d].material;
                if (fmi >= 0 && std::size_t(fmi) < inst.model->materials.size())
                    for (const formats::M3Layer& L : inst.model->materials[std::size_t(fmi)].layers)
                        if (L.usage == "Emissive" && L.anim.flipbook_rows > 0) {
                            fb_code = -float(L.anim.flipbook_cols + 64u * L.anim.flipbook_rows);
                            fb_frame = L.anim.flipbook_frame;
                            if (inst.seq >= 0 && L.anim.flipbook_frame_flags)
                                animated_layer_field(*inst.model, std::uint32_t(inst.seq), L.anim.flipbook_frame_interp, L.anim.flipbook_frame_flags, L.anim.flipbook_frame_id,
                                                     inst.time_ms, 1, &L.anim.flipbook_frame, &fb_frame, &inst.global_layers);
                            break;
                        }
            }
            const bool has_fb = fb_code < 0.0f;
            float e2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            bool has_e2 = false;
            if (d < mesh.draw_tex.size() && mesh.draw_tex[d][10] != kDefaultEmissive) {
                const int emi = mesh.draws[d].material;
                if (emi >= 0 && std::size_t(emi) < inst.model->materials.size()) {
                    const formats::M3Material& em = inst.model->materials[std::size_t(emi)];
                    for (const formats::M3Layer& L : em.layers) {
                        if (L.usage != "Emissive2" || L.raw_record.empty()) continue;
                        const formats::M3Layer::AnimRefs& a = L.anim;
                        float mul = a.bright_mult, add = a.midtone_offset, br = a.brightness;
                        if (inst.seq >= 0) {
                            const std::uint32_t s = std::uint32_t(inst.seq);
                            animated_layer_field(*inst.model, s, a.bright_mult_interp, a.bright_mult_flags, a.bright_mult_id, inst.time_ms, 1, &a.bright_mult, &mul, &inst.global_layers);
                            animated_layer_field(*inst.model, s, a.midtone_interp, a.midtone_flags, a.midtone_id, inst.time_ms, 1, &a.midtone_offset, &add, &inst.global_layers);
                            animated_layer_field(*inst.model, s, a.brightness_interp, a.brightness_flags, a.brightness_id, inst.time_ms, 1, &a.brightness, &br, &inst.global_layers);
                        }
                        const std::uint32_t sel = (L.word44 >= 2 && L.word44 <= 5) ? L.word44 - 1 : (L.word44 == 1 ? 5u : 0u);
                        e2[0] = mul;
                        e2[1] = add;
                        e2[2] = br * em.emis_mult;
                        e2[3] = float(em.emis2_op + 16u * sel + (L.word40 == 1 ? 256u : 0u));
                        has_e2 = true;
                        if (const char* dt = std::getenv("SC2_DRAW_TRACE"); dt && em.name.find(dt) != std::string::npos) {
                            static int ne2 = 0;
                            if (ne2++ < 6)
                                std::fprintf(stderr, "[drawtrace] '%s' emissive2 '%s' mult %.3f add %.3f brightness %.3f emisMult %.2f op %u (emissive op %u) select %u seq %d t %.0f\n", em.name.c_str(),
                                             L.image_path.c_str(), double(mul), double(add), double(br), double(em.emis_mult), em.emis2_op, em.emis_op, sel, inst.seq, double(inst.time_ms));
                        }
                        break;
                    }
                }
            }
            float euvo[2] = {0.0f, 0.0f};
            bool has_eu = false;
            static const bool kEmisUvAnim = std::getenv("SC2_NO_EMIS_UV_ANIM") == nullptr;
            if (kEmisUvAnim && inst.seq >= 0) {
                const int emi = mesh.draws[d].material;
                if (emi >= 0 && std::size_t(emi) < inst.model->materials.size())
                    for (const formats::M3Layer& L : inst.model->materials[std::size_t(emi)].layers) {
                        if (L.usage != "Emissive") continue;
                        animated_layer_field(*inst.model, std::uint32_t(inst.seq), L.anim.uv_offset_interp, L.anim.uv_offset_flags, L.anim.uv_offset_id, inst.time_ms, 2,
                                             L.anim.uv_offset, euvo, &inst.global_layers);
                        has_eu = euvo[0] != 0.0f || euvo[1] != 0.0f;
                        break;
                    }
            }
            if (!has_uvx && !has_lv && !has_sm && !has_env && !uvsrc && !has_mx && !has_fr && !has_fb && !has_e2 && !has_eu) continue;
            if (draw_fx[i].empty()) draw_fx[i].assign(mesh.draws.size(), fx_index[i]);
            InstanceFX f = inst.fx;
            if (has_uvx) {
                f.uv_tiling[0] = uvx[0];
                f.uv_tiling[1] = uvx[1];
                f.uv_angle = uvx[2];
            }
            if (has_mx) {
                const std::array<float, 16>& mx = mesh.draw_maskx[d];
                for (int k = 0; k < 4; ++k) {
                    f.mask_x[k] = mx[std::size_t(k)];
                    f.mask2_x[k] = mx[std::size_t(4 + k)];
                    f.mask_o[k] = mx[std::size_t(12 + k)];
                }
                f.mask2_lv[0] = mx[8];
                f.mask2_lv[1] = mx[9];
                f.mask2_lv[2] = mx[10];
                f.mask2_lv[3] = mx[11];
            }
            if (has_eu) { f.emis_uv[0] = euvo[0]; f.emis_uv[1] = euvo[1]; }
            if (has_fb) {
                f.mask2_lv[2] = fb_code;
                f.mask2_lv[3] = std::max(fb_frame, 0.0f);
            }
            if (has_fr)
                for (int k = 0; k < 4; ++k) {
                    f.fres_d[k] = mesh.draw_fres[d][std::size_t(k)];
                    f.fres_m[k] = mesh.draw_fres[d][std::size_t(4 + k)];
                    f.fres_m2[k] = mesh.draw_fres[d][std::size_t(8 + k)];
                    f.fres_s[k] = mesh.draw_fres[d][std::size_t(12 + k)];
                    f.fres_e[k] = mesh.draw_fres[d][std::size_t(16 + k)];
                    f.fres_em[k] = mesh.draw_fres[d][std::size_t(20 + k)];
                }
            if (has_lv) {
                f.ext[0] = mesh.draw_masklv[d][0];
                f.ext[1] = mesh.draw_masklv[d][1];
            }
            static const bool kTeamClampOld = std::getenv("SC2_EMIS_TEAM_CLAMP_AFTER_MULT") != nullptr;
            if ((uvsrc & 64) != 0 && ext_free && (inst.team_emissive[3] >= 0.0f || !kTeamClampOld)) {
                const int tmi = mesh.draws[d].material;
                const float em = (!kTeamClampOld && tmi >= 0 && std::size_t(tmi) < inst.model->materials.size())
                                     ? inst.model->materials[std::size_t(tmi)].emis_mult : 1.0f;
                const bool role = inst.team_emissive[3] >= 0.0f;
                const bool body = inst.team[3] > 0.0f && inst.team[3] < 1.5f;
                for (int k = 0; k < 3; ++k) f.team_emissive[k] = (role ? inst.team_emissive[k] : body ? inst.team[k] : 0.0f) * em;
                f.team_emissive[3] = 2.0f;
            }
            if (has_e2) {
                for (int k = 0; k < 4; ++k) f.emis2[k] = e2[k];
                const bool role = inst.team_emissive[3] >= 0.0f;
                const bool body = inst.team[3] > 0.0f && inst.team[3] < 1.5f;
                for (int k = 0; k < 3; ++k) f.emis2_t[k] = role ? inst.team_emissive[k] : body ? inst.team[k] : 0.0f;
                f.emis2_t[3] = 1.0f;
            }
            if ((uvsrc & 2048) != 0 && ext_free && f.team_emissive[3] < 1.5f) {
                const int fmi = mesh.draws[d].material;
                if (fmi >= 0 && std::size_t(fmi) < inst.model->materials.size())
                    for (const formats::M3Layer& L : inst.model->materials[std::size_t(fmi)].layers) {
                        std::uint32_t fm = 0;
                        float fe = 0.0f, fb = 0.0f, fs = 0.0f;
                        if (L.usage != "Emissive" || !formats::layer_fresnel(L, fm, fe, fb, fs)) continue;
                        f.team_emissive[0] = fe;
                        f.team_emissive[1] = fb;
                        f.team_emissive[2] = fs;
                        f.team_emissive[3] = fm == 2 ? 4.0f : 3.0f;
                        static const bool kOnce = std::getenv("SC2_EMIS_FRESNEL_ONCE") != nullptr;
                        if (!kOnce && inst.model->materials[std::size_t(fmi)].emis_op == 2) f.team_emissive[3] += 2.0f;
                    }
            }
            if (uvsrc) {
                f.uv_src = (((uvsrc & 4) ? 1.0f : 0.0f) + ((uvsrc & 1) ? 2.0f : 0.0f) + ((uvsrc & 2) ? 4.0f : 0.0f) + ((uvsrc & 8) ? 8.0f : 0.0f) +
                            ((uvsrc & 16) ? 16.0f : 0.0f) + float(uvsrc & 0x7e0u) + ((uvsrc & 4096) ? 2048.0f : 0.0f)) / 4096.0f;
                static const bool kUvSrcTrace = std::getenv("SC2_UVSRC_TRACE") != nullptr;
                if (kUvSrcTrace)
                    std::fprintf(stderr, "uvsrc inst %zu draw %zu bits %u kind %u tiling %.3f %.3f\n", i, d, unsigned(uvsrc), unsigned(f.kind),
                                 double(f.uv_tiling[0]), double(f.uv_tiling[1]));
                if (uvsrc & 2) {
                    static const float kPlanar = std::getenv("SC2_PLANAR_UV_SCALE") ? float(std::atof(std::getenv("SC2_PLANAR_UV_SCALE"))) : 0.09375f;
                    static const bool kPlanarFlip = std::getenv("SC2_PLANAR_V_FLIPPED") != nullptr;
                    f.uv_tiling[0] *= kPlanar;
                    f.uv_tiling[1] *= kPlanarFlip ? -kPlanar : kPlanar;
                }
            }
            if (d < mesh.draw_tri.size() && mesh.draw_tri[d][0] > 0.0f) {
                f.uv_tiling[0] = -mesh.draw_tri[d][0];
                f.uv_tiling[1] = mesh.draw_tri[d][1];
                f.uv_angle = mesh.draw_tri[d][2];
            }
            if (has_sm) f.aux = mesh.draw_specm[d];
            if (has_sm && inst.fx.kind == FxKind::None && d < mesh.draw_specl.size() && mesh.draw_specl[d][1] != 0.0f) {
                f.p0[0] = mesh.draw_specl[d][0];
                f.p0[1] = mesh.draw_specl[d][1];
                f.p0[2] = mesh.draw_specl[d][2];
                static const bool kSpecAddByte = std::getenv("SC2_SPEC_LAYER_ADD_BYTE") != nullptr;
                f.p0[3] = kSpecAddByte ? 2.0f : 1.0f;
                if (d < mesh.draw_specluma.size() && mesh.draw_specluma[d]) {
                    f.p0[0] = 1.0f;
                    f.p0[1] = 0.0f;
                    f.p0[2] = 1.0f + mesh.draw_specm[d];
                    f.p0[3] = 3.0f;
                }
            }
            if (has_env)
                for (int k = 0; k < 4; ++k) f.p1[k] = mesh.draw_env[d][std::size_t(k)];
            draw_fx[i][d] = uint32_t(fxrecs.size());
            fxrecs.push_back(fx_record_of(f));
        }
    }
    ensure_fx_capacity(uint32_t(fxrecs.size()));
    {
        void* fp = nullptr;
        const VkDeviceSize fbytes = VkDeviceSize(fxrecs.size()) * sizeof(FxRecord);
        check(vkMapMemory(dev_, fxBuf_.mem.get(), 0, fbytes, 0, &fp), "vkMapMemory(fx)");
        std::memcpy(fp, fxrecs.data(), size_t(fbytes));
        vkUnmapMemory(dev_, fxBuf_.mem.get());
    }
    void* gp = nullptr;
    check(vkMapMemory(dev_, globalsBuf_.mem.get(), 0, sizeof(GlobalsUBO), 0, &gp), "vkMapMemory(globals)");
    std::memcpy(gp, &globals, sizeof(GlobalsUBO));
    vkUnmapMemory(dev_, globalsBuf_.mem.get());

    size_t bidx = 0;
    vkResetDescriptorPool(dev_, bonePool_.get(), 0);
    for (auto& p : bonePoolsExtra_) vkResetDescriptorPool(dev_, p.get(), 0);
    std::vector<VkDescriptorSet> bone_set(live.size(), VK_NULL_HANDLE);
    auto alloc_bone_set = [&](const std::vector<float>& flat) -> VkDescriptorSet {
        const VkDeviceSize need = VkDeviceSize(flat.size()) * 4;
        if (bidx >= bonebufs_.size()) {
            bonebufs_.push_back(make_buffer(pd_, dev_, need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            const_cast<float*>(flat.data())));
            bone_cap_.push_back(need == 0 ? 4 : need);
        } else if (need > bone_cap_[bidx]) {
            bonebufs_[bidx] = make_buffer(pd_, dev_, need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          const_cast<float*>(flat.data()));
            bone_cap_[bidx] = need;
        } else if (need > 0) {
            void* p = nullptr;
            check(vkMapMemory(dev_, bonebufs_[bidx].mem.get(), 0, need, 0, &p), "vkMapMemory(bone)");
            std::memcpy(p, flat.data(), size_t(need));
            vkUnmapMemory(dev_, bonebufs_[bidx].mem.get());
        }
        Buffer& slot = bonebufs_[bidx];
        ++bidx;
        VkDescriptorSet set = alloc_set(bonePool_, bonePoolsExtra_, boneLayout_.get(),
                                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, 4096, "bone");
        VkDescriptorBufferInfo bdi{slot.buf.get(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet bw{};
        bw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        bw.dstSet = set;
        bw.dstBinding = 0;
        bw.descriptorCount = 1;
        bw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bw.pBufferInfo = &bdi;
        vkUpdateDescriptorSets(dev_, 1, &bw, 0, nullptr);
        return set;
    };
    size_t static_bones = 1;
    for (const SceneInstance* p : live)
        if (!(p->seq >= 0 && !p->model->bones.empty()))
            static_bones = std::max(static_bones, std::max<size_t>(1, p->model->bones.size()));
    std::vector<float> id_flat(static_bones * 16, 0.0f);
    for (size_t b = 0; b < static_bones; ++b) {
        const Mat4 I = Mat4::identity();
        for (int k = 0; k < 16; ++k) id_flat[b * 16 + k] = I.m[k];
    }
    const VkDescriptorSet shared_static = alloc_bone_set(id_flat);
    const auto t_bone0 = std::chrono::steady_clock::now();
    std::map<PoseKey, VkDescriptorSet> pose_cache;
    std::size_t pose_hits = 0;
    std::size_t posed = 0;
    for (size_t i = 0; i < live.size(); ++i) {
        const SceneInstance& inst = *live[i];
        if (!(inst.seq >= 0 && !inst.model->bones.empty())) { bone_set[i] = shared_static; continue; }
        ++posed;
        const bool blending = inst.blend_weight > 0.0f && inst.blend_seq >= 0 && inst.blend_seq != inst.seq;
        static const bool kBillboards = std::getenv("SC2_NO_BILLBOARDS") == nullptr;
        const bool billboarded = kBillboards && !inst.model->billboards.empty();
        static const bool kGlBones = std::getenv("SC2_NO_GLOBAL_LAYER_BONES") == nullptr;
        const ClipLayers* glb = kGlBones && !inst.global_layers.empty() ? &inst.global_layers : nullptr;
        const bool cacheable_inst = inst.beam_bone < 0 && !blending && !billboarded && !glb;
        PoseKey pkey{};
        if (cacheable_inst) {
            pkey = PoseKey{static_cast<const void*>(inst.model), inst.seq,
                           float_bits(inst.time_ms), inst.turret_bone, float_bits(inst.turret_yaw)};
            auto hit = pose_cache.find(pkey);
            if (hit != pose_cache.end()) { bone_set[i] = hit->second; ++pose_hits; continue; }
        }
        std::vector<Mat4> bone_mats = pose(*inst.model, uint32_t(inst.seq), inst.time_ms, inst.turret_bone, inst.turret_yaw, glb);
        if (bone_mats.empty()) bone_mats.push_back(Mat4::identity());
        if (inst.beam_bone >= 0 && size_t(inst.beam_bone) < bone_mats.size() &&
            inst.model->bones.size() == bone_mats.size()) {
            const std::vector<Mat4> bwm = pose_world(*inst.model, uint32_t(inst.seq), inst.time_ms);
            if (size_t(inst.beam_bone) < bwm.size()) {
                const Mat4& b0 = bwm[size_t(inst.beam_bone)];
                const Vec3 a = mul_point(inst.world, Vec3{b0.m[12], b0.m[13], b0.m[14]});
                const Vec3 dw{inst.beam_target[0] - a.x, inst.beam_target[1] - a.y, inst.beam_target[2] - a.z};
                float dm[3];
                for (int c = 0; c < 3; ++c) {
                    const float* col = &inst.world.m[size_t(c) * 4];
                    const float l2 = col[0] * col[0] + col[1] * col[1] + col[2] * col[2];
                    dm[c] = l2 > 1e-12f ? (col[0] * dw.x + col[1] * dw.y + col[2] * dw.z) / l2 : 0.0f;
                }
                const Mat4 shift = translation(dm[0], dm[1], dm[2]);
                for (size_t b = 0; b < bone_mats.size(); ++b) {
                    int p = int(b);
                    for (size_t guard = 0; p >= 0 && guard <= bone_mats.size(); ++guard) {
                        if (p == inst.beam_bone) { bone_mats[b] = shift * bone_mats[b]; break; }
                        p = inst.model->bones[size_t(p)].parent;
                    }
                }
            }
        }
        if (billboarded && inst.model->bones.size() == bone_mats.size()) {
            const std::vector<Mat4> bwm = pose_world(*inst.model, uint32_t(inst.seq), inst.time_ms, inst.turret_bone, inst.turret_yaw, glb);
            const Vec3 fwd = normalize(sub(cam.center, cam.eye));
            const Vec3 right = normalize(cross(fwd, cam.up));
            const Vec3 upv = cross(right, fwd);
            for (const formats::Model::Billboard& bb : inst.model->billboards) {
                if ((bb.type & 0xffu) != 6u || size_t(bb.bone) >= bwm.size()) continue;
                const Mat4& wb = bwm[size_t(bb.bone)];
                const Mat4 ww = inst.world * wb;
                float s[3];
                for (int c = 0; c < 3; ++c) {
                    const float* col = &ww.m[size_t(c) * 4];
                    s[c] = std::sqrt(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]);
                }
                Mat4 target = Mat4::identity();
                const Vec3 axes[3] = {right, fwd, upv};
                for (int c = 0; c < 3; ++c) {
                    target.m[size_t(c) * 4 + 0] = axes[c].x * s[c];
                    target.m[size_t(c) * 4 + 1] = axes[c].y * s[c];
                    target.m[size_t(c) * 4 + 2] = axes[c].z * s[c];
                }
                Mat4 lin = inst.world;
                lin.m[12] = lin.m[13] = lin.m[14] = 0.0f;
                Mat4 lin_inv, wb_inv;
                if (!invert4(lin.m.data(), lin_inv.m.data()) || !invert4(wb.m.data(), wb_inv.m.data())) continue;
                Mat4 turned = lin_inv * target;
                turned.m[12] = wb.m[12];
                turned.m[13] = wb.m[13];
                turned.m[14] = wb.m[14];
                const Mat4 delta = turned * wb_inv;
                for (size_t b = 0; b < bone_mats.size(); ++b) {
                    int p = int(b);
                    for (size_t guard = 0; p >= 0 && guard <= bone_mats.size(); ++guard) {
                        if (p == int(bb.bone)) { bone_mats[b] = delta * bone_mats[b]; break; }
                        p = inst.model->bones[size_t(p)].parent;
                    }
                }
            }
        }
        if (inst.blend_weight > 0.0f && inst.blend_seq >= 0 && inst.blend_seq != inst.seq) {
            const std::vector<Mat4> prev = pose(*inst.model, uint32_t(inst.blend_seq), inst.blend_time_ms, inst.turret_bone, inst.turret_yaw);
            const float w = std::min(1.0f, std::max(0.0f, inst.blend_weight));
            for (size_t b = 0; b < bone_mats.size() && b < prev.size(); ++b)
                for (int k = 0; k < 16; ++k)
                    bone_mats[b].m[k] = bone_mats[b].m[k] * (1.0f - w) + prev[b].m[k] * w;
        }
        std::vector<float> flat;
        flat.reserve(bone_mats.size() * 16);
        for (const Mat4& mm : bone_mats)
            for (int k = 0; k < 16; ++k) flat.push_back(mm.m[k]);
        bone_set[i] = alloc_bone_set(flat);
        if (cacheable_inst) pose_cache.emplace(pkey, bone_set[i]);
    }
    frame_prof_add("rec.bone_pose", std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(std::chrono::steady_clock::now() - t_bone0).count());
    frame_prof_count("bone.posed_instances", double(posed));
    frame_prof_count("bone.pose_hits", double(pose_hits));
    frame_prof_count("bone.distinct_poses", double(pose_cache.size()));
    frame_prof_count("bone.live_instances", double(live.size()));
    frame_prof_count("lit.drawn", double(lit_drawn_));
    frame_prof_count("lit.culled", double(lit_culled_));

    const bool draw_particles = !pverts.empty() && !pgroups.empty();
    std::vector<VkDescriptorSet> part_set;
    if (draw_particles) {
        partbuf_ = make_buffer(pd_, dev_, sizeof(ParticleVertex) * pverts.size(),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                               const_cast<ParticleVertex*>(pverts.data()));
        vkResetDescriptorPool(dev_, partPool_.get(), 0);
        for (auto& p : partPoolsExtra_) vkResetDescriptorPool(dev_, p.get(), 0);
        part_set.assign(pgroups.size(), VK_NULL_HANDLE);
        for (size_t g = 0; g < pgroups.size(); ++g) {
            part_set[g] = alloc_set(partPool_, partPoolsExtra_, partLayout_.get(),
                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5, 256, "particle");
            int ti = pgroups[g].tex;
            if (ti < 0 || ti >= int(textures_.size())) ti = kDefaultWhite;
            int mi = pgroups[g].mask_tex;
            if (mi < 0 || mi >= int(textures_.size())) mi = kDefaultWhite;
            int m2 = pgroups[g].mask2_tex;
            if (m2 < 0 || m2 >= int(textures_.size())) m2 = kDefaultWhite;
            int et = pgroups[g].emis_tex;
            if (et < 0 || et >= int(textures_.size())) et = kDefaultEmissive;
            VkDescriptorImageInfo dii[5] = {{sampler_.get(), textures_[ti].view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                            {sampler_.get(), textures_[mi].view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                            {sampler_.get(), textures_[m2].view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                            {sampler_.get(), depthCopy_.view.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                                            {sampler_.get(), textures_[et].view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkWriteDescriptorSet w[5]{};
            for (uint32_t b = 0; b < 5; ++b) {
                w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[b].dstSet = part_set[g];
                w[b].dstBinding = b;
                w[b].descriptorCount = 1;
                w[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[b].pImageInfo = &dii[b];
            }
            vkUpdateDescriptorSets(dev_, 5, w, 0, nullptr);
        }
    }
    static const bool kSoftParticles = std::getenv("SC2_NO_SOFT_PARTICLES") == nullptr;
    bool need_depth = water_depth;
    if (draw_particles && kSoftParticles)
        for (const ParticleGroup& pg : pgroups) need_depth = need_depth || pg.depth_blend > 0.0f;

    VkClearValue sclear;
    sclear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo srbi{};
    srbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    srbi.renderPass = shadowPass_.get();
    srbi.framebuffer = shadowFb_.get();
    srbi.renderArea = {{0, 0}, {tun_.shadow_dim, tun_.shadow_dim}};
    srbi.clearValueCount = 1;
    srbi.pClearValues = &sclear;
    {
        SC2_PROF_SCOPE("rec.shadow_pass");
    vkCmdBeginRenderPass(cmd, &srbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 2, 1, &global_set_, 0, nullptr);
    VkPipeline bound_shadow = shadowPipeline_.get();
    for (size_t i = 0; i < live.size(); ++i) {
        const SceneInstance& inst = *live[i];
        if (inst.model->vertex_count == 0 && !inst.model->has_splat_payload()) continue;
        if (inst.team[3] >= 1.5f || inst.fx.kind == FxKind::Cloak || inst.fx.kind == FxKind::Water || inst.fx.kind == FxKind::Creep) continue;
        std::size_t caster_first = 0, caster_end = std::size_t(-1);
        if (tun_.gamma_space && tun_.rig.present && inst.model->has_splat_payload() && !std::getenv("SC2_GROUND_CASTS")) {
            if (inst.model->caster_region_end <= inst.model->caster_region_first || std::getenv("SC2_NO_GROUND_DOODAD_SHADOW")) continue;
            caster_first = inst.model->caster_region_first;
            caster_end = inst.model->caster_region_end;
        }
        Mesh& mesh = ensure_mesh(*inst.model, inst.mesh_version);
        const bool cutout = inst.team[3] <= -1.5f;
        bool any_masked = false;
        for (const auto& dr : mesh.draws) {
            const int m = dr.material;
            if (m >= 0 && std::size_t(m) < inst.model->materials.size() && !inst.model->vertex_alpha_coverage &&
                formats::material_is_alpha_blended(inst.model->materials[m])) {
                any_masked = true;
                break;
            }
        }
        if (inst.model->vertex_alpha_coverage) any_masked = true;
        const VkPipeline want_shadow =
            (cutout || any_masked) ? shadowCutoutPipeline_.get() : shadowPipeline_.get();
        if (want_shadow != bound_shadow) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want_shadow);
            bound_shadow = want_shadow;
        }
        float pc[20];
        std::memcpy(pc, inst.world.m.data(), 64);
        pc[16] = inst.team[0]; pc[17] = inst.team[1];
        pc[18] = inst.team[2]; pc[19] = inst.team[3];
        vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80, pc);
        VkDeviceSize off = 0;
        VkBuffer vb = mesh.vbuf.buf.get();
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
        vkCmdBindIndexBuffer(cmd, mesh.ibuf.buf.get(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 1, 1, &bone_set[i], 0, nullptr);
        for (size_t d = 0; d < mesh.draws.size(); ++d) {
            if (d < caster_first || d >= caster_end) continue;
            float maskpc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            const int dmi = mesh.draws[d].material;
            const bool masked =
                dmi >= 0 && std::size_t(dmi) < inst.model->materials.size() && !inst.model->vertex_alpha_coverage &&
                formats::material_is_alpha_blended(inst.model->materials[dmi]) &&
                formats::material_alpha_mask_channel(inst.model->materials[dmi]) > 0;
            if (masked) {
                maskpc[0] = 1.0f;
                maskpc[1] = float(formats::material_alpha_mask_channel(inst.model->materials[dmi]));
            }
            if (dmi >= 0 && std::size_t(dmi) < inst.model->materials.size()) {
                const formats::M3Material& sm = inst.model->materials[dmi];
                bool has_mask = false;
                for (const formats::M3Layer& ly : sm.layers) has_mask = has_mask || ly.usage == "AlphaMask";
                bool diffuse_rgb = false;
                static const bool kRgbCutout = std::getenv("SC2_RGB_CUTOUT") != nullptr;
                if (!kRgbCutout)
                    for (const formats::M3Layer& ly : sm.layers)
                        if (ly.usage == "Diffuse" && !ly.raw_record.empty() && ly.word44 == 0) diffuse_rgb = true;
                maskpc[2] = (sm.cutout_thresh > 0 && !(diffuse_rgb && !has_mask)) ? cutout_threshold(sm) : (has_mask ? 0.5f : 0.0f);
                if (const int cm = cutout_mask_channel(sm); cm > 0 && !masked) {
                    maskpc[0] = 2.0f;
                    maskpc[1] = float(cm);
                }
            }
            if (cutout || any_masked)
                vkCmdPushConstants(cmd, layout_.get(),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 80, 16,
                                   maskpc);
            if (cutout || any_masked)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &mesh.draw_set[d], 0, nullptr);
            const int mi = mesh.draws[d].material;
            if (mi >= 0 && std::size_t(mi) < inst.model->materials.size() &&
                formats::material_is_additive(inst.model->materials[mi]))
                continue;
            if (!region_visible(*inst.model, inst.seq, inst.time_ms, d, glayers(inst))) continue;
            if (material_unshadowed(*inst.model, mi)) continue;
            static const bool kDisplaceCasts = std::getenv("SC2_DISPLACE_CASTS") != nullptr;
            if (!kDisplaceCasts && mi >= 0 && std::size_t(mi) < inst.model->materials.size() && inst.model->materials[std::size_t(mi)].displace > 0.0f) continue;
            static const bool kNoCast = std::getenv("SC2_SHADOW_IGNORE_NOCAST") == nullptr;
            if (kNoCast && !inst.model->vertex_alpha_coverage && !inst.model->cliff_skin_opaque && mi >= 0 && std::size_t(mi) < inst.model->materials.size() && (inst.model->materials[mi].flags & 0x20u) != 0)
                continue;
            vkCmdDrawIndexed(cmd, mesh.draws[d].index_count, 1, mesh.draws[d].first_index, 0, 0);
        }
    }
    vkCmdEndRenderPass(cmd);
    }

    VkClearValue clears[2];
    clears[0].color = {{tun_.clear_color[0], tun_.clear_color[1], tun_.clear_color[2], cam.transparent_clear ? 0.0f : 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = (need_copy || ssao_on || need_depth) ? litPassA_.get() : litPass_.get();
    rbi.framebuffer = litFb_.get();
    rbi.renderArea = {{0, 0}, {width_, height_}};
    rbi.clearValueCount = 2;
    rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    {
        VkViewport dvp{0, 0, float(width_), float(height_), 0, 1};
        VkRect2D dsc{{0, 0}, {width_, height_}};
        vkCmdSetViewport(cmd, 0, 1, &dvp);
        vkCmdSetScissor(cmd, 0, 1, &dsc);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, litPipeline_.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 2, 1, &global_set_, 0, nullptr);
    std::vector<size_t> splat_draws;
    struct BlendedDraw {
        size_t inst;
        size_t draw;
        float dist2;
    };
    std::vector<BlendedDraw> blended;
    auto outside_frustum = [&](size_t li) -> bool {
        if (li >= inst_aabb_.size()) return false;
        const Vec3 mn = inst_aabb_[li][0], mx = inst_aabb_[li][1];
        int out[6] = {0, 0, 0, 0, 0, 0};
        for (int c = 0; c < 8; ++c) {
            const Vec3 p{(c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y, (c & 4) ? mx.z : mn.z};
            const float* m = view_proj.m.data();
            const float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
            const float cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
            const float cz = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
            const float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
            if (cx < -cw) ++out[0];
            if (cx > cw) ++out[1];
            if (cy < -cw) ++out[2];
            if (cy > cw) ++out[3];
            if (cz < 0.0f) ++out[4];
            if (cz > cw) ++out[5];
        }
        for (int k = 0; k < 6; ++k)
            if (out[k] == 8) return true;
        return false;
    };
    lit_drawn_ = lit_culled_ = 0;
    for (size_t i = 0; i < live.size(); ++i) {
        const SceneInstance& inst = *live[i];
        if (outside_frustum(i)) { ++lit_culled_; continue; }
        ++lit_drawn_;
        if (inst.model->vertex_count == 0 && !inst.model->has_splat_payload()) continue;
        if (inst.model->has_splat_payload() && ensure_splat(*inst.model)) {
            splat_draws.push_back(i);
            continue;
        }
        Mesh& mesh = ensure_mesh(*inst.model, inst.mesh_version);
        float pc[20];
        std::memcpy(pc, inst.world.m.data(), 64);
        pc[16] = inst.team[0]; pc[17] = inst.team[1];
        pc[18] = inst.team[2]; pc[19] = inst.team[3];
        vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80, pc);
        VkDeviceSize off = 0;
        VkBuffer vb = mesh.vbuf.buf.get();
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
        vkCmdBindIndexBuffer(cmd, mesh.ibuf.buf.get(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 1, 1, &bone_set[i], 0, nullptr);
        for (size_t d = 0; d < mesh.draws.size(); ++d) {
            const int mi = mesh.draws[d].material;
            if (material_hidden(*inst.model, mi)) continue;
            if (!region_visible(*inst.model, inst.seq, inst.time_ms, d, glayers(inst))) continue;
            if (const char* dt = std::getenv("SC2_DRAW_TRACE"); dt && dt[0] == '*' && mi >= 0 && std::size_t(mi) < inst.model->materials.size()) {
                static std::set<std::string> names;
                if (names.insert(inst.model->materials[mi].name + "#" + std::to_string(inst.model->vertex_count)).second) {
                    std::fprintf(stderr, "[drawtrace] material '%s' (model verts %u, team.a %.2f, flags 0x%08x, blend %u, spec %.1f)\n", inst.model->materials[mi].name.c_str(), inst.model->vertex_count, double(inst.team[3]),
                                 inst.model->materials[mi].flags, unsigned(inst.model->materials[mi].blend_mode), double(inst.model->materials[mi].specularity));
                    if (dt[1] == '+')
                        for (const formats::M3Layer& L : inst.model->materials[mi].layers)
                            if (!L.image_path.empty()) std::fprintf(stderr, "[drawtrace]   %s %s (uv source %u, word36 0x%x, v%u; uv offset %.3f %.3f flags %u id %08x, tiling %.2f %.2f)\n", L.usage.c_str(), L.image_path.c_str(),
                                                                    unsigned(L.word40), unsigned(L.word36), unsigned(L.layr_version), double(L.anim.uv_offset[0]), double(L.anim.uv_offset[1]),
                                                                    unsigned(L.anim.uv_offset_flags), unsigned(L.anim.uv_offset_id), double(L.anim.uv_tiling[0]), double(L.anim.uv_tiling[1]));
                }
            }
            if (const char* dt = std::getenv("SC2_DRAW_TRACE"); dt && mi >= 0 && std::size_t(mi) < inst.model->materials.size() &&
                inst.model->materials[mi].name.find(dt) != std::string::npos) {
                static int seen = 0;
                if (seen++ < 4)
                    std::fprintf(stderr, "[drawtrace] '%s' reaches the opaque loop: blend %u alpha-blended %d additive %d cliff-skin %d opacity %.2f team.a %.2f\n",
                                 inst.model->materials[mi].name.c_str(), unsigned(inst.model->materials[mi].blend_mode),
                                 int(formats::material_is_alpha_blended(inst.model->materials[mi])), int(formats::material_is_additive(inst.model->materials[mi])),
                                 int(inst.model->cliff_skin_opaque), double(inst.opacity), double(inst.team[3]));
            }
            float mpc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (mi >= 0 && std::size_t(mi) < inst.model->materials.size()) {
                mpc[0] = inst.model->materials[mi].specularity;
                static const bool kTwoSidedO = std::getenv("SC2_NO_TWO_SIDED") == nullptr;
                if (kTwoSidedO && (inst.model->materials[mi].flags & 0x8u) != 0 && !inst.model->vertex_alpha_coverage) mpc[0] += 100000.0f;
                static const bool kOpaqueCull = std::getenv("SC2_OPAQUE_ALL_FACES") == nullptr;
                if (kOpaqueCull && (inst.model->materials[mi].flags & 0x8u) == 0 && inst.model->materials[mi].blend_mode == formats::kBlendOpaque &&
                    !inst.model->vertex_alpha_coverage)
                    mpc[0] += 300000.0f;
                for (const formats::M3Layer& aol : inst.model->materials[mi].layers)
                    if (aol.usage == "AmbientOcclusion" && aol.word40 == 1) mpc[2] = 0.25f;
                if (const char* au = std::getenv("SC2_AO_UV")) {
                    const std::string m = au;
                    mpc[2] = m == "uv1" ? 0.25f : m == "uv1v" ? 0.5f : m == "uv0v" ? 0.75f : 0.0f;
                }
                {
                    const formats::M3Material& mm = inst.model->materials[mi];
                    std::string diff, spec;
                    bool mask = false;
                    for (const formats::M3Layer& ly : mm.layers) {
                        if (ly.usage == "Diffuse") diff = ly.image_path;
                        else if (ly.usage == "Specular") spec = ly.image_path;
                        else if (ly.usage == "AlphaMask") mask = true;
                    }
                    bool diffuse_rgb = false;
                    static const bool kRgbCutout = std::getenv("SC2_RGB_CUTOUT") != nullptr;
                    if (!kRgbCutout)
                        for (const formats::M3Layer& ly : mm.layers)
                            if (ly.usage == "Diffuse" && !ly.raw_record.empty() && ly.word44 == 0) diffuse_rgb = true;
                    if (mm.cutout_thresh > 0 && !(diffuse_rgb && !mask)) mpc[1] = cutout_threshold(mm) * 0.5f;
                    else if (!mask) mpc[1] = -1.0f;
                    if (mpc[1] > 0.0f) mpc[2] += float(cutout_mask_channel(mm));
                    static const bool kTeamAlways = std::getenv("SC2_TEAM_ALPHA_ALWAYS") != nullptr;
                    if (!kTeamAlways)
                        for (const formats::M3Layer& ly : mm.layers)
                            if (ly.usage == "Diffuse") {
                                if (!ly.raw_record.empty() && ly.word44 == 0) mpc[3] = -1.0f;
                                break;
                            }
                }
            }
            const bool refractive = inst.fx.kind == FxKind::Cloak || inst.fx.kind == FxKind::Water || inst.fx.kind == FxKind::Creep;
            static const bool kShellBlend = std::getenv("SC2_NO_SHELL_BLEND") == nullptr;
            bool shell = false;
            if (kShellBlend && mi >= 0 && std::size_t(mi) < inst.model->materials.size() && !inst.model->cliff_skin_opaque &&
                inst.model->materials[mi].blend_mode == formats::kBlendAlphaBlend && !formats::material_is_alpha_blended(inst.model->materials[mi]))
                for (const formats::M3Material& om : inst.model->materials)
                    if (formats::material_is_alpha_blended(om) && formats::material_alpha_mask(om)) { shell = true; break; }
            static const bool kUnmaskedDiffuse = !(std::getenv("SC2_UNMASKED_BLEND") && std::string(std::getenv("SC2_UNMASKED_BLEND")) == "opaque");
            if (kUnmaskedDiffuse && mi >= 0 && std::size_t(mi) < inst.model->materials.size() && !inst.model->cliff_skin_opaque &&
                !inst.model->vertex_alpha_coverage && inst.team[3] != -1.0f && inst.model->materials[mi].blend_mode == formats::kBlendAlphaBlend &&
                !formats::material_is_alpha_blended(inst.model->materials[mi]))
                shell = true;
            if (inst.opacity < 1.0f || refractive || shell ||
                (mi >= 0 && std::size_t(mi) < inst.model->materials.size() &&
                 (formats::material_is_additive(inst.model->materials[mi]) ||
                  (formats::material_is_alpha_blended(inst.model->materials[mi]) && !inst.model->cliff_skin_opaque)))) {
                const float dx = inst.world.m[12] - cam.eye.x, dy = inst.world.m[13] - cam.eye.y,
                            dz = inst.world.m[14] - cam.eye.z;
                blended.push_back(BlendedDraw{i, d, dx * dx + dy * dy + dz * dz});
                continue;
            }
            vkCmdPushConstants(cmd, layout_.get(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 80, 16, mpc);
            float apc[4];
            material_anim(*inst.model, mi, inst.seq, inst.time_ms, apc, &inst.global_layers);
            apc[3] = diffuse_layer_packed(*inst.model, mi, apc[3]);
            vkCmdPushConstants(cmd, layout_.get(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 96, 16, apc);
            const float tpc[4] = {inst.tint[0], inst.tint[1], inst.tint[2], float(draw_fx[i].empty() ? fx_index[i] : draw_fx[i][d])};
            vkCmdPushConstants(cmd, layout_.get(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 112, 16, tpc);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &mesh.draw_set[d], 0, nullptr);
            vkCmdDrawIndexed(cmd, mesh.draws[d].index_count, 1, mesh.draws[d].first_index, 0, 0);
        }
    }
    for (size_t i : splat_draws) {
        const SceneInstance& inst = *live[i];
        TerrainSplat* ts = ensure_splat(*inst.model);
        Mesh& mesh = ensure_mesh(*inst.model, inst.mesh_version);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splatPipeline_.get());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splatPipeLayout_.get(), 0, 1, &ts->set, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splatPipeLayout_.get(), 1, 1, &bone_set[i], 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splatPipeLayout_.get(), 2, 1, &global_set_, 0, nullptr);
        float pc[20];
        std::memcpy(pc, inst.world.m.data(), 64);
        pc[16] = inst.team[0]; pc[17] = inst.team[1];
        pc[18] = inst.team[2]; pc[19] = inst.team[3];
        vkCmdPushConstants(cmd, splatPipeLayout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80, pc);
        VkDeviceSize off = 0;
        VkBuffer vb = mesh.vbuf.buf.get();
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
        vkCmdBindIndexBuffer(cmd, mesh.ibuf.buf.get(), 0, VK_INDEX_TYPE_UINT32);
        static const bool kDoodadDbl = std::getenv("SC2_DOODAD_DBL_LAMBERT") != nullptr;
        const std::size_t df = inst.model->caster_region_first, de = inst.model->caster_region_end;
        float flag_now = -1.0f;
        for (size_t d = 0; d < mesh.draws.size(); ++d) {
            const float want = (kDoodadDbl && de > df && d >= df && d < de) ? 1.0f : 0.0f;
            if (want != flag_now) {
                pc[16] = want;
                vkCmdPushConstants(cmd, splatPipeLayout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80, pc);
                flag_now = want;
            }
            vkCmdDrawIndexed(cmd, mesh.draws[d].index_count, 1, mesh.draws[d].first_index, 0, 0);
        }
    }
    if (need_copy || ssao_on || need_depth) {
        vkCmdEndRenderPass(cmd);
        if (need_depth) {
            VkImageMemoryBarrier zb[2]{};
            zb[0].sType = zb[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            zb[0].srcQueueFamilyIndex = zb[0].dstQueueFamilyIndex = zb[1].srcQueueFamilyIndex = zb[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            zb[0].subresourceRange = zb[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            zb[0].image = depth_.img.get();
            zb[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            zb[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            zb[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            zb[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            zb[1].image = depthCopy_.img.get();
            zb[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            zb[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            zb[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            zb[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 2, zb);
            VkImageCopy zc{};
            zc.srcSubresource = zc.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            zc.extent = {width_, height_, 1};
            vkCmdCopyImage(cmd, depth_.img.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depthCopy_.img.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &zc);
            zb[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            zb[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            zb[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            zb[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            zb[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            zb[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            zb[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            zb[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 2, zb);
        }
        if (ssao_on) {
            VkImageMemoryBarrier ab[2]{};
            ab[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            ab[0].image = hdrColor_.img.get();
            ab[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ab[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            ab[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ab[0].srcQueueFamilyIndex = ab[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ab[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            ab[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            ab[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            ab[1].image = depth_.img.get();
            ab[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            ab[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            ab[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            ab[1].srcQueueFamilyIndex = ab[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ab[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            ab[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, ab);
            VkRenderPassBeginInfo arb{};
            arb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            arb.renderPass = aoPass_.get();
            arb.framebuffer = aoFb_.get();
            arb.renderArea = {{0, 0}, {width_, height_}};
            vkCmdBeginRenderPass(cmd, &arb, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport avp{0, 0, float(width_), float(height_), 0, 1};
            VkRect2D asc{{0, 0}, {width_, height_}};
            vkCmdSetViewport(cmd, 0, 1, &avp);
            vkCmdSetScissor(cmd, 0, 1, &asc);
            const float th = std::tan(cam.fovy * 0.5f);
            static const float kAoSamples = std::getenv("SC2_SSAO_SAMPLES") ? float(std::atof(std::getenv("SC2_SSAO_SAMPLES"))) : 16.0f;
            static const float kAoRadiusScale = std::getenv("SC2_SSAO_RADIUS") ? float(std::atof(std::getenv("SC2_SSAO_RADIUS"))) : 1.0f;
            static const float kAoUnit = std::getenv("SC2_SSAO_UNIT") ? float(std::atof(std::getenv("SC2_SSAO_UNIT"))) : 1.0f;
            static const float kAoLimiter = std::getenv("SC2_SSAO_LIMITER") ? float(std::atof(std::getenv("SC2_SSAO_LIMITER"))) : 0.0f;
            static const bool kAoDetail = std::getenv("SC2_SSAO_NO_DETAIL") == nullptr;
            float apc[16] = {th * float(width_) / float(height_), th, cam.znear, cam.zfar,
                             tun_.ao_radius * 0.15f * kAoRadiusScale * kAoUnit, tun_.ao_power, tun_.ao_no_occlusion * kAoUnit, 0.0f,
                             0.0f, kAoSamples, 0.01f * kAoUnit, kAoLimiter,
                             kAoDetail ? tun_.ao_detail_radius * 0.15f * kAoRadiusScale * kAoUnit : 0.0f, tun_.ao_detail_power,
                             tun_.ao_detail_no_occlusion * kAoUnit, 0.0f};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tmPipeLayout_.get(), 0, 1, &tm_set_, 0, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, aoPipelineA_.get());
            vkCmdPushConstants(cmd, tmPipeLayout_.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(apc), apc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            apc[8] = 1.0f;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, aoPipelineB_.get());
            vkCmdPushConstants(cmd, tmPipeLayout_.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(apc), apc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
            VkImageMemoryBarrier db2 = ab[1];
            db2.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            db2.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            db2.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            db2.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1, &db2);
        }
        if (need_copy) {
        VkImageMemoryBarrier cbv{};
        cbv.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        cbv.image = sceneCopy_.img.get();
        cbv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cbv.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cbv.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        cbv.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        cbv.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &cbv);
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = {int32_t(width_), int32_t(height_), 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.dstOffsets[1] = {int32_t(copy_w_), int32_t(copy_h_), 1};
        vkCmdBlitImage(cmd, hdrColor_.img.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sceneCopy_.img.get(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        cbv.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        cbv.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cbv.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        cbv.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &cbv);
        }
        VkImageMemoryBarrier hb{};
        hb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        hb.image = hdrColor_.img.get();
        hb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        hb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        hb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        hb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        hb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &hb);
        VkRenderPassBeginInfo rb2{};
        rb2.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rb2.renderPass = litPassB_.get();
        rb2.framebuffer = litFb_.get();
        rb2.renderArea = {{0, 0}, {width_, height_}};
        rb2.clearValueCount = 0;
        vkCmdBeginRenderPass(cmd, &rb2, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport dvp{0, 0, float(width_), float(height_), 0, 1};
        VkRect2D dsc{{0, 0}, {width_, height_}};
        vkCmdSetViewport(cmd, 0, 1, &dvp);
        vkCmdSetScissor(cmd, 0, 1, &dsc);
    }
    if (!blended.empty()) {
        auto displaces = [&](const BlendedDraw& d) {
            const SceneInstance& si = *live[d.inst];
            const Mesh& me = ensure_mesh(*si.model, si.mesh_version);
            const int dm = me.draws[d.draw].material;
            return me.any_displace && dm >= 0 && std::size_t(dm) < si.model->materials.size() && si.model->materials[std::size_t(dm)].displace > 0.0f;
        };
        std::stable_sort(blended.begin(), blended.end(), [&](const BlendedDraw& a, const BlendedDraw& b) {
            const bool da = displaces(a), db = displaces(b);
            if (da != db) return da;
            return a.dist2 > b.dist2;
        });
        VkPipeline bound = VK_NULL_HANDLE;
        for (const BlendedDraw& bd : blended) {
            const SceneInstance& inst = *live[bd.inst];
            Mesh& mesh = ensure_mesh(*inst.model, inst.mesh_version);
            if (std::getenv("SC2_NO_BLENDED") && inst.team[3] <= -1.5f) continue;
            const int mi = mesh.draws[bd.draw].material;
            if (material_hidden(*inst.model, mi)) continue;
            if (!region_visible(*inst.model, inst.seq, inst.time_ms, bd.draw, glayers(inst))) continue;
            static const formats::M3Material kNoMaterial{};
            const formats::M3Material& mat = (mi >= 0 && std::size_t(mi) < inst.model->materials.size())
                                                 ? inst.model->materials[std::size_t(mi)] : kNoMaterial;
            const bool refractive = inst.fx.kind == FxKind::Cloak || inst.fx.kind == FxKind::Water || inst.fx.kind == FxKind::Creep;
            const bool inst_alpha = (inst.opacity < 1.0f || refractive) && !formats::material_is_additive(mat);
            static const bool kMeshGlowMask = std::getenv("SC2_NO_GLOW_MASK") == nullptr;
            const bool mesh_glow = kMeshGlowMask && !cam.transparent_clear && (mat.flags & 0x10u) != 0;
            const bool overlay = mesh.draws[bd.draw].overlay && mat.blend_mode == formats::kBlendAlphaBlend && !inst_alpha;
            const VkPipeline want = mat.displace > 0.0f                           ? alphaBlendPipeline_.get()
                                    : overlay                                     ? alphaBlendEqualPipeline_.get()
                                    : inst_alpha                                  ? alphaBlendPipeline_.get()
                                    : mat.blend_mode == formats::kBlendAlphaAdd    ? (mesh_glow ? alphaAddGlowPipeline_.get() : alphaAddPipeline_.get())
                                    : mat.blend_mode == formats::kBlendAlphaBlend ? alphaBlendPipeline_.get()
                                                                                  : (mesh_glow ? addGlowPipeline_.get() : addPipeline_.get());
            if (want != bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 2, 1, &global_set_, 0, nullptr);
                bound = want;
            }
            float pc[20];
            std::memcpy(pc, inst.world.m.data(), 64);
            pc[16] = inst.team[0]; pc[17] = inst.team[1];
            pc[18] = inst.team[2]; pc[19] = inst.team[3];
            static const bool kBlendedTeam = std::getenv("SC2_BLENDED_NO_TEAM_MIX") == nullptr;
            if (kBlendedTeam && inst.team[3] > 0.0f && inst.team[3] < 1.5f && mi >= 0 && std::size_t(mi) < inst.model->materials.size())
                for (const formats::M3Layer& ly : inst.model->materials[std::size_t(mi)].layers)
                    if (ly.usage == "Diffuse") {
                        if (!ly.raw_record.empty() && ly.word44 == 1) pc[19] = 1.25f;
                        break;
                    }
            vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80, pc);
            const bool unshaded = (mat.flags & 0x10u) != 0 && !std::getenv("SC2_NO_UNSHADED");
            static const bool kAddAlpha = std::getenv("SC2_ADD_DIFFUSE_NO_ALPHA") == nullptr;
            float add_alpha = (kAddAlpha && mat.blend_mode == formats::kBlendAdd && mi >= 0) ? 0.25f : 0.0f;
            static const bool kMaskCover = std::getenv("SC2_MASK_DIFFUSE_TEST") == nullptr;
            if (kMaskCover && mat.blend_mode == formats::kBlendAlphaBlend && formats::material_alpha_mask_channel(mat) == 1) {
                const formats::M3Layer* ml = formats::material_alpha_mask(mat);
                for (const formats::M3Layer& dl : mat.layers)
                    if (dl.usage == "Diffuse" && ml && !ml->image_path.empty() && dl.image_path == ml->image_path) add_alpha = 0.25f;
            }
            static const bool kUvSrcCover = std::getenv("SC2_UVSRC_DIFFUSE_TEST") == nullptr && std::getenv("SC2_NO_UV_SOURCE") == nullptr;
            if (kUvSrcCover && mat.blend_mode == formats::kBlendAlphaBlend && formats::material_alpha_mask_channel(mat) != 0) {
                bool apart = false;
                for (const formats::M3Layer& L : mat.layers)
                    if (!L.raw_record.empty() && ((L.usage == "AlphaMask" && L.word40 == 1) || (L.usage == "Diffuse" && L.word40 == 5))) apart = true;
                if (apart) add_alpha = 0.25f;
            }
            static const bool kConstMaskCover = std::getenv("SC2_CONST_MASK_DIFFUSE_TEST") == nullptr;
            if (kConstMaskCover && mat.blend_mode == formats::kBlendAlphaBlend && formats::material_alpha_mask_channel(mat) != 0) {
                const formats::M3Layer* cml = formats::material_alpha_mask(mat);
                if (cml && cml->const_colour) add_alpha = 0.25f;
            }
            static const bool kAlphaAddMasks = std::getenv("SC2_ALPHAADD_DIFFUSE_ALPHA") == nullptr;
            if (kAlphaAddMasks && mat.blend_mode == formats::kBlendAlphaAdd) add_alpha = 0.25f;
            static const bool kMaskedCover = std::getenv("SC2_MASKED_DIFFUSE_TEST") == nullptr;
            if (kMaskedCover && mat.blend_mode == formats::kBlendAlphaBlend && formats::material_alpha_mask_channel(mat) != 0) add_alpha = 0.25f;
            if (overlay && formats::material_alpha_mask_channel(mat) != 0) add_alpha = 0.25f;
            if (mat.blend_mode == formats::kBlendAlphaBlend && formats::material_alpha_mask_channel(mat) == 0 && !inst_alpha) add_alpha = 0.25f;
            float ao_uv1 = 0.0f;
            for (const formats::M3Layer& aol : mat.layers)
                if (aol.usage == "AmbientOcclusion" && aol.word40 == 1) ao_uv1 = 0.25f;
            if (const char* au = std::getenv("SC2_AO_UV")) {
                const std::string m = au;
                ao_uv1 = m == "uv1" ? 0.25f : m == "uv1v" ? 0.5f : m == "uv0v" ? 0.75f : 0.0f;
            }
            int mask_code = formats::material_alpha_mask_channel(mat);
            static const bool kUnmaskedDiffuseB = !(std::getenv("SC2_UNMASKED_BLEND") && std::string(std::getenv("SC2_UNMASKED_BLEND")) == "opaque");
            if (kUnmaskedDiffuseB && mask_code == 0 && mat.blend_mode == formats::kBlendAlphaBlend && !inst_alpha) mask_code = 6;
            static const bool kConstMaskAlone = std::getenv("SC2_CONST_MASK_X_DIFFUSE_ALPHA") == nullptr;
            if (!kConstMaskAlone && mask_code == 1 && mat.blend_mode == formats::kBlendAlphaBlend) {
                const formats::M3Layer* cml7 = formats::material_alpha_mask(mat);
                if (cml7 && cml7->const_colour) mask_code = 7;
            }
            static const bool kTwoSidedB = std::getenv("SC2_NO_TWO_SIDED") == nullptr;
            float two_sided = (kTwoSidedB && mi >= 0 && (mat.flags & 0x8u) != 0 && !inst.model->vertex_alpha_coverage) ? 100000.0f : 0.0f;
            static const bool kBlendCull = std::getenv("SC2_BLEND_ALL_FACES") == nullptr;
            if (kBlendCull && mi >= 0 && !unshaded && (mat.flags & 0x8u) == 0 && mat.blend_mode != formats::kBlendOpaque && !inst.model->vertex_alpha_coverage) two_sided = 200000.0f;
            float mpc[4] = {unshaded ? -1.0f : mat.specularity + two_sided, float(mat.blend_mode) + add_alpha, float(mask_code) + ao_uv1,
                            inst.opacity < 1.0f ? inst.opacity : 0.0f};
            if (mat.displace > 0.0f) {
                float s = mat.displace;
                if (inst.seq >= 0)
                    animated_layer_field(*inst.model, std::uint32_t(inst.seq), mat.displace_interp, mat.displace_flags, mat.displace_id, inst.time_ms, 1, &mat.displace, &s, &inst.global_layers);
                static const float kDisplaceScale = std::getenv("SC2_DISPLACE_SCALE") ? float(std::atof(std::getenv("SC2_DISPLACE_SCALE"))) : 0.05f;
                mpc[1] = 100.0f + std::clamp(s, 0.0f, 50.0f);
                mpc[2] = kDisplaceScale;
                mpc[3] = (!mat.layers.empty() && mat.layers[0].word40 == 1) ? 1.0f : 0.0f;
            }
            if (const char* dt = std::getenv("SC2_DRAW_TRACE"); dt && mat.name.find(dt) != std::string::npos) {
                static int printed = 0;
                if (printed++ < 4)
                    std::fprintf(stderr, "[drawtrace] '%s' mat %.3f %.3f %.3f %.3f masklv %.3f %.3f team %.2f %.2f %.2f %.2f tex diffuse %d mask %d\n", mat.name.c_str(), double(mpc[0]),
                                 double(mpc[1]), double(mpc[2]), double(mpc[3]), double(bd.draw < mesh.draw_masklv.size() ? mesh.draw_masklv[bd.draw][0] : -99.0f),
                                 double(bd.draw < mesh.draw_masklv.size() ? mesh.draw_masklv[bd.draw][1] : -99.0f), double(pc[16]), double(pc[17]), double(pc[18]), double(pc[19]),
                                 mesh.draw_tex[bd.draw][0], mesh.draw_tex[bd.draw][4]);
            }
            vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 80, 16, mpc);
            float apc[4];
            material_anim(*inst.model, mi, inst.seq, inst.time_ms, apc, &inst.global_layers);
            apc[3] = diffuse_layer_packed(*inst.model, mi, apc[3]);
            vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 96, 16, apc);
            const float tpc[4] = {inst.tint[0], inst.tint[1], inst.tint[2],
                                  float(draw_fx[bd.inst].empty() ? fx_index[bd.inst] : draw_fx[bd.inst][bd.draw])};
            vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 112, 16, tpc);
            VkDeviceSize off = 0;
            VkBuffer vb = mesh.vbuf.buf.get();
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
            vkCmdBindIndexBuffer(cmd, mesh.ibuf.buf.get(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 1, 1, &bone_set[bd.inst], 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &mesh.draw_set[bd.draw], 0, nullptr);
            if ((mat.flags & 0x100u) != 0 && want == alphaBlendPipeline_.get() && !inst_alpha && !std::getenv("SC2_NO_DEPTH_PREPASS")) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depthPrepassPipeline_.get());
                vkCmdDrawIndexed(cmd, mesh.draws[bd.draw].index_count, 1, mesh.draws[bd.draw].first_index, 0, 0);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, alphaBlendEqualPipeline_.get());
                bound = alphaBlendEqualPipeline_.get();
            }
            vkCmdDrawIndexed(cmd, mesh.draws[bd.draw].index_count, 1, mesh.draws[bd.draw].first_index, 0, 0);
        }
    }
    if (draw_particles) {
        float pmvp[32] = {};
        pmvp[25] = cam.znear;
        pmvp[26] = cam.zfar;
        pmvp[16] = (tun_.gamma_space && tun_.rig.present) ? 1.0f : 0.0f;
        std::memcpy(pmvp, view_proj.m.data(), 64);
        vkCmdPushConstants(cmd, partPipeLayout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128, pmvp);
        VkDeviceSize poff = 0;
        VkBuffer pvb = partbuf_.buf.get();
        vkCmdBindVertexBuffers(cmd, 0, 1, &pvb, &poff);
        uint32_t bound_blend = UINT32_MAX;
        for (size_t g = 0; g < pgroups.size(); ++g) {
            static const bool kGlowMask = std::getenv("SC2_NO_GLOW_MASK") == nullptr;
            const bool glow = kGlowMask && pgroups[g].glow && !cam.transparent_clear;
            const uint32_t want_blend = pgroups[g].blend | (glow ? 0x100u : 0u);
            if (want_blend != bound_blend) {
                bound_blend = want_blend;
                const uint32_t gb = pgroups[g].blend;
                VkPipeline pipe = glow ? partGlowPipeline_.get() : partPipeline_.get();
                if (gb == formats::kBlendAdd) pipe = glow ? partGlowAddPipeline_.get() : partAddPipeline_.get();
                else if (gb == formats::kBlendAlphaBlend || gb == formats::kBlendOpaque)
                    pipe = glow ? partGlowAlphaBlendPipeline_.get() : partAlphaBlendPipeline_.get();
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                vkCmdPushConstants(cmd, partPipeLayout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128, pmvp);
            }
            pmvp[17] = pgroups[g].mask_tex >= 0 ? pgroups[g].mask_code : 0.0f;
            pmvp[18] = pgroups[g].mask_mul;
            pmvp[19] = pgroups[g].mask_add;
            pmvp[20] = pgroups[g].mask2_tex >= 0 ? pgroups[g].mask2_code : 0.0f;
            pmvp[21] = pgroups[g].mask2_mul;
            pmvp[22] = pgroups[g].mask2_add;
            pmvp[23] = std::getenv("SC2_PARTICLE_DIFFUSE_ALPHA") ? -1.0f : float(pgroups[g].blend);
            static const bool kLitProduct = std::getenv("SC2_PARTICLE_MASK_ALONE") == nullptr;
            if (kLitProduct && !pgroups[g].mask_alone && pgroups[g].blend <= formats::kBlendAlphaBlend) pmvp[23] = -1.0f;
            if (pgroups[g].blend == formats::kBlendAdd && std::getenv("SC2_PARTICLE_ADD_ALPHA")) pmvp[23] = 2.25f;
            pmvp[24] = need_depth ? pgroups[g].depth_blend : 0.0f;
            static const bool kBlendSaturates = std::getenv("SC2_PARTICLE_HDR_BLEND") == nullptr;
            pmvp[27] = (kBlendSaturates && !pgroups[g].glow && pgroups[g].blend <= formats::kBlendAlphaBlend) ? 1.0f : 0.0f;
            for (int k = 0; k < 4; ++k) pmvp[28 + k] = pgroups[g].emis[k];
            vkCmdPushConstants(cmd, partPipeLayout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128, pmvp);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, partPipeLayout_.get(), 0, 1, &part_set[g], 0, nullptr);
            vkCmdDraw(cmd, pgroups[g].count, 1, pgroups[g].first, 0);
        }
    }
    vkCmdEndRenderPass(cmd);

    float col_packed_levels = 1000.0f * 2048.0f + 1000.0f, col_packed_light = 0.0f;
    {
        const bool off = !tun_.col_levels || std::getenv("SC2_COL_LEVELS_OFF") != nullptr;
        const float low = off ? 0.0f : std::clamp(tun_.col_input_low, 0.0f, 0.9f);
        const float high = off ? 1.0f : std::clamp(tun_.col_input_high, low + 0.05f, 1.0f);
        static const bool kFilmicSquared = std::getenv("SC2_COL_CONTRAST_SQUARED") != nullptr;
        const float cexp = (tun_.hdr_operator == 4 && !kFilmicSquared) ? tun_.col_contrast : tun_.col_contrast * tun_.col_contrast;
        const float p = off ? 1.0f : std::clamp(cexp * std::pow(std::max(tun_.col_input_gamma, 0.05f), -0.2f), 0.25f, 7.9f);
        const float gain = off ? 1.0f : std::clamp(tun_.col_brightness * std::pow((1.0f - low) / (high - low), p), 0.0f, 2.047f);
        col_packed_levels = float(std::lround(p * 1000.0f) * 2048 + std::lround(gain * 1000.0f));
        static const bool kLightTowardWhite = std::getenv("SC2_COL_LIGHTNESS_TOWARD_WHITE") != nullptr;
        col_packed_light = float(std::lround((std::clamp(tun_.col_lightness, -1.0f, 1.0f) + 1.0f) * 1000.0f) + (kLightTowardWhite ? 2048 : 0) + 4096 * std::lround(low * 1000.0f));
    }
    static const float kColFlags = (std::getenv("SC2_COL_LEVELS_SCALE") ? 64.0f : 0.0f) + (std::getenv("SC2_COL_LUT_NO_CAP") ? 128.0f : 0.0f) +
                                   (std::getenv("SC2_WHITE_SHIFT") ? 0.0f : 256.0f);
    TonemapPC tm{tun_.exposure, std::getenv("SC2_TONEMAP_CURVE") ? std::atoi(std::getenv("SC2_TONEMAP_CURVE")) : tun_.curve, cam.transparent_clear ? 1 : 0, tun_.white_point,
                 tun_.col_blend, col_packed_levels, std::clamp(tun_.col_hue, -31.0f, 31.0f) + kColFlags, tun_.col_saturation, col_packed_light,
                 tun_.bloom_threshold, tun_.bloom_gain, tun_.bloom_scale, (tun_.gamma_space && tun_.rig.present) ? tun_.hdr_operator : -1, 0, {0, 0}, {}};
    {
        float inv[16];
        if (fog_present_ && !fog_in_scene() && !cam.transparent_clear && fog_world_w_ > 0.0f && fog_world_h_ > 0.0f &&
            invert4(view_proj.m.data(), inv)) {
            for (int c = 0; c < 4; ++c) {
                inv[c * 4 + 0] /= fog_world_w_;
                inv[c * 4 + 1] /= fog_world_h_;
            }
            std::memcpy(tm.fog_from_ndc, inv, sizeof(inv));
            tm.fog_on = std::getenv("SC2_FOW_VEILS_VOID") ? 2 : 1;
            tm.pad_[1] = int32_t(std::lround(cam.center.z * 256.0f)) + 1048576;
        }
        if (!tm.fog_on && cam.dof_amount > 0.0f && cam.dof_end > cam.dof_start && cam.dof_radius_px > 0.0f && !cam.orthographic) {
            tm.pad_[1] = 1;
            tm.fog_from_ndc[0] = cam.dof_focus;
            tm.fog_from_ndc[1] = cam.dof_start;
            tm.fog_from_ndc[2] = cam.dof_end;
            tm.fog_from_ndc[3] = cam.dof_amount;
            tm.fog_from_ndc[4] = cam.znear;
            tm.fog_from_ndc[5] = cam.zfar;
            tm.fog_from_ndc[6] = cam.dof_radius_px;
        }
        VkImageMemoryBarrier db{};
        db.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        db.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        db.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.image = depth_.img.get();
        db.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        db.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        db.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &db);
    }
    static const bool kBloomRings = std::getenv("SC2_BLOOM_RINGS") != nullptr;
    if (tun_.bloom_gain > 0.0f && tun_.gamma_space && tun_.rig.present && !cam.transparent_clear && !kBloomRings) {
        static const float kSigma = std::getenv("SC2_BLOOM_SIGMA") ? float(std::atof(std::getenv("SC2_BLOOM_SIGMA"))) : 4.0f;
        const float sigma = std::max(0.5f, kSigma * std::max(tun_.bloom_scale, 0.25f));
        const float radius = std::min(32.0f, std::ceil(sigma * 3.0f));
        static const float kPrescale = std::getenv("SC2_BLOOM_PRESCALE") ? float(std::atof(std::getenv("SC2_BLOOM_PRESCALE"))) : 1.0f;
        struct BloomStep {
            VkFramebuffer fb;
            VkDescriptorSet src;
            float pc[8];
        };
        const BloomStep steps[3] = {
            {bloomFbA_.get(), bloom_set_[0], {0.0f, tun_.bloom_threshold, 1.0f / float(width_), 1.0f / float(height_), 0.0f, 0.0f, kPrescale, 0.0f}},
            {bloomFbB_.get(), bloom_set_[1], {1.0f, 0.0f, 1.0f / float(bloom_w_), 0.0f, sigma, radius, 0.0f, 0.0f}},
            {bloomFbA_.get(), bloom_set_[2], {1.0f, 0.0f, 0.0f, 1.0f / float(bloom_h_), sigma, radius, 0.0f, 0.0f}},
        };
        for (const BloomStep& st : steps) {
            VkRenderPassBeginInfo brb{};
            brb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            brb.renderPass = bloomPass_.get();
            brb.framebuffer = st.fb;
            brb.renderArea = {{0, 0}, {bloom_w_, bloom_h_}};
            vkCmdBeginRenderPass(cmd, &brb, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport bvp{0, 0, float(bloom_w_), float(bloom_h_), 0, 1};
            VkRect2D bsc{{0, 0}, {bloom_w_, bloom_h_}};
            vkCmdSetViewport(cmd, 0, 1, &bvp);
            vkCmdSetScissor(cmd, 0, 1, &bsc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomPipeline_.get());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tmPipeLayout_.get(), 0, 1, &st.src, 0, nullptr);
            vkCmdPushConstants(cmd, tmPipeLayout_.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(st.pc), st.pc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
        }
        tm.pad_[0] = 1;
    }
    VkRenderPassBeginInfo tmrbi{};
    tmrbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    tmrbi.renderPass = tmPass_.get();
    tmrbi.framebuffer = tmFb_.get();
    tmrbi.renderArea = {{0, 0}, {width_, height_}};
    tmrbi.clearValueCount = 0;
    tmrbi.pClearValues = nullptr;
    vkCmdBeginRenderPass(cmd, &tmrbi, VK_SUBPASS_CONTENTS_INLINE);
    {
        VkViewport dvp{0, 0, float(width_), float(height_), 0, 1};
        VkRect2D dsc{{0, 0}, {width_, height_}};
        vkCmdSetViewport(cmd, 0, 1, &dvp);
        vkCmdSetScissor(cmd, 0, 1, &dsc);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tmPipeline_.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tmPipeLayout_.get(), 0, 1, &tm_set_, 0, nullptr);
    vkCmdPushConstants(cmd, tmPipeLayout_.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TonemapPC), &tm);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void SceneRenderer::trail_note(std::uint64_t key, float now_s, const Vec3& current) {
    std::vector<TrailSample>& h = trails_[key];
    if (!h.empty() && now_s < h.back().t) h.clear();
    if (!h.empty() && now_s - h.back().t < 1e-4f) { h.back().p = current; return; }
    h.push_back({now_s, current});
    size_t drop = 0;
    while (drop + 1 < h.size() && (now_s - h[drop].t > 6.0f || h.size() - drop > 256)) ++drop;
    if (drop) h.erase(h.begin(), h.begin() + long(drop));
    if (trails_.size() > 4096) {
        for (auto it = trails_.begin(); it != trails_.end();)
            it = (it->second.empty() || now_s - it->second.back().t > 6.0f) ? trails_.erase(it) : std::next(it);
    }
}
Vec3 SceneRenderer::trail_origin(std::uint64_t key, float now_s, float age_s, const Vec3& current) {
    auto it = trails_.find(key);
    if (it == trails_.end() || it->second.empty()) return current;
    const std::vector<TrailSample>& h = it->second;
    const float want = now_s - age_s;
    if (want <= h.front().t) return h.front().p;
    for (size_t i = h.size(); i-- > 1;)
        if (h[i - 1].t <= want) {
            const float span = h[i].t - h[i - 1].t;
            const float u = span > 1e-6f ? (want - h[i - 1].t) / span : 1.0f;
            return {h[i - 1].p.x + (h[i].p.x - h[i - 1].p.x) * u, h[i - 1].p.y + (h[i].p.y - h[i - 1].p.y) * u,
                    h[i - 1].p.z + (h[i].p.z - h[i - 1].p.z) * u};
        }
    return current;
}

void build_particles(SceneRenderer& sr, const std::vector<const SceneInstance*>& live,
                     const Camera& cam,
                     std::vector<SceneRenderer::ParticleVertex>& pverts,
                     std::vector<SceneRenderer::ParticleGroup>& pgroups) {
    if (std::getenv("SC2_NO_PARTICLES")) return;
    Vec3 fwd = normalize(sub(cam.center, cam.eye));
    Vec3 right = normalize(cross(fwd, cam.up));
    Vec3 upc = cross(right, fwd);
    float lit_shade[3] = {1.0f, 1.0f, 1.0f};
    {
        const SceneRenderer::Tunables& lt = sr.tunables();
        if (lt.rig.present) {
            const RigLight* ls[3] = {&lt.rig.key, &lt.rig.fill, &lt.rig.back};
            for (int c = 0; c < 3; ++c) lit_shade[c] = lt.rig.ambient[c];
            for (const RigLight* l : ls) {
                if (!l->present) continue;
                const float nl = std::max(0.0f, fwd.x * l->dir.x + fwd.y * l->dir.y + fwd.z * l->dir.z);
                for (int c = 0; c < 3; ++c) lit_shade[c] += l->color[c] * nl;
            }
        }
    }
    struct PQuad {
        float depth;
        SceneRenderer::ParticleVertex v[4];
    };
    using PKey = std::tuple<uint32_t, int, int, int, float, float, int, int, int, float, float, float, int>;
    std::map<PKey, std::vector<PQuad>> bytex;
    std::vector<std::array<float, 4>> pemis_table{{0.0f, 0.0f, 0.0f, 0.0f}};
    std::vector<int> pemis_tex{-1};
    uint32_t emitter_uid = 0;
    for (const SceneInstance* instp : live) {
        const SceneInstance& inst = *instp;
        const formats::Model& m = *inst.model;
        if (m.particles.empty() && m.ribbons.empty()) continue;
        if (const char* tv = std::getenv("SC2_PAR_TRACE_VERTS"); tv && std::atoi(tv) == 0) {
            static std::set<const formats::Model*> seen;
            if (seen.insert(&m).second)
                std::fprintf(stderr, "[par] model verts %u particles %zu ribbons %zu seq %d at %.1f %.1f\n", m.vertex_count, m.particles.size(), m.ribbons.size(), inst.seq, double(inst.world.m[12]), double(inst.world.m[13]));
        }
        if (const char* tn = std::getenv("SC2_PAR_TRACE_NEAR")) {
            float nx = 0.0f, ny = 0.0f, nr = 0.0f;
            static std::set<const SceneInstance*> seen_near;
            if (std::sscanf(tn, "%f,%f,%f", &nx, &ny, &nr) == 3 && std::fabs(inst.world.m[12] - nx) <= nr && std::fabs(inst.world.m[13] - ny) <= nr &&
                seen_near.size() < 400 && seen_near.insert(&inst).second) {
                std::fprintf(stderr, "[parnear] verts %u particles %zu seq %d at %.2f %.2f %.2f mat0 '%s'", m.vertex_count, m.particles.size(), inst.seq,
                             double(inst.world.m[12]), double(inst.world.m[13]), double(inst.world.m[14]), m.materials.empty() ? "-" : m.materials.front().name.c_str());
                for (const formats::M3Particle& q : m.particles)
                    std::fprintf(stderr, " | rate %.1f%s life %.2f speed %.2f size %.2f/%.2f alpha %.2f/%.2f/%.2f shape %u area %.2f %.2f %.2f r %.2f spread %.2f %.2f max %u trail %d",
                                 double(q.emission_rate), q.rate_anim_flags ? "A" : "", double(q.lifetime), double(q.emission_speed), double(q.size_start), double(q.size_end),
                                 double(q.color[0][3]), double(q.color[1][3]), double(q.color[2][3]), unsigned(q.shape), double(q.area_size[0]), double(q.area_size[1]),
                                 double(q.area_size[2]), double(q.area_radius), double(q.spread[0]), double(q.spread[1]), unsigned(q.max_particles), int(q.trailing));
                std::fprintf(stderr, "\n");
            }
        }
        uint32_t pseq = inst.seq >= 0 ? uint32_t(inst.seq) : UINT32_MAX;
        float ptime = inst.seq >= 0 ? inst.time_ms : 0.0f;
        const std::vector<Mat4> bw = instance_bones_world(inst, pose_world(m, pseq, ptime, -1, 0.0f, glayers(inst)));
        float tsec = inst.time_ms / 1000.0f;
        std::map<int, std::vector<Mat4>> birth_pose;
        for (const formats::M3Particle& pe : m.particles) {
            ++emitter_uid;
            if (pe.lifetime <= 0.0f) continue;
            if (material_hidden(m, formats::particle_material(m, pe))) continue;
            int ptex = sr.default_white();
            float pbright = 1.0f;
            uint32_t pblend = formats::kBlendAlphaAdd;
            int pmask = -1, pmask_code = 0, pfog = 0, pmask2 = -1, pmask2_code = 0;
            float pdepth = 0.0f;
            float pmask2_mul = 1.0f, pmask2_add = 0.0f;
            float pmask_mul = 1.0f, pmask_add = 0.0f;
            int pemis = 0;
            bool pflip_uv = true;
            bool pglow = false;
            bool pmask_alone = false;
            bool plit = false;
            if (const int pm = formats::particle_material(m, pe); pm >= 0) {
                const char* used = "Diffuse";
                static const bool kEmisMult = std::getenv("SC2_PARTICLE_NO_EMIS_MULT") == nullptr;
                ptex = sr.resolve_texture(m, pm, "Diffuse", -1);
                if (ptex < 0) { ptex = sr.resolve_texture(m, pm, "Emissive", -1); used = "Emissive"; }
                if (ptex < 0) { ptex = sr.resolve_texture(m, pm, "Emissive2", sr.default_white()); used = "Emissive2"; }
                for (const formats::M3Layer& L : m.materials[size_t(pm)].layers)
                    if (L.usage == used) { pbright = L.anim.bright_mult; break; }
                static const bool kFlipAnySource = std::getenv("SC2_PARTICLE_FLIPBOOK_ANY_SOURCE") != nullptr;
                if (!kFlipAnySource)
                    for (const formats::M3Layer& L : m.materials[size_t(pm)].layers)
                        if (L.usage == used && !L.raw_record.empty()) { pflip_uv = L.word40 == 6; break; }
                static const bool kConstEmis = std::getenv("SC2_NO_PARTICLE_CONST_EMISSIVE") == nullptr;
                static const bool kEmisTwice = std::getenv("SC2_PARTICLE_CONST_EMISSIVE_TWICE") != nullptr;
                if (kConstEmis && (kEmisTwice || std::string(used) == "Diffuse"))
                    for (const formats::M3Layer& L : m.materials[size_t(pm)].layers)
                        if (L.usage == "Emissive" && L.const_colour && L.inline_rgba.size() >= 4) {
                            const float k = L.anim.bright_mult * m.materials[size_t(pm)].emis_mult;
                            pemis_table.push_back({float(L.inline_rgba[0]) / 255.0f * k, float(L.inline_rgba[1]) / 255.0f * k, float(L.inline_rgba[2]) / 255.0f * k, 0.0f});
                            pemis_tex.push_back(-1);
                            pemis = int(pemis_table.size()) - 1;
                            break;
                        }
                static const bool kParSel = std::getenv("SC2_PARTICLE_NO_CHANNEL_SELECT") == nullptr;
                if (kParSel)
                    for (const formats::M3Layer& L : m.materials[size_t(pm)].layers)
                        if (L.usage == used && !L.raw_record.empty() && L.word44 == 2) {
                            static const bool kOff = std::getenv("SC2_PARTICLE_TEAM_EMIS_OP_OFF") != nullptr;
                            const bool team_op = !kOff && std::string(used) == "Emissive" && m.materials[size_t(pm)].emis_op == 4;
                            std::array<float, 4> te{0.0f, 0.0f, 0.0f, team_op ? 9.0f : 1.0f};
                            if (team_op)
                                for (int k = 0; k < 3; ++k)
                                    te[std::size_t(k)] = inst.team_emissive[3] >= 0.0f ? inst.team_emissive[k] : (inst.team[3] > 0.0f ? inst.team[k] : 0.0f);
                            if (pemis == 0 || team_op) {
                                pemis_table.push_back(te);
                                pemis_tex.push_back(-1);
                                pemis = int(pemis_table.size()) - 1;
                            } else {
                                pemis_table[std::size_t(pemis)][3] = 1.0f;
                            }
                            break;
                        }
                static const bool kEmisImage = std::getenv("SC2_NO_PARTICLE_EMISSIVE_IMAGE") == nullptr;
                if (kEmisImage && std::string(used) == "Diffuse") {
                    const formats::M3Layer* dl = nullptr;
                    const formats::M3Layer* el = nullptr;
                    for (const formats::M3Layer& L : m.materials[size_t(pm)].layers) {
                        if (L.usage == "Diffuse") dl = &L;
                        else if (L.usage == "Emissive" && !el) el = &L;
                    }
                    if (dl && el && !el->image_path.empty() && dl->image_path != el->image_path) {
                        const int et = sr.resolve_texture(m, pm, "Emissive", -1);
                        const float gain = el->anim.bright_mult * m.materials[size_t(pm)].emis_mult;
                        if (et >= 0 && gain > 0.0f) {
                            if (pemis == 0) {
                                pemis_table.push_back({0.0f, 0.0f, 0.0f, 0.0f});
                                pemis_tex.push_back(-1);
                                pemis = int(pemis_table.size()) - 1;
                            }
                            pemis_tex[std::size_t(pemis)] = et;
                            pemis_table[std::size_t(pemis)][3] += (el->word44 == 2 ? 2.0f : 0.0f) + 10.0f * std::round(gain * 100.0f);
                        }
                    }
                }
                if (std::string(used) == "Diffuse" && !std::getenv("SC2_NO_PARTICLE_EMISSIVE")) {
                    const formats::M3Layer* dl = nullptr;
                    const formats::M3Layer* el = nullptr;
                    for (const formats::M3Layer& L : m.materials[size_t(pm)].layers) {
                        if (L.usage == "Diffuse") dl = &L;
                        else if (L.usage == "Emissive") el = &L;
                    }
                    if (dl && el && !el->image_path.empty() && dl->image_path == el->image_path) pbright += el->anim.bright_mult * (kEmisMult ? m.materials[size_t(pm)].emis_mult : 1.0f);
                } else if (kEmisMult && std::string(used) != "Diffuse") {
                    pbright *= m.materials[size_t(pm)].emis_mult;
                }
                pblend = m.materials[size_t(pm)].blend_mode;
                pdepth = m.materials[size_t(pm)].depth_blend;
                if (pblend > formats::kBlendAlphaAdd) pblend = formats::kBlendAlphaBlend;
                {
                    const formats::M3Layer* dl = nullptr;
                    const formats::M3Layer* el = nullptr;
                    for (const formats::M3Layer& L : m.materials[size_t(pm)].layers) {
                        if (L.usage == "Diffuse") dl = &L;
                        else if (L.usage == "Emissive") el = &L;
                    }
                    pglow = std::string(used) != "Diffuse" || (dl && el && !el->image_path.empty() && dl->image_path == el->image_path) ||
                            (m.materials[size_t(pm)].flags & 0x10u) != 0;
                    static const bool kPlumeProduct = std::getenv("SC2_PLUME_COVER_PRODUCT") != nullptr;
                    pmask_alone = dl && el && !el->image_path.empty() && dl->image_path == el->image_path && !kPlumeProduct;
                    if (pblend != formats::kBlendAdd && pblend != formats::kBlendAlphaAdd && !std::getenv("SC2_GLOW_ALPHABLEND")) pglow = false;
                    if (pglow) pmask_alone = true;
                    if (pmask_alone && !pglow && std::getenv("SC2_PAR_COVER_TRACE"))
                        std::fprintf(stderr, "[parcover] '%s' flags %#x painted through %s, diffuse '%s' emissive '%s', verts %u\n", m.materials[size_t(pm)].name.c_str(),
                                     unsigned(m.materials[size_t(pm)].flags), used, dl ? dl->image_path.c_str() : "-", el ? el->image_path.c_str() : "-", m.vertex_count);
                    plit = std::string(used) == "Diffuse" && !(dl && el && !el->image_path.empty() && dl->image_path == el->image_path) &&
                           (m.materials[size_t(pm)].flags & 0x10u) == 0 && pblend != formats::kBlendAdd && pblend != formats::kBlendAlphaAdd &&
                           sr.tunables().rig.present && !std::getenv("SC2_PARTICLE_UNLIT");
                }
                if (!(m.materials[size_t(pm)].flags & 0x4u) && !std::getenv("SC2_NO_PARTICLE_FOG"))
                    pfog = (pblend == formats::kBlendAdd || pblend == formats::kBlendAlphaAdd) ? 2 : 1;
                if (!std::getenv("SC2_NO_PARTICLE_MASK")) {
                    const formats::M3Material& pmat = m.materials[size_t(pm)];
                    const int code = formats::material_alpha_mask_channel(pmat);
                    if (const formats::M3Layer* ML = formats::material_alpha_mask(pmat); ML && code > 0) {
                        pmask = sr.resolve_texture(m, pm, "AlphaMask", -1);
                        if (pmask >= 0) {
                            pmask_code = code;
                            pmask_mul = ML->anim.bright_mult;
                            pmask_add = ML->anim.midtone_offset;
                        }
                    }
                    for (const formats::M3Layer& L2 : pmat.layers) {
                        if (std::getenv("SC2_PARTICLE_NO_MASK2")) break;
                        if (L2.usage != "AlphaMask2" || L2.word44 < 2 || L2.word44 > 5) continue;
                        pmask2 = sr.resolve_texture(m, pm, "AlphaMask2", -1);
                        if (pmask2 >= 0) {
                            pmask2_code = L2.word44 == 2 ? 1 : int(L2.word44) - 1;
                            pmask2_mul = L2.anim.bright_mult;
                            pmask2_add = L2.anim.midtone_offset;
                        }
                        break;
                    }
                }
            }
            Mat4 mb = pe.bone < bw.size() ? bw[pe.bone] : inst.world;
            Vec3 origin{mb.m[12], mb.m[13], mb.m[14]};
            const std::uint64_t trail_key = (std::uint64_t(inst.trail_id) << 16) | std::uint64_t(std::size_t(&pe - m.particles.data()) & 0xFFFFu);
            const Vec3 origin_now = origin;
            if (inst.trail_id) sr.trail_note(trail_key, inst.trail_time_s, origin_now);
            Vec3 axis = normalize({mb.m[8], mb.m[9], mb.m[10]});
            if (dot(axis, axis) < 0.5f) axis = upc;
            Vec3 t1 = normalize(cross(axis, right));
            if (dot(t1, t1) < 0.5f) t1 = normalize(cross(axis, upc));
            Vec3 t2 = cross(axis, t1);
            const Mat4 mb_now = mb;
            const Vec3 axis_now = axis, t1_now = t1, t2_now = t2;
            static const bool kParScale = std::getenv("SC2_NO_PARTICLE_SCALE") == nullptr;
            float iscale = kParScale ? std::sqrt(inst.world.m[0] * inst.world.m[0] + inst.world.m[1] * inst.world.m[1] + inst.world.m[2] * inst.world.m[2]) : 1.0f;
            static const bool kParBoneScale = std::getenv("SC2_PARTICLE_INSTANCE_SCALE") == nullptr;
            static const bool kParStaticBoneScale = std::getenv("SC2_PARTICLE_STATIC_BONE_SCALE") != nullptr;
            if (kParScale && kParBoneScale && pe.bone < bw.size() && pe.bone < m.bones.size() && (kParStaticBoneScale || m.bones[pe.bone].scale.anim_flags != 0)) {
                const float sx = std::sqrt(mb.m[0] * mb.m[0] + mb.m[1] * mb.m[1] + mb.m[2] * mb.m[2]);
                const float sy = std::sqrt(mb.m[4] * mb.m[4] + mb.m[5] * mb.m[5] + mb.m[6] * mb.m[6]);
                const float sz = std::sqrt(mb.m[8] * mb.m[8] + mb.m[9] * mb.m[9] + mb.m[10] * mb.m[10]);
                iscale = (sx + sy + sz) / 3.0f;
            }
            formats::M3AnimRef rate_ref;
            rate_ref.interp = pe.rate_interp;
            rate_ref.anim_flags = pe.rate_anim_flags;
            rate_ref.anim_id = pe.rate_anim_id;
            rate_ref.init[0] = pe.emission_rate;
            const float live_rate =
                animated_scalar(m, pseq, rate_ref, inst.time_ms, pe.emission_rate, glayers(inst));
            int n = formats::particle_count_at_rate(pe, live_rate);
            static const bool kBurstCount = std::getenv("SC2_PAR_BURST_COUNT") != nullptr;
            if (kBurstCount && pe.rate_anim_flags && inst.seq >= 0 && std::size_t(inst.seq) < m.sequences.size() &&
                (m.sequences[std::size_t(inst.seq)].flags & 0x1u) != 0 && pe.lifetime > 0.0f) {
                const float t1 = inst.time_ms, t0 = std::max(0.0f, inst.time_ms - pe.lifetime * 1000.0f);
                float emitted = 0.0f;
                constexpr int kSteps = 24;
                const float dt = (t1 - t0) / float(kSteps);
                for (int k = 0; k < kSteps; ++k)
                    emitted += std::max(0.0f, animated_scalar(m, pseq, rate_ref, t0 + dt * (float(k) + 0.5f), pe.emission_rate)) * dt / 1000.0f;
                n = std::min(n, int(std::lround(emitted)));
            }
            if (const char* ta = std::getenv("SC2_PAR_TRACE_ACTIVE"); ta && unsigned(std::atoi(ta)) == m.vertex_count && pe.emission_rate == 0.0f && live_rate > 0.0f) {
                static int act = 0;
                if (act < 30) {
                    ++act;
                    std::fprintf(stderr, "[paract] emitter %zu seq %d '%s' t %.0f live rate %.2f n %d rate_anim %u\n", std::size_t(&pe - m.particles.data()), inst.seq,
                                 inst.seq >= 0 && std::size_t(inst.seq) < m.sequences.size() ? m.sequences[std::size_t(inst.seq)].name.c_str() : "?", double(inst.time_ms),
                                 double(live_rate), n, unsigned(pe.rate_anim_id));
                }
            }
            if (const char* tv = std::getenv("SC2_PAR_TRACE_VERTS"); tv && unsigned(std::atoi(tv)) == m.vertex_count) {
                static int traced = 0;
                if (traced < 40) {
                    ++traced;
                    std::fprintf(stderr, "[par] emitter %zu bone %u seq %d t %.0f origin %.2f %.2f %.2f inst %.2f %.2f %.2f rate init %.2f live %.2f n %d life %.2f speed %.2f size %.2f/%.2f/%.2f blend %u tex %d (white %d) bright %.2f mat %d emis %d\n",
                                 std::size_t(&pe - m.particles.data()), unsigned(pe.bone), inst.seq, double(inst.time_ms), double(origin.x), double(origin.y), double(origin.z),
                                 double(inst.world.m[12]), double(inst.world.m[13]), double(inst.world.m[14]), double(pe.emission_rate), double(live_rate), n, double(pe.lifetime),
                                 double(pe.emission_speed), double(pe.size_start), double(pe.size_mid), double(pe.size_end), unsigned(pblend), ptex, sr.default_white(), double(pbright),
                                 formats::particle_material(m, pe), pemis);
                    std::fprintf(stderr, "[par]   lod reduce %u cut %u (ext %d)\n", unsigned(pe.lod_reduce), unsigned(pe.lod_cut), int(pe.has_ext));
                    std::fprintf(stderr, "[par]   v%u speed2 %.2f rand %d life2 %.2f rand %d addflags 0x%x rate anim flags %u id %08x flipbook %ux%u phase %u %u %u len %.2f random %d\n", unsigned(pe.version), double(pe.emission_speed2), int(pe.rand_speed2), double(pe.lifetime2), int(pe.rand_life2), unsigned(pe.additional_flags),
                                 unsigned(pe.rate_anim_flags), unsigned(pe.rate_anim_id), unsigned(pe.flip_cols), unsigned(pe.flip_rows), unsigned(pe.flip_phase[0]), unsigned(pe.flip_phase[1]),
                                 unsigned(pe.flip_phase[2]), double(pe.flip_phase1_len), int(pe.flip_random_start));
                    std::fprintf(stderr, "[par]   has_color %d rgba0 %.2f %.2f %.2f %.2f rgba1 %.2f %.2f %.2f %.2f rgba2 %.2f %.2f %.2f %.2f mid %.2f/%.2f mask %d code %d x%.2f +%.2f mask2 %d code %d x%.2f +%.2f fog %d ptype %u shape %d zacc %.2f slow %.2f\n",
                                 int(pe.has_color), double(pe.color[0][0]), double(pe.color[0][1]), double(pe.color[0][2]), double(pe.color[0][3]), double(pe.color[1][0]), double(pe.color[1][1]), double(pe.color[1][2]), double(pe.color[1][3]),
                                 double(pe.color[2][0]), double(pe.color[2][1]), double(pe.color[2][2]), double(pe.color[2][3]), double(pe.color_mid_at), double(pe.alpha_mid_at), pmask, pmask_code, double(pmask_mul), double(pmask_add),
                                 pmask2, pmask2_code, double(pmask2_mul), double(pmask2_add), pfog, unsigned(pe.ptype), int(pe.shape), double(pe.z_accel), double(pe.slowdown));
                    std::fprintf(stderr, "[par]   area %.2f %.2f %.2f radius %.2f angle %.2f %.2f spread %.2f %.2f tail2 %d randsize %d size2 %.3f %.3f %.3f etype %u scale %.3f bone scale %.3f %.3f %.3f\n", double(pe.area_size[0]),
                                 double(pe.area_size[1]), double(pe.area_size[2]), double(pe.area_radius), double(pe.angle[0]), double(pe.angle[1]), double(pe.spread[0]), double(pe.spread[1]), int(pe.has_tail2),
                                 int(pe.rand_size2), double(pe.size2[0]), double(pe.size2[1]), double(pe.size2[2]), unsigned(pe.emission_type),
                                 double(std::sqrt(inst.world.m[0] * inst.world.m[0] + inst.world.m[1] * inst.world.m[1] + inst.world.m[2] * inst.world.m[2])),
                                 double(std::sqrt(mb.m[0] * mb.m[0] + mb.m[1] * mb.m[1] + mb.m[2] * mb.m[2])), double(std::sqrt(mb.m[4] * mb.m[4] + mb.m[5] * mb.m[5] + mb.m[6] * mb.m[6])),
                                 double(std::sqrt(mb.m[8] * mb.m[8] + mb.m[9] * mb.m[9] + mb.m[10] * mb.m[10])));
                }
            }
            static const bool kParRange = std::getenv("SC2_PARTICLE_NO_RANGE") == nullptr;
            if (kParRange && pe.rand_life2 && pe.lifetime2 > 0.0f && pe.lifetime > 0.0f)
                n = int(float(n) * 0.5f * (pe.lifetime + pe.lifetime2) / pe.lifetime + 0.5f);
            static const bool kParSteady = std::getenv("SC2_PAR_STEADY") != nullptr;
            const bool one_shot = inst.seq >= 0 && std::size_t(inst.seq) < m.sequences.size() && (m.sequences[std::size_t(inst.seq)].flags & 0x1u) != 0;
            const float clip_len_ms = (inst.seq >= 0 && std::size_t(inst.seq) < m.sequences.size()) ? float(m.sequences[std::size_t(inst.seq)].length_ms()) : 0.0f;
            const float life_max = std::max(pe.lifetime, (pe.rand_life2 && pe.lifetime2 > 0.0f) ? pe.lifetime2 : 0.0f);
            const bool history = !kParSteady && inst.seq >= 0 && (one_shot || pe.rate_anim_flags);
            float rate_max = live_rate;
            if (history && pe.rate_anim_flags) {
                constexpr int kSteps = 32;
                for (int k = 0; k <= kSteps; ++k) {
                    float tb = inst.time_ms - life_max * 1000.0f * float(k) / float(kSteps);
                    if (tb < 0.0f) {
                        if (one_shot || clip_len_ms <= 0.0f) break;
                        tb = std::fmod(tb, clip_len_ms) + clip_len_ms;
                    }
                    rate_max = std::max(rate_max, animated_scalar(m, pseq, rate_ref, tb, pe.emission_rate));
                }
                if (rate_max > live_rate) {
                    n = formats::particle_count_at_rate(pe, rate_max);
                    if (kParRange && pe.rand_life2 && pe.lifetime2 > 0.0f && pe.lifetime > 0.0f)
                        n = int(float(n) * 0.5f * (pe.lifetime + pe.lifetime2) / pe.lifetime + 0.5f);
                }
            }
            bool burst_mode = false;
            float burst_t = 0.0f;
            if (pe.burst_flags && inst.seq >= 0 && live_rate <= 0.0f && rate_max <= 0.0f && clip_len_ms > 0.0f) {
                formats::M3AnimRef bref;
                bref.interp = pe.burst_interp;
                bref.anim_flags = pe.burst_flags;
                bref.anim_id = pe.burst_id;
                float peak = 0.0f, first = -1.0f;
                constexpr int kSteps = 64;
                for (int k = 0; k <= kSteps; ++k) {
                    const float tb = clip_len_ms * float(k) / float(kSteps);
                    const float v = animated_scalar(m, pseq, bref, tb, 0.0f);
                    if (v >= 0.5f && first < 0.0f) first = tb;
                    peak = std::max(peak, v);
                }
                if (first >= 0.0f && peak >= 0.5f) {
                    burst_mode = true;
                    burst_t = first;
                    n = std::min(int(peak + 0.5f), formats::particle_cap(pe));
                }
            }
            if (n <= 0) continue;
            static const bool kTrailQuads = std::getenv("SC2_PAR_TRAIL_AS_QUADS") != nullptr;
            struct TrailPt { float age; Vec3 c; float size; float rgb[3]; float fade; };
            std::vector<TrailPt> trail_pts;
            const bool as_trail = !kTrailQuads && pe.ptype == 6;
            for (int i = 0; i < n; ++i) {
                float u1, u2, u3;
                particle_rng(emitter_uid, i, u1, u2, u3);
                mb = mb_now;
                axis = axis_now;
                t1 = t1_now;
                t2 = t2_now;
                origin = origin_now;
                float birth_at = -1.0f;
                if (history && !burst_mode) {
                    float lb = pe.lifetime;
                    if (kParRange && pe.rand_life2 && pe.lifetime2 > 0.0f) {
                        float ua, ub, uc;
                        particle_rng(emitter_uid ^ 0x9e3779b9u, i, ua, ub, uc);
                        lb += (pe.lifetime2 - pe.lifetime) * ua;
                    }
                    const float age_b = std::fmod(tsec + u1 * lb, lb);
                    float birth_ms = inst.time_ms - age_b * 1000.0f;
                    if (birth_ms < 0.0f) {
                        if (one_shot) continue;
                        if (clip_len_ms > 0.0f) birth_ms = std::fmod(birth_ms, clip_len_ms) + clip_len_ms;
                    }
                    if (pe.rate_anim_flags && rate_max > 0.0f) {
                        float k1, k2, k3;
                        particle_rng(emitter_uid ^ 0x27d4eb2fu, i, k1, k2, k3);
                        const float rb = animated_scalar(m, pseq, rate_ref, birth_ms, pe.emission_rate);
                        if (k1 >= rb / rate_max) continue;
                    }
                    birth_at = birth_ms;
                }
                static const bool kBirthPose = std::getenv("SC2_PAR_POSE_NOW") == nullptr;
                if (kBirthPose && birth_at >= 0.0f && pe.bone < bw.size()) {
                    const int q = int(birth_at / 33.333f);
                    auto bp = birth_pose.find(q);
                    if (bp == birth_pose.end()) bp = birth_pose.emplace(q, instance_bones_world(inst, pose_world(m, pseq, (float(q) + 0.5f) * 33.333f))).first;
                    if (pe.bone < bp->second.size()) {
                        mb = bp->second[pe.bone];
                        origin = {mb.m[12], mb.m[13], mb.m[14]};
                        axis = normalize({mb.m[8], mb.m[9], mb.m[10]});
                        if (dot(axis, axis) < 0.5f) axis = upc;
                        t1 = normalize(cross(axis, right));
                        if (dot(t1, t1) < 0.5f) t1 = normalize(cross(axis, upc));
                        t2 = cross(axis, t1);
                    }
                }
                float p_life = pe.lifetime, p_speed = pe.emission_speed;
                if (kParRange) {
                    float ua, ub, uc;
                    particle_rng(emitter_uid ^ 0x9e3779b9u, i, ua, ub, uc);
                    if (pe.rand_life2 && pe.lifetime2 > 0.0f) p_life += (pe.lifetime2 - pe.lifetime) * ua;
                    if (pe.rand_speed2) p_speed += (pe.emission_speed2 - pe.emission_speed) * ub;
                }
                float phase = u1 * p_life;
                float age = std::fmod(tsec + phase, p_life);
                if (age < 0.0f) age += p_life;
                if (burst_mode) {
                    age = (inst.time_ms - burst_t) * 0.001f;
                    if (age < 0.0f || age >= p_life) continue;
                }
                float life01 = age / p_life;
                if (inst.trail_id && pe.trailing) origin = sr.trail_origin(trail_key, inst.trail_time_s, age, origin_now);
                if (inst.path_ended_s > 0.0f && (!pe.trailing_flag || age < inst.path_ended_s)) continue;
                if (inst.path_speed > 0.0f && pe.trailing_flag) {
                    const Vec3 back{inst.path_from.x - origin_now.x, inst.path_from.y - origin_now.y, inst.path_from.z - origin_now.z};
                    const float flown = std::sqrt(dot(back, back)), d = inst.path_speed * (age - inst.path_ended_s);
                    if (std::getenv("SC2_SHOT_TRACE") && inst.path_speed > 0.0f) {
                        static int told = 0;
                        if (told++ < 60)
                            std::fprintf(stderr, "[shotpath] addflags 0x%x emitter %zu type %u particle %d speed %.2f flown %.2f age %.3f life %.3f n %d kept %d now %.2f %.2f %.2f from %.2f %.2f %.2f\n", unsigned(pe.additional_flags), std::size_t(&pe - m.particles.data()), unsigned(pe.ptype), i,
                                         double(inst.path_speed), double(flown), double(age), double(p_life), n, int(d <= flown + 1e-3f), double(origin_now.x), double(origin_now.y), double(origin_now.z),
                                         double(inst.path_from.x), double(inst.path_from.y), double(inst.path_from.z));
                    }
                    if (d > flown + 1e-3f) continue;
                    if (flown > 1e-4f) origin = {origin_now.x + back.x * d / flown, origin_now.y + back.y * d / flown, origin_now.z + back.z * d / flown};
                }
                Vec3 dir;
                Vec3 c;
                Vec3 spawn = origin;
                if (pe.has_shape) {
                    float u4, u5, u6;
                    particle_rng(emitter_uid ^ 0x5bd1e995u, i, u4, u5, u6);
                    auto turn = [](Vec3 v, Vec3 about, float a) {
                        const float ca = std::cos(a), sa = std::sin(a);
                        Vec3 cr = cross(about, v);
                        const float d = dot(about, v) * (1.0f - ca);
                        return Vec3{v.x * ca + cr.x * sa + about.x * d, v.y * ca + cr.y * sa + about.y * d,
                                    v.z * ca + cr.z * sa + about.z * d};
                    };
                    dir = turn(turn(axis, t1, pe.angle[0]), t2, pe.angle[1]);
                    dir = turn(turn(dir, t1, pe.spread[0] * (2.0f * u2 - 1.0f)), t2,
                               pe.spread[1] * (2.0f * u3 - 1.0f));
                    if (const char* tv = std::getenv("SC2_PAR_TRACE_VERTS"); tv && unsigned(std::atoi(tv)) == m.vertex_count) {
                        static int traced_dir = 0;
                        if (traced_dir++ < 6)
                            std::fprintf(stderr, "[par]   particle %d axis %.2f %.2f %.2f dir %.2f %.2f %.2f speed %.2f life %.2f age %.2f\n", i, double(axis.x), double(axis.y),
                                         double(axis.z), double(dir.x), double(dir.y), double(dir.z), double(p_speed), double(p_life), double(age));
                    }
                    Vec3 off{0, 0, 0};
                    const float az = u4 * 6.2831853f;
                    static const char* kPlaneFrame = std::getenv("SC2_PARTICLE_PLANE_FRAME");
                    static float plane_sx = 0.0f, plane_sy = 0.0f;
                    static const bool plane_frame = kPlaneFrame && std::sscanf(kPlaneFrame, "%f,%f", &plane_sx, &plane_sy) == 2;
                    Vec3 e1 = t1, e2 = t2;
                    switch (pe.shape) {
                        case 1: {
                            static const float kPlane = std::getenv("SC2_PARTICLE_PLANE_SCALE") ? float(std::atof(std::getenv("SC2_PARTICLE_PLANE_SCALE"))) : 0.5f;
                            off = {pe.area_size[0] * kPlane * (2.0f * u5 - 1.0f), pe.area_size[1] * kPlane * (2.0f * u6 - 1.0f), 0.0f};
                            if (plane_frame) {
                                off.x += pe.area_size[0] * 2.0f * kPlane * plane_sx;
                                off.y += pe.area_size[1] * 2.0f * kPlane * plane_sy;
                            }
                            static const bool kCameraFrame = std::getenv("SC2_PARTICLE_PLANE_CAMERA_FRAME") != nullptr;
                            if (!kCameraFrame) {
                                const Vec3 bx{mb.m[0], mb.m[1], mb.m[2]}, by{mb.m[4], mb.m[5], mb.m[6]};
                                if (dot(bx, bx) > 1e-8f && dot(by, by) > 1e-8f) {
                                    e1 = normalize(bx);
                                    e2 = normalize(by);
                                }
                            }
                            break;
                        }
                        case 2: {
                            const float r = pe.area_radius * std::cbrt(u5);
                            const float cz = 2.0f * u6 - 1.0f, sz = std::sqrt(std::max(0.0f, 1.0f - cz * cz));
                            off = {r * sz * std::cos(az), r * sz * std::sin(az), r * cz};
                            break;
                        }
                        case 3:
                            off = {pe.area_size[0] * (2.0f * u5 - 1.0f), pe.area_size[1] * (2.0f * u6 - 1.0f),
                                   pe.area_size[2] * (2.0f * u4 - 1.0f)};
                            break;
                        case 4:
                        case 5: {
                            const float r = pe.area_radius * std::sqrt(u5);
                            const float z = pe.shape == 4 ? pe.area_size[2] * (2.0f * u6 - 1.0f) : 0.0f;
                            off = {r * std::cos(az), r * std::sin(az), z};
                            break;
                        }
                        default: break;
                    }
                    off = {off.x * iscale, off.y * iscale, off.z * iscale};
                    Vec3 o2{origin.x + e1.x * off.x + e2.x * off.y + axis.x * off.z,
                            origin.y + e1.y * off.x + e2.y * off.y + axis.y * off.z,
                            origin.z + e1.z * off.x + e2.z * off.y + axis.z * off.z};
                    spawn = o2;
                    if (pe.emission_type == 1) {
                        const Vec3 rd = sub(o2, origin);
                        if (dot(rd, rd) > 1e-10f) dir = normalize(rd);
                    }
                    const float k = pe.slowdown;
                    const float travel = k > 1e-4f ? p_speed * (1.0f - std::exp(-k * age)) / k
                                                   : p_speed * age;
                    static const bool kFallWorld = std::getenv("SC2_PARTICLE_FALL_SCALED") == nullptr;
                    const float fall = 0.5f * pe.z_accel * age * age * (kFallWorld ? 1.0f : iscale);
                    c = {o2.x + dir.x * travel * iscale, o2.y + dir.y * travel * iscale, o2.z + dir.z * travel * iscale + fall};
                } else {
                    float ct = 1.0f - u2 * (1.0f - 0.9f);
                    float st = std::sqrt(std::max(0.0f, 1.0f - ct * ct));
                    float az = u3 * 6.2831853f;
                    dir = {axis.x * ct + (t1.x * std::cos(az) + t2.x * std::sin(az)) * st,
                           axis.y * ct + (t1.y * std::cos(az) + t2.y * std::sin(az)) * st,
                           axis.z * ct + (t1.z * std::cos(az) + t2.z * std::sin(az)) * st};
                    float travel = p_speed * age * iscale;
                    c = {origin.x + dir.x * travel, origin.y + dir.y * travel, origin.z + dir.z * travel};
                }
                auto key3 = [&](float a, float b, float c, float mid_at) {
                    return life01 < mid_at ? a + (b - a) * (life01 / std::max(mid_at, 1e-4f))
                                           : b + (c - b) * ((life01 - mid_at) / std::max(1.0f - mid_at, 1e-4f));
                };
                float size = pe.has_color ? key3(pe.size_start, pe.size_mid, pe.size_end, pe.size_mid_at)
                                          : pe.size_start + (pe.size_end - pe.size_start) * life01;
                float ur, ur2, ur3;
                particle_rng(emitter_uid ^ 0x2545F491u, i, ur, ur2, ur3);
                if (pe.has_tail2 && pe.rand_size2)
                    size += (key3(pe.size2[0], pe.size2[1], pe.size2[2], pe.size_mid_at) - size) * ur;
                size *= iscale;
                if (size <= 0.0f) continue;
                static const bool kBeyondField = std::getenv("SC2_PARTICLE_BEYOND_FIELD") != nullptr;
                if (!kBeyondField && !sr.fog_field_contains(c.x, c.y)) continue;
                {
                    static const char* kFloor = std::getenv("SC2_PARTICLE_FLOOR_Z");
                    static const float kFloorZ = kFloor ? float(std::atof(kFloor)) : 0.0f;
                    if (kFloor && c.z < kFloorZ) continue;
                }
                float rgb[3] = {1.0f, 1.0f, 1.0f};
                float fade = std::sin(3.14159265f * life01);
                if (pe.has_color) {
                    for (int ch = 0; ch < 3; ++ch)
                        rgb[ch] = key3(pe.color[0][ch], pe.color[1][ch], pe.color[2][ch], pe.color_mid_at);
                    fade = key3(pe.color[0][3], pe.color[1][3], pe.color[2][3], pe.alpha_mid_at);
                    if (pe.has_tail2 && pe.rand_color2) {
                        for (int ch = 0; ch < 3; ++ch)
                            rgb[ch] += (key3(pe.color2[0][ch], pe.color2[1][ch], pe.color2[2][ch], pe.color_mid_at) - rgb[ch]) * ur;
                        fade += (key3(pe.color2[0][3], pe.color2[1][3], pe.color2[2][3], pe.alpha_mid_at) - fade) * ur;
                    }
                }
                float spin = 0.0f;
                if (pe.has_tail2) {
                    spin = key3(pe.rot[0], pe.rot[1], pe.rot[2], pe.rot_mid_at);
                    if (pe.rand_rot2) spin += (key3(pe.rot2[0], pe.rot2[1], pe.rot2[2], pe.rot_mid_at) - spin) * ur2;
                }
                const float spin_c = std::cos(spin), spin_s = std::sin(spin);
                if (fade <= 0.0f) continue;
                const float pgain = pbright * (pblend == formats::kBlendAdd ? fade : 1.0f);
                for (float& ch : rgb) ch *= pgain;
                if (plit) for (int ch = 0; ch < 3; ++ch) rgb[ch] *= lit_shade[ch];
                if (const char* tv = std::getenv("SC2_PAR_TRACE_VERTS"); tv && unsigned(std::atoi(tv)) == m.vertex_count) {
                    static int traced_col = 0;
                    if (i < 3 && traced_col++ < 27)
                        std::fprintf(stderr, "[par]   colour %d lit %d shade %.3f %.3f %.3f gain %.3f rgb %.3f %.3f %.3f alpha %.3f life %.2f glow %d fog %d exposure %.3f\n", i, int(plit), double(lit_shade[0]),
                                     double(lit_shade[1]), double(lit_shade[2]), double(pgain), double(rgb[0]), double(rgb[1]), double(rgb[2]), double(fade), double(life01), int(pglow), pfog,
                                     double(sr.tunables().exposure));
                    static int traced_pos = 0;
                    static const bool kPlaceAll = std::getenv("SC2_PAR_PLACE_ALL") != nullptr;
                    if (kPlaceAll || (i < 8 && traced_pos++ < 96))
                        std::fprintf(stderr, "[par]   place %d spawn %.2f %.2f %.2f now %.2f %.2f %.2f age %.2f size %.3f\n", i, double(spawn.x), double(spawn.y), double(spawn.z), double(c.x), double(c.y),
                                     double(c.z), double(age), double(size));
                }
                if (as_trail) {
                    TrailPt tp;
                    tp.age = age;
                    tp.c = c;
                    tp.size = size;
                    for (int ch = 0; ch < 3; ++ch) tp.rgb[ch] = rgb[ch];
                    tp.fade = fade;
                    trail_pts.push_back(tp);
                    continue;
                }
                const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                const int flip_cells = pflip_uv ? int(pe.flip_cols) * int(pe.flip_rows) : 1;
                int flip_cell = 0;
                if (flip_cells > 1) {
                    if (pe.flip_random_start) {
                        flip_cell = int(u1 * float(flip_cells));
                    } else {
                        const float len = pe.flip_phase1_len;
                        const float a0 = float(pe.flip_phase[0]), a1 = float(pe.flip_phase[1]), a2 = float(pe.flip_phase[2]);
                        const float fcell = life01 < len ? a0 + (a1 - a0) * (life01 / std::max(len, 1e-4f))
                                                         : a1 + (a2 - a1) * ((life01 - len) / std::max(1.0f - len, 1e-4f));
                        flip_cell = int(fcell + 0.5f);
                    }
                    flip_cell = std::min(flip_cells - 1, std::max(0, flip_cell));
                }
                SceneRenderer::ParticleVertex q[4];
                for (int k = 0; k < 4; ++k) {
                    const float cxr = corner[k][0] * spin_c - corner[k][1] * spin_s;
                    const float cyr = corner[k][0] * spin_s + corner[k][1] * spin_c;
                    static const float kSizeScale = std::getenv("SC2_PARTICLE_SIZE_SCALE") ? float(std::atof(std::getenv("SC2_PARTICLE_SIZE_SCALE"))) : 0.5f;
                    float ox = cxr * size * kSizeScale, oy = cyr * size * kSizeScale;
                    q[k].pos[0] = c.x + right.x * ox + upc.x * oy;
                    q[k].pos[1] = c.y + right.y * ox + upc.y * oy;
                    q[k].pos[2] = c.z + right.z * ox + upc.z * oy;
                    q[k].uv[0] = (corner[k][0] + 1.0f) * 0.5f;
                    q[k].uv[1] = (corner[k][1] + 1.0f) * 0.5f;
                    if (flip_cells > 1) {
                        q[k].uv[0] = (float(flip_cell % int(pe.flip_cols)) + q[k].uv[0]) / float(pe.flip_cols);
                        q[k].uv[1] = (float(flip_cell / int(pe.flip_cols)) + q[k].uv[1]) / float(pe.flip_rows);
                    }
                    q[k].color[0] = rgb[0];
                    q[k].color[1] = rgb[1];
                    q[k].color[2] = rgb[2];
                    q[k].color[3] = fade;
                }
                static const bool kType1Square = std::getenv("SC2_PAR_TYPE1_SQUARE") != nullptr;
                static const char* kFlatMat = std::getenv("SC2_PAR_FLAT_MAT");
                if (kFlatMat && pe.ptype == 0) {
                    const int mi = formats::particle_material(m, pe);
                    if (mi >= 0 && m.materials[std::size_t(mi)].name == kFlatMat) {
                        Vec3 h{axis.x, axis.y, 0.0f};
                        if (dot(h, h) < 1e-6f) h = Vec3{1, 0, 0};
                        h = normalize(h);
                        static const bool kFlatTurn = std::getenv("SC2_PAR_FLAT_TURN") != nullptr;
                        if (kFlatTurn) h = Vec3{-h.y, h.x, 0.0f};
                        const Vec3 s2{-h.y, h.x, 0.0f};
                        static const float kFlatScale = std::getenv("SC2_PAR_FLAT_SCALE") ? float(std::atof(std::getenv("SC2_PAR_FLAT_SCALE"))) : 1.0f;
                        const float fs = size * kFlatScale;
                        for (int k = 0; k < 4; ++k) {
                            q[k].pos[0] = c.x + h.x * fs * corner[k][0] + s2.x * fs * corner[k][1];
                            q[k].pos[1] = c.y + h.y * fs * corner[k][0] + s2.y * fs * corner[k][1];
                            q[k].pos[2] = c.z;
                        }
                    }
                }
                if ((pe.ptype == 9 || pe.ptype == 7 || (pe.ptype == 1 && !kType1Square)) && (pe.ptype != 1 || dot(dir, dir) > 1e-8f)) {
                    const Vec3 view = normalize(sub(c, cam.eye));
                    Vec3 a{0, 0, 0}, b{0, 0, 0}, mid = c;
                    bool ok = false;
                    if (pe.ptype == 1) {
                        const Vec3 dn = normalize(dir);
                        const Vec3 along = sub(dn, Vec3{view.x * dot(dn, view), view.y * dot(dn, view), view.z * dot(dn, view)});
                        if (dot(along, along) > 1e-8f) {
                            const Vec3 al = normalize(along), sd = normalize(cross(al, view));
                            const float hw = size * 0.5f, hl = hw * std::max(pe.length_width, 1.0f);
                            a = {al.x * hl, al.y * hl, al.z * hl};
                            b = {sd.x * hw, sd.y * hw, sd.z * hw};
                            ok = true;
                        }
                    } else if (pe.ptype == 9) {
                        const Vec3 ray = sub(c, spawn);
                        const float len = std::sqrt(dot(ray, ray));
                        if (len > 1e-5f) {
                            const Vec3 side = cross(ray, view);
                            if (dot(side, side) > 1e-10f) {
                                const Vec3 sn = normalize(side);
                                a = {ray.x * 0.5f, ray.y * 0.5f, ray.z * 0.5f};
                                b = {sn.x * size, sn.y * size, sn.z * size};
                                mid = {spawn.x + a.x, spawn.y + a.y, spawn.z + a.z};
                                ok = true;
                            }
                        }
                    } else {
                        const Vec3 ref = std::fabs(dir.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
                        const Vec3 e1 = normalize(cross(dir, ref)), e2 = cross(dir, e1);
                        a = {e1.x * size, e1.y * size, e1.z * size};
                        b = {e2.x * size, e2.y * size, e2.z * size};
                        ok = true;
                    }
                    if (ok)
                        for (int k = 0; k < 4; ++k) {
                            q[k].pos[0] = mid.x + a.x * corner[k][0] + b.x * corner[k][1];
                            q[k].pos[1] = mid.y + a.y * corner[k][0] + b.y * corner[k][1];
                            q[k].pos[2] = mid.z + a.z * corner[k][0] + b.z * corner[k][1];
                        }
                }
                PQuad pq;
                pq.depth = dot(sub(c, cam.eye), fwd);
                for (int k = 0; k < 4; ++k) pq.v[k] = q[k];
                bytex[PKey{pblend, ptex, pmask, pmask_code, pmask_mul, pmask_add, pfog + (pglow ? 4 : 0) + (pmask_alone ? 8 : 0), pmask2, pmask2_code, pmask2_mul, pmask2_add, pdepth, pemis}].push_back(pq);
            }
            if (as_trail && trail_pts.size() >= 2) {
                std::sort(trail_pts.begin(), trail_pts.end(), [](const TrailPt& a, const TrailPt& b) { return a.age < b.age; });
                const float span = std::max(trail_pts.back().age - trail_pts.front().age, 1e-4f);
                for (std::size_t k = 0; k + 1 < trail_pts.size(); ++k) {
                    const TrailPt& p0 = trail_pts[k];
                    const TrailPt& p1 = trail_pts[k + 1];
                    const Vec3 seg = sub(p1.c, p0.c);
                    if (dot(seg, seg) < 1e-10f) continue;
                    const Vec3 view = normalize(sub(p0.c, cam.eye));
                    const Vec3 sd = cross(seg, view);
                    if (dot(sd, sd) < 1e-12f) continue;
                    const Vec3 sn = normalize(sd);
                    const TrailPt* pp[2] = {&p0, &p1};
                    SceneRenderer::ParticleVertex q[4];
                    for (int e = 0; e < 2; ++e) {
                        const TrailPt& t = *pp[e];
                        const float hw = t.size * 0.5f;
                        const float u = (t.age - trail_pts.front().age) / span;
                        for (int sgn = 0; sgn < 2; ++sgn) {
                            SceneRenderer::ParticleVertex& v = q[e == 0 ? (sgn == 0 ? 0 : 3) : (sgn == 0 ? 1 : 2)];
                            const float s = sgn == 0 ? -hw : hw;
                            v.pos[0] = t.c.x + sn.x * s;
                            v.pos[1] = t.c.y + sn.y * s;
                            v.pos[2] = t.c.z + sn.z * s;
                            v.uv[0] = u;
                            v.uv[1] = sgn == 0 ? 0.0f : 1.0f;
                            v.color[0] = t.rgb[0];
                            v.color[1] = t.rgb[1];
                            v.color[2] = t.rgb[2];
                            v.color[3] = t.fade;
                        }
                    }
                    PQuad pq;
                    pq.depth = dot(sub(p0.c, cam.eye), fwd);
                    for (int kk = 0; kk < 4; ++kk) pq.v[kk] = q[kk];
                    bytex[PKey{pblend, ptex, pmask, pmask_code, pmask_mul, pmask_add, pfog + (pglow ? 4 : 0) + (pmask_alone ? 8 : 0), pmask2, pmask2_code, pmask2_mul, pmask2_add, pdepth, pemis}].push_back(pq);
                }
            }
        }
    }
    for (const SceneInstance* instp : live) {
        const SceneInstance& inst = *instp;
        const formats::Model& m = *inst.model;
        if (m.ribbons.empty()) continue;
        static const bool kNoRibbons = std::getenv("SC2_NO_RIBBONS") != nullptr;
        if (kNoRibbons) continue;
        const uint32_t rseq = inst.seq >= 0 ? uint32_t(inst.seq) : UINT32_MAX;
        const std::vector<Mat4> bw = instance_bones_world(inst, pose_world(m, rseq, inst.seq >= 0 ? inst.time_ms : 0.0f, -1, 0.0f, glayers(inst)));
        for (const formats::M3Ribbon& rb : m.ribbons) {
            static const bool kFreeHang = std::getenv("SC2_NO_FREE_RIBBON_HANG") == nullptr;
            float hang_len = 0.0f;
            if (kFreeHang && rb.end_bone < 0 && rb.version == 6 && rb.raw.size() >= 424) {
                float g = 0.0f, life = 0.0f;
                std::memcpy(&g, &rb.raw[188], 4);
                std::memcpy(&life, &rb.raw[420], 4);
                if (g < 0.0f && g > -100.0f && life > 0.0f && life < 30.0f) hang_len = 0.5f * -g * life * life;
            }
            const bool hang = hang_len > 0.0f;
            if (!rb.has_fields || (rb.end_bone < 0 && !hang)) continue;
            if (rb.bone >= bw.size() || (!hang && size_t(rb.end_bone) >= bw.size())) continue;
            static const bool kRibHidden = std::getenv("SC2_RIBBON_ON_HIDDEN_BONE") != nullptr;
            if (!kRibHidden) {
                auto collapsed = [](const Mat4& q) { return q.m[0] * q.m[0] + q.m[1] * q.m[1] + q.m[2] * q.m[2] < 1e-10f; };
                if (collapsed(bw[rb.bone]) || (!hang && collapsed(bw[size_t(rb.end_bone)]))) continue;
            }
            int rtex = sr.default_white();
            uint32_t rblend = formats::kBlendAlphaAdd;
            float manim[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            float ruvx[3] = {1.0f, 1.0f, 0.0f};
            if (const int rm = formats::ribbon_material(m, rb); rm >= 0) {
                rtex = sr.resolve_texture(m, rm, "Diffuse", sr.default_white());
                rblend = m.materials[size_t(rm)].blend_mode;
                if (rblend > formats::kBlendAlphaAdd) rblend = formats::kBlendAlphaBlend;
                material_anim(m, rm, inst.seq, inst.time_ms, manim, &inst.global_layers);
                material_uv_xform(m, rm, inst.seq, inst.time_ms, ruvx, &inst.global_layers);
            }
            static const bool kRibTeam = std::getenv("SC2_NO_RIBBON_TEAM_COLOR") == nullptr;
            int remis = 0;
            if (kRibTeam && inst.team[3] > 0.0f && inst.team[3] < 1.5f)
                if (const int rm = formats::ribbon_material(m, rb); rm >= 0)
                    for (const formats::M3Layer& L : m.materials[size_t(rm)].layers)
                        if (L.usage == "Diffuse" && !L.raw_record.empty() && L.word44 == 1) {
                            pemis_table.push_back({0.0f, 0.0f, 0.0f, 4.0f});
                            pemis_tex.push_back(-1);
                            remis = int(pemis_table.size()) - 1;
                            break;
                        }
            const float ruv_c = std::cos(ruvx[2]), ruv_s = std::sin(ruvx[2]);
            float w3[3] = {rb.width[0], rb.width[1], rb.width[2]};
            if (rb.width_flags && inst.seq >= 0)
                animated_layer_field(m, rseq, rb.width_interp, rb.width_flags, rb.width_id, inst.time_ms, 3, rb.width, w3, glayers(inst));
            static const bool kTubeHalf = std::getenv("SC2_RIBBON_TUBE_HALF_WIDTH") != nullptr;
            if (!kTubeHalf && rb.version == 8 && rb.sides > 1)
                for (float& w : w3) w *= 0.25f;
            const Vec3 A{bw[rb.bone].m[12], bw[rb.bone].m[13], bw[rb.bone].m[14]};
            const Mat4& eb = hang ? bw[rb.bone] : bw[size_t(rb.end_bone)];
            Vec3 B{eb.m[12], eb.m[13], eb.m[14]};
            Vec3 hang_side{0.0f, 0.0f, 0.0f};
            if (hang) {
                const Mat4& hb = bw[rb.bone];
                const float iscale = std::sqrt(inst.world.m[0] * inst.world.m[0] + inst.world.m[1] * inst.world.m[1] + inst.world.m[2] * inst.world.m[2]);
                B = Vec3{A.x, A.y, A.z - hang_len * iscale};
                hang_side = Vec3{hb.m[0], hb.m[1], hb.m[2]};
            }
            const Vec3 ab = sub(B, A);
            if (dot(ab, ab) < 1e-8f) continue;
            const int nseg = std::min(64, std::max(1, int(rb.divisions + 0.5f)));
            auto key = [](float a, float b, float c, float t) {
                return t < 0.5f ? a + (b - a) * (t * 2.0f) : b + (c - b) * ((t - 0.5f) * 2.0f);
            };
            static const bool kStraight = std::getenv("SC2_RIBBON_STRAIGHT") != nullptr;
            const bool spline = !kStraight && rb.version == 8 && rb.sides > 1;
            Vec3 T0 = ab, T1 = ab;
            if (spline) {
                static const float kTanScale = std::getenv("SC2_RIBBON_TANGENT_SCALE") ? float(std::atof(std::getenv("SC2_RIBBON_TANGENT_SCALE"))) : 1.5f;
                const float len = std::sqrt(dot(ab, ab)) * kTanScale;
                const Mat4& rbm = bw[rb.bone];
                const Vec3 z0 = normalize(Vec3{rbm.m[8], rbm.m[9], rbm.m[10]});
                const Vec3 z1 = normalize(Vec3{eb.m[8], eb.m[9], eb.m[10]});
                T0 = Vec3{z0.x * len, z0.y * len, z0.z * len};
                T1 = Vec3{-z1.x * len, -z1.y * len, -z1.z * len};
            }
            SceneRenderer::ParticleVertex prev[2];
            std::vector<SceneRenderer::ParticleVertex> tube_prev;
            for (int k = 0; k <= nseg; ++k) {
                const float t = float(k) / float(nseg);
                Vec3 P{A.x + ab.x * t, A.y + ab.y * t, A.z + ab.z * t};
                Vec3 along = ab;
                if (spline) {
                    const float t2 = t * t, t3 = t2 * t;
                    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f, h10 = t3 - 2.0f * t2 + t, h01 = -2.0f * t3 + 3.0f * t2, h11 = t3 - t2;
                    const float d00 = 6.0f * t2 - 6.0f * t, d10 = 3.0f * t2 - 4.0f * t + 1.0f, d01 = -6.0f * t2 + 6.0f * t, d11 = 3.0f * t2 - 2.0f * t;
                    P = Vec3{A.x * h00 + T0.x * h10 + B.x * h01 + T1.x * h11, A.y * h00 + T0.y * h10 + B.y * h01 + T1.y * h11,
                             A.z * h00 + T0.z * h10 + B.z * h01 + T1.z * h11};
                    along = Vec3{A.x * d00 + T0.x * d10 + B.x * d01 + T1.x * d11, A.y * d00 + T0.y * d10 + B.y * d01 + T1.y * d11,
                                 A.z * d00 + T0.z * d10 + B.z * d01 + T1.z * d11};
                }
                Vec3 side = cross(along, sub(cam.eye, P));
                if (dot(side, side) < 1e-10f) side = right;
                side = normalize(side);
                float hw = key(w3[0], w3[1], w3[2], t);
                if (hang) {
                    const float sl = std::sqrt(dot(hang_side, hang_side));
                    hw *= 0.25f * sl;
                }
                static const bool kFlatStrip = std::getenv("SC2_RIBBON_FLAT_STRIP") != nullptr;
                if (spline && !kFlatStrip) {
                    const int cols = std::max(2, int(rb.sides) / 2);
                    const Vec3 tocam = normalize(cross(side, along));
                    const Vec3 outw = dot(tocam, sub(cam.eye, P)) >= 0.0f ? tocam : Vec3{-tocam.x, -tocam.y, -tocam.z};
                    std::vector<SceneRenderer::ParticleVertex> ring(size_t(cols) + 1);
                    for (int j = 0; j <= cols; ++j) {
                        const float a = 3.1415927f * float(j) / float(cols);
                        const float ca = -std::cos(a), sa = std::sin(a);
                        const Vec3 n{side.x * ca + outw.x * sa, side.y * ca + outw.y * sa, side.z * ca + outw.z * sa};
                        float shade[3] = {1.0f, 1.0f, 1.0f};
                        const SceneRenderer::Tunables& lt = sr.tunables();
                        if (lt.rig.present) {
                            const RigLight* ls[3] = {&lt.rig.key, &lt.rig.fill, &lt.rig.back};
                            for (int c = 0; c < 3; ++c) shade[c] = lt.rig.ambient[c];
                            for (const RigLight* l : ls) {
                                if (!l->present) continue;
                                const float nl = std::max(0.0f, -(n.x * l->dir.x + n.y * l->dir.y + n.z * l->dir.z));
                                for (int c = 0; c < 3; ++c) shade[c] += l->color[c] * nl;
                            }
                        }
                        SceneRenderer::ParticleVertex& rv = ring[size_t(j)];
                        rv.pos[0] = P.x + side.x * hw * ca;
                        rv.pos[1] = P.y + side.y * hw * ca;
                        rv.pos[2] = P.z + side.z * hw * ca;
                        const float bu = 0.5f * float(j) / float(cols) * ruvx[0], bv = t * ruvx[1];
                        rv.uv[0] = bu * ruv_c - bv * ruv_s + manim[0];
                        rv.uv[1] = bu * ruv_s + bv * ruv_c + manim[1];
                        for (int ch = 0; ch < 3; ++ch)
                            rv.color[ch] = key(rb.color[0][ch], rb.color[1][ch], rb.color[2][ch], t) * manim[3] * shade[ch];
                        rv.color[3] = key(rb.color[0][3], rb.color[1][3], rb.color[2][3], t);
                    }
                    if (k > 0)
                        for (int j = 0; j < cols; ++j) {
                            PQuad pq;
                            pq.depth = dot(sub(P, cam.eye), fwd);
                            pq.v[0] = tube_prev[size_t(j)]; pq.v[1] = tube_prev[size_t(j) + 1]; pq.v[2] = ring[size_t(j) + 1]; pq.v[3] = ring[size_t(j)];
                            bytex[PKey{rblend, rtex, -1, 0, 1.0f, 0.0f, 0, -1, 0, 1.0f, 0.0f, 0.0f, remis}].push_back(pq);
                        }
                    tube_prev = ring;
                    continue;
                }
                SceneRenderer::ParticleVertex v[2];
                for (int e = 0; e < 2; ++e) {
                    const float sgn = e == 0 ? -1.0f : 1.0f;
                    v[e].pos[0] = P.x + side.x * hw * sgn;
                    v[e].pos[1] = P.y + side.y * hw * sgn;
                    v[e].pos[2] = P.z + side.z * hw * sgn;
                    const float bu = t * ruvx[0], bv = float(e) * ruvx[1];
                    v[e].uv[0] = bu * ruv_c - bv * ruv_s + manim[0];
                    v[e].uv[1] = bu * ruv_s + bv * ruv_c + manim[1];
                    for (int ch = 0; ch < 3; ++ch)
                        v[e].color[ch] = remis > 0 ? inst.team[size_t(ch)] : key(rb.color[0][ch], rb.color[1][ch], rb.color[2][ch], t) * manim[3];
                    v[e].color[3] = key(rb.color[0][3], rb.color[1][3], rb.color[2][3], t);
                }
                if (k > 0) {
                    PQuad pq;
                    const Vec3 mid{(prev[0].pos[0] + v[1].pos[0]) * 0.5f, (prev[0].pos[1] + v[1].pos[1]) * 0.5f,
                                   (prev[0].pos[2] + v[1].pos[2]) * 0.5f};
                    pq.depth = dot(sub(mid, cam.eye), fwd);
                    pq.v[0] = prev[0]; pq.v[1] = prev[1]; pq.v[2] = v[1]; pq.v[3] = v[0];
                    bytex[PKey{rblend, rtex, -1, 0, 1.0f, 0.0f, 0, -1, 0, 1.0f, 0.0f, 0.0f, remis}].push_back(pq);
                }
                prev[0] = v[0];
                prev[1] = v[1];
            }
        }
    }
    for (auto& kv : bytex) {
        if (kv.second.empty()) continue;
        const uint32_t blend = std::get<0>(kv.first);
        if (blend == formats::kBlendAlphaBlend || blend == formats::kBlendOpaque)
            std::stable_sort(kv.second.begin(), kv.second.end(),
                             [](const PQuad& a, const PQuad& b) { return a.depth > b.depth; });
        SceneRenderer::ParticleGroup g;
        g.tex = std::get<1>(kv.first);
        g.mask_tex = std::get<2>(kv.first);
        g.mask_code = float(std::get<3>(kv.first));
        g.mask_mul = std::get<4>(kv.first);
        g.mask_add = std::get<5>(kv.first);
        g.mask2_tex = std::get<7>(kv.first);
        g.mask2_code = float(std::get<8>(kv.first));
        g.mask2_mul = std::get<9>(kv.first);
        g.mask2_add = std::get<10>(kv.first);
        g.depth_blend = std::get<11>(kv.first);
        {
            const int ei = std::get<12>(kv.first);
            if (ei > 0 && std::size_t(ei) < pemis_table.size())
                for (int k = 0; k < 4; ++k) g.emis[k] = pemis_table[std::size_t(ei)][std::size_t(k)];
            if (ei > 0 && std::size_t(ei) < pemis_tex.size()) g.emis_tex = pemis_tex[std::size_t(ei)];
        }
        g.first = uint32_t(pverts.size());
        g.count = uint32_t(kv.second.size() * 6);
        g.blend = blend;
        g.glow = (std::get<6>(kv.first) & 4) != 0;
        g.mask_alone = (std::get<6>(kv.first) & 8) != 0;
        pgroups.push_back(g);
        const int fogged = std::get<6>(kv.first) & 3;
        const SceneRenderer::Tunables& tn = sr.tunables();
        for (const PQuad& pq : kv.second)
            for (int vi : {0, 1, 2, 0, 2, 3}) {
                SceneRenderer::ParticleVertex pv = pq.v[vi];
                if (const float fa = sr.fog_alpha_at(pv.pos[0], pv.pos[1]); fa > 0.0f) {
                    const float keep = 1.0f - fa;
                    pv.color[0] *= keep; pv.color[1] *= keep; pv.color[2] *= keep;
                    if (blend != formats::kBlendAlphaBlend && blend != formats::kBlendOpaque) pv.color[3] *= keep;
                }
                if (fogged && tn.height_fog) {
                    float f = 0.0f;
                    if (tn.gamma_space) {
                        const Vec3 vv{pv.pos[0] - cam.eye.x, pv.pos[1] - cam.eye.y, pv.pos[2] - cam.eye.z};
                        const float dist = std::sqrt(dot(vv, vv));
                        f = tn.fog_density * dist * std::exp((tn.fog_start - cam.eye.z) * tn.fog_falloff);
                        if (std::fabs(vv.z) > 0.01f) {
                            const float ft = std::min(std::max(tn.fog_falloff * vv.z, -80.0f), 80.0f);
                            f = ft == 0.0f ? 0.0f : f * (1.0f - std::exp(-ft)) / ft;
                        }
                    } else {
                        const float depth = std::max(tn.fog_start + tn.fog_falloff - pv.pos[2], 0.0f);
                        f = 1.0f - std::exp(-tn.fog_density * depth);
                    }
                    pv.fog[3] = std::min(std::max(f, 0.0f), 1.0f);
                    if (fogged == 1)
                        for (int k = 0; k < 3; ++k) pv.fog[k] = tn.fog_color[k];
                }
                pverts.push_back(pv);
            }
    }
}

}
