#include "sc2/render/scene_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

#include "sc2/casc/storage.hpp"
#include "sc2/data/environment.hpp"
#include "sc2/render/shadow_fit.hpp"
#include "sc2/formats/dds.hpp"
#include "sc2/render/anim.hpp"

static const uint32_t kSceneVertSpv[] =
#include "scene.vert.spv.h"
    ;
static const uint32_t kSceneFragSpv[] =
#include "scene.frag.spv.h"
    ;
static const uint32_t kShadowVertSpv[] =
#include "shadow.vert.spv.h"
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
static const uint32_t kTerrainSplatFragSpv[] =
#include "terrain_splat.frag.spv.h"
    ;

namespace sc2::render {

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
};
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
    float lightPos[16][4];
    float lightCol[16][4];
    float lightMeta[4];
};

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
        const Vec3 d{float(env.key_dir[0]), float(env.key_dir[1]), float(env.key_dir[2])};
        if (d.x * d.x + d.y * d.y + d.z * d.z > 1e-9f) tun.light_dir = normalize(d);
    }
    if (env.has_hdr && env.hdr_exposure > 0.0) tun.exposure = float(env.hdr_exposure);
    const LightRig r = rig_from_env(env);
    if (r.present) tun.rig = r;
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
    hdrColor_ = make_image(pd_, dev_, kHdr, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, width_, height_);
    ldrColor_ = make_image(pd_, dev_, ldr_format_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, width_, height_);
    depth_ = make_image(pd_, dev_, kDepthF, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, width_, height_);
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
    VkDescriptorImageInfo dii{sampler_.get(), hdrColor_.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = tm_set_;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
}

void SceneRenderer::resize(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || (w == width_ && h == height_)) return;
    vkDeviceWaitIdle(dev_);
    width_ = w;
    height_ = h;
    create_targets();
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
    }
}

void SceneRenderer::set_fog(const FogField& fog) {
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
    std::vector<uint8_t> rgba(std::size_t(fog.w) * fog.h * 4);
    for (std::size_t i = 0; i < fog.alpha.size(); ++i)
        rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4 + 3] = fog.alpha[i];
    upload_fog_image(fog.w, fog.h, rgba);
}

int SceneRenderer::upload_texture(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    if (w == 0 || h == 0 || w > 16384 || h > 16384 || rgba.size() < size_t(w) * h * 4)
        throw std::runtime_error("upload_texture: rgba bytes do not cover w*h*4");
    int idx = int(textures_.size());
    uint32_t mips = 1;
    for (uint32_t d = (w > h ? w : h); d > 1; d >>= 1) ++mips;
    Image img = make_image(pd_, dev_, kTexColor,
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
    if (const formats::M3Layer* layer = layer_ptr_of(m, material, usage)) {
        if (!layer->inline_rgba.empty() && layer->inline_w && layer->inline_h &&
            layer->inline_w <= 16384 && layer->inline_h <= 16384 &&
            layer->inline_rgba.size() >= size_t(layer->inline_w) * layer->inline_h * 4) {
            auto it = inline_index_.find(layer);
            if (it != inline_index_.end()) return it->second;
            int idx = upload_texture(layer->inline_w, layer->inline_h, layer->inline_rgba);
            inline_index_[layer] = idx;
            return idx;
        }
    }
    std::string path = layer_of(m, material, usage);
    if (path.empty() || !storage_) return def;
    auto it = tex_index_.find(path);
    if (it != tex_index_.end()) return it->second;
    int idx = def;
    std::vector<std::string> roots = {texture_root_};
    for (const char* r : {"mods/liberty.sc2mod/base.sc2assets/", "mods/core.sc2mod/base.sc2assets/",
                          "mods/void.sc2mod/base.sc2assets/", "mods/swarm.sc2mod/base.sc2assets/", ""})
        roots.push_back(r);
    for (const std::string& root : roots) {
        try {
            auto bytes = storage_->read_by_name(root + path);
            formats::DdsInfo info;
            auto rgba = formats::dds_decode_rgba(bytes, info);
            if (!rgba.empty() && info.width && info.height) {
                idx = upload_texture(info.width, info.height, rgba);
                break;
            }
        } catch (const std::exception&) {   }
    }
    tex_index_[path] = idx;
    return idx;
}

static void material_anim(const formats::Model& m, int mi, int seq, float time_ms, float out[4]) {
    out[0] = 0.0f; out[1] = 0.0f; out[2] = 1.0f; out[3] = 1.0f;
    if (mi < 0 || std::size_t(mi) >= m.materials.size()) return;
    const formats::M3Material& mat = m.materials[std::size_t(mi)];
    const uint32_t s = seq >= 0 ? uint32_t(seq) : UINT32_MAX;
    for (const formats::M3Layer& L : mat.layers) {
        const formats::M3Layer::AnimRefs& a = L.anim;
        if (L.usage == "Diffuse") {
            if (seq >= 0) {
                animated_layer_field(m, s, a.uv_offset_interp, a.uv_offset_flags, a.uv_offset_id, time_ms, 2, a.uv_offset, out);
                animated_layer_field(m, s, a.bright_mult_interp, a.bright_mult_flags, a.bright_mult_id, time_ms, 1, &a.bright_mult, out + 3);
            } else {
                out[0] = a.uv_offset[0]; out[1] = a.uv_offset[1]; out[3] = a.bright_mult;
            }
        } else if (L.usage == "Emissive") {
            if (seq >= 0)
                animated_layer_field(m, s, a.bright_mult_interp, a.bright_mult_flags, a.bright_mult_id, time_ms, 1, &a.bright_mult, out + 2);
            else
                out[2] = a.bright_mult;
        }
    }
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
                            resolve_texture(m, mat, "AlphaMask", kDefaultWhite)};
    }
    mesh.vbuf = make_buffer(pd_, dev_, sizeof(Vertex) * verts.size(),
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, verts.data());
    mesh.ibuf = make_buffer(pd_, dev_, sizeof(uint32_t) * tri.size(),
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, tri.data());
    mesh.draw_set.assign(mesh.draws.size(), VK_NULL_HANDLE);
    for (size_t d = 0; d < mesh.draws.size(); ++d) {
        if (!free_mat_sets_.empty()) {
            mesh.draw_set[d] = free_mat_sets_.back();
            free_mat_sets_.pop_back();
        } else {
            mesh.draw_set[d] = alloc_set(matPool_, matPoolsExtra_, texLayout_.get(),
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5, 512, "material");
        }
        VkDescriptorImageInfo dii[5];
        VkWriteDescriptorSet w[5];
        for (int s = 0; s < 5; ++s) {
            dii[s] = {sampler_.get(), textures_[mesh.draw_tex[d][s]].view.get(),
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w[s] = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            w[s].dstSet = mesh.draw_set[d];
            w[s].dstBinding = uint32_t(s);
            w[s].descriptorCount = 1;
            w[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[s].pImageInfo = &dii[s];
        }
        vkUpdateDescriptorSets(dev_, 5, w, 0, nullptr);
    }
    return mesh_cache_.emplace(&m, std::move(mesh)).first->second;
}

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
                            const uint8_t* bytes, size_t len) -> Image {
        Image img = make_image_array(pd_, dev_, fmt,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     w, h, kLayers);
        Buffer stg = make_buffer(pd_, dev_, len, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 const_cast<uint8_t*>(bytes));
        one_time([&](VkCommandBuffer cb) {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.image = img.img.get();
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kLayers};
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy rgn{};
            rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kLayers};
            rgn.imageExtent = {w, h, 1};
            vkCmdCopyBufferToImage(cb, stg.buf.get(), img.img.get(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
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
    ts.diffuse_arr = upload_array(kTexColor, tw, th, 4, diff.data(), diff.size());
    ts.weight_arr = upload_array(VK_FORMAT_R8_UNORM, m.splat_w, m.splat_h, 1,
                                 m.splat_weights.data(),
                                 size_t(m.splat_w) * m.splat_h * kLayers);

    VkDescriptorSetAllocateInfo dsa{};

    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool = splatPool_.get();
    dsa.descriptorSetCount = 1;
    VkDescriptorSetLayout sl = splatLayout_.get();
    dsa.pSetLayouts = &sl;
    check(vkAllocateDescriptorSets(dev_, &dsa, &ts.set), "vkAllocateDescriptorSets(splat)");
    VkDescriptorImageInfo dii[2] = {
        {sampler_.get(), ts.diffuse_arr.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler_.get(), ts.weight_arr.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2];
    for (int s = 0; s < 2; ++s) {
        w[s] = vk_init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        w[s].dstSet = ts.set;
        w[s].dstBinding = uint32_t(s);
        w[s].descriptorCount = 1;
        w[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[s].pImageInfo = &dii[s];
    }
    vkUpdateDescriptorSets(dev_, 2, w, 0, nullptr);
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
    ssmi.magFilter = ssmi.minFilter = VK_FILTER_NEAREST;
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

    VkDescriptorSetLayoutBinding texb[5]{};
    for (int s = 0; s < 5; ++s) {
        texb[s].binding = uint32_t(s);
        texb[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texb[s].descriptorCount = 1;
        texb[s].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo texli{};
    texli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    texli.bindingCount = 5;
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

    VkDescriptorSetLayoutBinding gb[3]{};
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
    VkDescriptorSetLayoutCreateInfo gli{};
    gli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    gli.bindingCount = 3;
    gli.pBindings = gb;
    VkDescriptorSetLayout rawGlobalLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev_, &gli, nullptr, &rawGlobalLayout), "vkCreateDescriptorSetLayout(global)");
    globalLayout_ = Unique<VkDescriptorSetLayout>(rawGlobalLayout, [dev](VkDescriptorSetLayout l) { vkDestroyDescriptorSetLayout(dev, l, nullptr); });

    VkDescriptorSetLayoutBinding pb{};
    pb.binding = 0;
    pb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pb.descriptorCount = 1;
    pb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo pli2{};
    pli2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    pli2.bindingCount = 1;
    pli2.pBindings = &pb;
    VkDescriptorSetLayout rawPartLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev_, &pli2, nullptr, &rawPartLayout), "vkCreateDescriptorSetLayout(particle)");
    partLayout_ = Unique<VkDescriptorSetLayout>(rawPartLayout, [dev](VkDescriptorSetLayout l) { vkDestroyDescriptorSetLayout(dev, l, nullptr); });

    VkDescriptorSetLayoutBinding tmb{};
    tmb.binding = 0;
    tmb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    tmb.descriptorCount = 1;
    tmb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo tmli{};
    tmli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    tmli.bindingCount = 1;
    tmli.pBindings = &tmb;
    VkDescriptorSetLayout rawTmLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev_, &tmli, nullptr, &rawTmLayout), "vkCreateDescriptorSetLayout(tonemap)");
    tmLayout_ = Unique<VkDescriptorSetLayout>(rawTmLayout, [dev](VkDescriptorSetLayout l) { vkDestroyDescriptorSetLayout(dev, l, nullptr); });

    shadowMap_ = make_image(pd_, dev_, kDepthF,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT, tun_.shadow_dim, tun_.shadow_dim);

    GlobalsUBO zero{};
    globalsBuf_ = make_buffer(pd_, dev_, sizeof(GlobalsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &zero);

    VkDescriptorPoolSize gpsizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo gdpi{};
    gdpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    gdpi.maxSets = 1;
    gdpi.poolSizeCount = 2;
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

    VkDescriptorPoolSize partSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxPartSets};
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
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 112};
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
    VkVertexInputAttributeDescription via[5] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
                                                {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
                                                {4, 0, VK_FORMAT_R32G32B32A32_UINT, 48}};
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &vib;
    vin.vertexAttributeDescriptionCount = 5;
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
    }

    {
        VkDescriptorSetLayoutBinding sb[2]{};
        for (int s = 0; s < 2; ++s) {
            sb[s].binding = uint32_t(s);
            sb[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            sb[s].descriptorCount = 1;
            sb[s].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo sli{};
        sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sli.bindingCount = 2;
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

        VkDescriptorPoolSize sp{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
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
    }

    {
        VkPushConstantRange ppcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
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
        VkVertexInputAttributeDescription pvia[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                     {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
                                                     {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20}};
        VkPipelineVertexInputStateCreateInfo pvin{};
        pvin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        pvin.vertexBindingDescriptionCount = 1;
        pvin.pVertexBindingDescriptions = &pvib;
        pvin.vertexAttributeDescriptionCount = 3;
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
        VkPipeline rawPartPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &pgpi, nullptr, &rawPartPipeline), "vkCreateGraphicsPipelines(particle)");
        partPipeline_ = Unique<VkPipeline>(rawPartPipeline, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });
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
        VkDescriptorPoolSize tmpsize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
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
    }
}

void SceneRenderer::prepare(const std::vector<const SceneInstance*>& live) {
    frame_lights_.clear();
    for (const SceneInstance* instp : live) {
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
                intensity = animated_scalar(m, seq, ref, inst.time_ms, L.intensity);
            }
            if (intensity <= 0.0f || L.attenuation_far <= 0.0f) continue;
            if (!posed) { bw = pose_world(m, seq, inst.seq >= 0 ? inst.time_ms : 0.0f); posed = true; }
            const Mat4 mb = (L.bone >= 0 && size_t(L.bone) < bw.size()) ? inst.world * bw[size_t(L.bone)] : inst.world;
            FrameLight f;
            f.pos = {mb.m[12], mb.m[13], mb.m[14]};
            f.col = {L.color[0] * intensity, L.color[1] * intensity, L.color[2] * intensity};
            f.att_near = L.attenuation_near;
            f.att_far = L.attenuation_far;
            frame_lights_.push_back(f);
        }
    }
    Vec3 wmn{1e30f, 1e30f, 1e30f}, wmx{-1e30f, -1e30f, -1e30f};
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
            const std::vector<Mat4> bw = pose_world(m, uint32_t(inst->seq), inst->time_ms);
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
        for (int a = 0; a < 3; ++a) {
            (&wmn.x)[a] = std::min((&wmn.x)[a], (&imn.x)[a]);
            (&wmx.x)[a] = std::max((&wmx.x)[a], (&imx.x)[a]);
        }
    }
    if (wmn.x > wmx.x) { wmn = {0, 0, 0}; wmx = {0, 0, 0}; }
    aabb_min_ = wmn;
    aabb_max_ = wmx;
}

void SceneRenderer::record(VkCommandBuffer cmd, const std::vector<const SceneInstance*>& live,
                           const Camera& cam, const std::vector<ParticleVertex>& pverts,
                           const std::vector<ParticleGroup>& pgroups) {
    Mat4 view_proj = cam.view_proj(float(width_) / float(height_));

    const ShadowBox sbox =
        fit_shadow_ortho(aabb_min_, aabb_max_, cam, float(width_) / float(height_), tun_.light_dir,
                         tun_.fit_pad, tun_.shadow_dim, tun_.shadow_clip, tun_.camera_fit_shadow);
    Mat4 lview = look_at(sbox.eye, sbox.center, sbox.up);
    Mat4 lproj = ortho(sbox.xmn, sbox.xmx, sbox.ymn, sbox.ymx, sbox.znear, sbox.zfar);
    Mat4 lightVP = lproj * lview;

    GlobalsUBO globals{};
    std::memcpy(globals.viewProj, view_proj.m.data(), 64);
    std::memcpy(globals.lightVP, lightVP.m.data(), 64);
    globals.shadowParams[0] = 1.0f / float(tun_.shadow_dim);
    globals.shadowParams[1] = tun_.depth_bias;
    globals.shadowParams[2] = tun_.shadow_strength;
    globals.shadowParams[3] = tun_.gloss;
    globals.eyePos[0] = cam.eye.x;
    globals.eyePos[1] = cam.eye.y;
    globals.eyePos[2] = cam.eye.z;
    globals.eyePos[3] = tun_.spec_strength;
    globals.lightDir[0] = tun_.light_dir.x;
    globals.lightDir[1] = tun_.light_dir.y;
    globals.lightDir[2] = tun_.light_dir.z;
    globals.lightDir[3] = 0.0f;
    globals.rigAmbient[0] = tun_.rig.ambient[0];
    globals.rigAmbient[1] = tun_.rig.ambient[1];
    globals.rigAmbient[2] = tun_.rig.ambient[2];
    globals.rigAmbient[3] = tun_.rig.present ? 1.0f : 0.0f;
    {
        const RigLight* ls[3] = {&tun_.rig.key, &tun_.rig.fill, &tun_.rig.back};
        for (int i = 0; i < 3; ++i) {
            globals.rigDir[i][0] = ls[i]->dir.x;
            globals.rigDir[i][1] = ls[i]->dir.y;
            globals.rigDir[i][2] = ls[i]->dir.z;
            globals.rigDir[i][3] = ls[i]->present ? 1.0f : 0.0f;
            for (int c = 0; c < 3; ++c) globals.rigColor[i][c] = ls[i]->color[c];
            globals.rigColor[i][3] = 0.0f;
        }
        globals.rigParams[0] = tun_.rig_shadow_floor;
    }
    for (const SceneInstance* p : live)
        if (p->model && p->model->has_splat_payload()) {
            if (TerrainSplat* ts = ensure_splat(*p->model)) {
                globals.terrainExtent[0] = ts->extent[0];
                globals.terrainExtent[1] = ts->extent[1];
            }
            break;
        }
    globals.fogExtent[0] = fog_world_w_;
    globals.fogExtent[1] = fog_world_h_;
    globals.fogExtent[2] = fog_present_ ? 1.0f : 0.0f;
    globals.fogExtent[3] = 0.0f;
    {
        std::vector<FrameLight> fl = frame_lights_;
        std::sort(fl.begin(), fl.end(), [&](const FrameLight& a, const FrameLight& b) {
            const float da = (a.pos.x - cam.center.x) * (a.pos.x - cam.center.x) + (a.pos.y - cam.center.y) * (a.pos.y - cam.center.y) + (a.pos.z - cam.center.z) * (a.pos.z - cam.center.z);
            const float db = (b.pos.x - cam.center.x) * (b.pos.x - cam.center.x) + (b.pos.y - cam.center.y) * (b.pos.y - cam.center.y) + (b.pos.z - cam.center.z) * (b.pos.z - cam.center.z);
            return da < db;
        });
        const size_t n = std::min<size_t>(fl.size(), 16);
        for (size_t i = 0; i < n; ++i) {
            globals.lightPos[i][0] = fl[i].pos.x; globals.lightPos[i][1] = fl[i].pos.y; globals.lightPos[i][2] = fl[i].pos.z; globals.lightPos[i][3] = fl[i].att_far;
            globals.lightCol[i][0] = fl[i].col.x; globals.lightCol[i][1] = fl[i].col.y; globals.lightCol[i][2] = fl[i].col.z; globals.lightCol[i][3] = fl[i].att_near;
        }
        globals.lightMeta[0] = float(n);
        globals.lightMeta[1] = globals.lightMeta[2] = globals.lightMeta[3] = 0.0f;
        lights_drawn_ = n;
        lights_seen_ = frame_lights_.size();
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
    for (size_t i = 0; i < live.size(); ++i) {
        const SceneInstance& inst = *live[i];
        if (!(inst.seq >= 0 && !inst.model->bones.empty())) { bone_set[i] = shared_static; continue; }
        std::vector<Mat4> bone_mats = pose(*inst.model, uint32_t(inst.seq), inst.time_ms);
        if (bone_mats.empty()) bone_mats.push_back(Mat4::identity());
        std::vector<float> flat;
        flat.reserve(bone_mats.size() * 16);
        for (const Mat4& mm : bone_mats)
            for (int k = 0; k < 16; ++k) flat.push_back(mm.m[k]);
        bone_set[i] = alloc_bone_set(flat);
    }

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
                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, 256, "particle");
            int ti = pgroups[g].tex;
            if (ti < 0 || ti >= int(textures_.size())) ti = kDefaultWhite;
            VkDescriptorImageInfo dii{sampler_.get(), textures_[ti].view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = part_set[g];
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &dii;
            vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
        }
    }

    VkClearValue sclear;
    sclear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo srbi{};
    srbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    srbi.renderPass = shadowPass_.get();
    srbi.framebuffer = shadowFb_.get();
    srbi.renderArea = {{0, 0}, {tun_.shadow_dim, tun_.shadow_dim}};
    srbi.clearValueCount = 1;
    srbi.pClearValues = &sclear;
    vkCmdBeginRenderPass(cmd, &srbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 2, 1, &global_set_, 0, nullptr);
    for (size_t i = 0; i < live.size(); ++i) {
        const SceneInstance& inst = *live[i];
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
            if (mi >= 0 && std::size_t(mi) < inst.model->materials.size() &&
                formats::material_is_additive(inst.model->materials[mi]))
                continue;
            vkCmdDrawIndexed(cmd, mesh.draws[d].index_count, 1, mesh.draws[d].first_index, 0, 0);
        }
    }
    vkCmdEndRenderPass(cmd);

    VkClearValue clears[2];
    clears[0].color = {{0.06f, 0.07f, 0.09f, cam.transparent_clear ? 0.0f : 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = litPass_.get();
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
            float mpc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (mi >= 0 && std::size_t(mi) < inst.model->materials.size())
                mpc[0] = inst.model->materials[mi].specularity;
            if (mi >= 0 && std::size_t(mi) < inst.model->materials.size() &&
                (formats::material_is_additive(inst.model->materials[mi]) ||
                 (formats::material_is_alpha_blended(inst.model->materials[mi]) && !inst.model->cliff_skin_opaque))) {
                const float dx = inst.world.m[12] - cam.eye.x, dy = inst.world.m[13] - cam.eye.y,
                            dz = inst.world.m[14] - cam.eye.z;
                blended.push_back(BlendedDraw{i, d, dx * dx + dy * dy + dz * dz});
                continue;
            }
            vkCmdPushConstants(cmd, layout_.get(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 80, 16, mpc);
            float apc[4];
            material_anim(*inst.model, mi, inst.seq, inst.time_ms, apc);
            vkCmdPushConstants(cmd, layout_.get(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 96, 16, apc);
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
        for (size_t d = 0; d < mesh.draws.size(); ++d)
            vkCmdDrawIndexed(cmd, mesh.draws[d].index_count, 1, mesh.draws[d].first_index, 0, 0);
    }
    if (!blended.empty()) {
        std::stable_sort(blended.begin(), blended.end(),
                         [](const BlendedDraw& a, const BlendedDraw& b) { return a.dist2 > b.dist2; });
        VkPipeline bound = VK_NULL_HANDLE;
        for (const BlendedDraw& bd : blended) {
            const SceneInstance& inst = *live[bd.inst];
            Mesh& mesh = ensure_mesh(*inst.model, inst.mesh_version);
            const int mi = mesh.draws[bd.draw].material;
            const formats::M3Material& mat = inst.model->materials[std::size_t(mi)];
            const VkPipeline want = mat.blend_mode == formats::kBlendAlphaAdd    ? alphaAddPipeline_.get()
                                    : mat.blend_mode == formats::kBlendAlphaBlend ? alphaBlendPipeline_.get()
                                                                                  : addPipeline_.get();
            if (want != bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 2, 1, &global_set_, 0, nullptr);
                bound = want;
            }
            float pc[20];
            std::memcpy(pc, inst.world.m.data(), 64);
            pc[16] = inst.team[0]; pc[17] = inst.team[1];
            pc[18] = inst.team[2]; pc[19] = inst.team[3];
            vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80, pc);
            float mpc[4] = {mat.specularity, float(mat.blend_mode), float(formats::material_alpha_mask_channel(mat)), 0.0f};
            vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 80, 16, mpc);
            float apc[4];
            material_anim(*inst.model, mi, inst.seq, inst.time_ms, apc);
            vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 96, 16, apc);
            VkDeviceSize off = 0;
            VkBuffer vb = mesh.vbuf.buf.get();
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
            vkCmdBindIndexBuffer(cmd, mesh.ibuf.buf.get(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 1, 1, &bone_set[bd.inst], 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &mesh.draw_set[bd.draw], 0, nullptr);
            vkCmdDrawIndexed(cmd, mesh.draws[bd.draw].index_count, 1, mesh.draws[bd.draw].first_index, 0, 0);
        }
    }
    if (draw_particles) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, partPipeline_.get());
        float pmvp[16];
        std::memcpy(pmvp, view_proj.m.data(), 64);
        vkCmdPushConstants(cmd, partPipeLayout_.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, 64, pmvp);
        VkDeviceSize poff = 0;
        VkBuffer pvb = partbuf_.buf.get();
        vkCmdBindVertexBuffers(cmd, 0, 1, &pvb, &poff);
        for (size_t g = 0; g < pgroups.size(); ++g) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, partPipeLayout_.get(), 0, 1, &part_set[g], 0, nullptr);
            vkCmdDraw(cmd, pgroups[g].count, 1, pgroups[g].first, 0);
        }
    }
    vkCmdEndRenderPass(cmd);

    TonemapPC tm{tun_.exposure, tun_.curve, cam.transparent_clear ? 1 : 0};
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

void build_particles(SceneRenderer& sr, const std::vector<const SceneInstance*>& live,
                     const Camera& cam,
                     std::vector<SceneRenderer::ParticleVertex>& pverts,
                     std::vector<SceneRenderer::ParticleGroup>& pgroups) {
    Vec3 fwd = normalize(sub(cam.center, cam.eye));
    Vec3 right = normalize(cross(fwd, cam.up));
    Vec3 upc = cross(right, fwd);
    std::map<int, std::vector<SceneRenderer::ParticleVertex>> bytex;
    uint32_t emitter_uid = 0;
    for (const SceneInstance* instp : live) {
        const SceneInstance& inst = *instp;
        const formats::Model& m = *inst.model;
        if (m.particles.empty()) continue;
        uint32_t pseq = inst.seq >= 0 ? uint32_t(inst.seq) : UINT32_MAX;
        float ptime = inst.seq >= 0 ? inst.time_ms : 0.0f;
        std::vector<Mat4> bw = pose_world(m, pseq, ptime);
        float tsec = inst.time_ms / 1000.0f;
        for (const formats::M3Particle& pe : m.particles) {
            ++emitter_uid;
            if (pe.lifetime <= 0.0f) continue;
            int ptex = sr.default_white();
            if (const int pm = formats::particle_material(m, pe); pm >= 0)
                ptex = sr.resolve_texture(m, pm, "Diffuse", sr.default_white());
            Mat4 mb = pe.bone < bw.size() ? inst.world * bw[pe.bone] : inst.world;
            Vec3 origin{mb.m[12], mb.m[13], mb.m[14]};
            Vec3 axis = normalize({mb.m[8], mb.m[9], mb.m[10]});
            if (dot(axis, axis) < 0.5f) axis = upc;
            Vec3 t1 = normalize(cross(axis, right));
            if (dot(t1, t1) < 0.5f) t1 = normalize(cross(axis, upc));
            Vec3 t2 = cross(axis, t1);
            formats::M3AnimRef rate_ref;
            rate_ref.interp = pe.rate_interp;
            rate_ref.anim_flags = pe.rate_anim_flags;
            rate_ref.anim_id = pe.rate_anim_id;
            rate_ref.init[0] = pe.emission_rate;
            const float live_rate =
                animated_scalar(m, pseq, rate_ref, inst.time_ms, pe.emission_rate);
            int n = formats::particle_count_at_rate(pe, live_rate);
            if (n <= 0) continue;
            for (int i = 0; i < n; ++i) {
                float u1, u2, u3;
                particle_rng(emitter_uid, i, u1, u2, u3);
                float phase = u1 * pe.lifetime;
                float age = std::fmod(tsec + phase, pe.lifetime);
                if (age < 0.0f) age += pe.lifetime;
                float life01 = age / pe.lifetime;
                float ct = 1.0f - u2 * (1.0f - 0.9f);
                float st = std::sqrt(std::max(0.0f, 1.0f - ct * ct));
                float az = u3 * 6.2831853f;
                Vec3 dir{axis.x * ct + (t1.x * std::cos(az) + t2.x * std::sin(az)) * st,
                         axis.y * ct + (t1.y * std::cos(az) + t2.y * std::sin(az)) * st,
                         axis.z * ct + (t1.z * std::cos(az) + t2.z * std::sin(az)) * st};
                float travel = pe.emission_speed * age;
                Vec3 c{origin.x + dir.x * travel, origin.y + dir.y * travel,
                       origin.z + dir.z * travel};
                float size = pe.size_start + (pe.size_end - pe.size_start) * life01;
                if (size <= 0.0f) continue;
                float fade = std::sin(3.14159265f * life01);
                if (fade <= 0.0f) continue;
                const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                SceneRenderer::ParticleVertex q[4];
                for (int k = 0; k < 4; ++k) {
                    float ox = corner[k][0] * size, oy = corner[k][1] * size;
                    q[k].pos[0] = c.x + right.x * ox + upc.x * oy;
                    q[k].pos[1] = c.y + right.y * ox + upc.y * oy;
                    q[k].pos[2] = c.z + right.z * ox + upc.z * oy;
                    q[k].uv[0] = (corner[k][0] + 1.0f) * 0.5f;
                    q[k].uv[1] = (corner[k][1] + 1.0f) * 0.5f;
                    q[k].color[0] = q[k].color[1] = q[k].color[2] = 1.0f;
                    q[k].color[3] = fade;
                }
                std::vector<SceneRenderer::ParticleVertex>& out = bytex[ptex];
                for (int vi : {0, 1, 2, 0, 2, 3}) out.push_back(q[vi]);
            }
        }
    }
    for (auto& kv : bytex) {
        if (kv.second.empty()) continue;
        pgroups.push_back({kv.first, uint32_t(pverts.size()), uint32_t(kv.second.size())});
        pverts.insert(pverts.end(), kv.second.begin(), kv.second.end());
    }
}

}
