#include "sc2/render/scene.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/formats/dds.hpp"
#include "sc2/render/anim.hpp"
#include "sc2/render/vk_raii.hpp"
#include "mesh_build.hpp"

static const uint32_t kMeshVertSpv[] =
#include "mesh.vert.spv.h"
    ;
static const uint32_t kMeshFragSpv[] =
#include "mesh.frag.spv.h"
    ;
static const uint32_t kParticleVertSpv[] =
#include "particle.vert.spv.h"
    ;
static const uint32_t kParticleFragSpv[] =
#include "particle.frag.spv.h"
    ;

namespace sc2::render {

namespace {

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string("vulkan: ") + what + " failed (" +
                                 std::to_string(int(r)) + ")");
}

uint32_t find_memory(VkPhysicalDevice pd, uint32_t bits,
                     VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    throw std::runtime_error("vulkan: no suitable memory type");
}

struct Buffer {
    Unique<VkBuffer> buf;
    Unique<VkDeviceMemory> mem;
};

Buffer make_buffer(VkPhysicalDevice pd, VkDevice dev, VkDeviceSize size,
                   VkBufferUsageFlags usage, const void* src) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size ? size : 4;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer raw = VK_NULL_HANDLE;
    check(vkCreateBuffer(dev, &bi, nullptr, &raw), "vkCreateBuffer");
    Buffer out;
    out.buf = Unique<VkBuffer>(raw, [dev](VkBuffer b) { vkDestroyBuffer(dev, b, nullptr); });
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, raw, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(pd, req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory rawmem = VK_NULL_HANDLE;
    check(vkAllocateMemory(dev, &ai, nullptr, &rawmem), "vkAllocateMemory(buffer)");
    out.mem = Unique<VkDeviceMemory>(rawmem, [dev](VkDeviceMemory m) { vkFreeMemory(dev, m, nullptr); });
    check(vkBindBufferMemory(dev, raw, rawmem, 0), "vkBindBufferMemory");
    if (src && size) {
        void* p = nullptr;
        check(vkMapMemory(dev, rawmem, 0, size, 0, &p), "vkMapMemory");
        std::memcpy(p, src, size_t(size));
        vkUnmapMemory(dev, rawmem);
    }
    return out;
}

struct Image {
    Unique<VkImage> img;
    Unique<VkDeviceMemory> mem;
    Unique<VkImageView> view;
};

Image make_image(VkPhysicalDevice pd, VkDevice dev, VkFormat fmt,
                 VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t w,
                 uint32_t h) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage raw = VK_NULL_HANDLE;
    check(vkCreateImage(dev, &ii, nullptr, &raw), "vkCreateImage");
    Image out;
    out.img = Unique<VkImage>(raw, [dev](VkImage i) { vkDestroyImage(dev, i, nullptr); });
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, raw, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        find_memory(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory rawmem = VK_NULL_HANDLE;
    check(vkAllocateMemory(dev, &ai, nullptr, &rawmem), "vkAllocateMemory(image)");
    out.mem = Unique<VkDeviceMemory>(rawmem, [dev](VkDeviceMemory m) { vkFreeMemory(dev, m, nullptr); });
    check(vkBindImageMemory(dev, raw, rawmem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = raw;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView rawview = VK_NULL_HANDLE;
    check(vkCreateImageView(dev, &vi, nullptr, &rawview), "vkCreateImageView");
    out.view = Unique<VkImageView>(rawview, [dev](VkImageView v) { vkDestroyImageView(dev, v, nullptr); });
    return out;
}

Unique<VkShaderModule> make_shader(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule sm = VK_NULL_HANDLE;
    check(vkCreateShaderModule(dev, &ci, nullptr, &sm), "vkCreateShaderModule");
    return Unique<VkShaderModule>(sm, [dev](VkShaderModule s) { vkDestroyShaderModule(dev, s, nullptr); });
}

Vec3 mul_point(const Mat4& m, const Vec3& p) {
    return {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
            m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
            m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}

struct TexData {
    uint32_t w, h;
    std::vector<uint8_t> rgba;
};

enum TexSlot { kDiffuse = 0, kNormal = 1, kEmissive = 2, kNumSlots = 3 };

struct Mesh {
    Buffer vbuf;
    Buffer ibuf;
    std::vector<Draw> draws;
    std::vector<std::array<int, kNumSlots>> draw_tex;
    std::vector<VkDescriptorSet> draw_set;
};

struct PVertex {
    float pos[3];
    float uv[2];
    float color[4];
};

struct PGroup {
    int tex;
    uint32_t first;
    uint32_t count;
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

int particle_count(const formats::M3Particle& p) {
    if (p.emission_rate > 0.0f) {
        long r = std::lround(double(p.emission_rate) * double(p.lifetime));
        if (r < 1) r = 1;
        if (r > 64) r = 64;
        return int(r);
    }
    return 8;
}

}

bool project(const Mat4& mvp, float x, float y, float z, uint32_t width,
             uint32_t height, float& px, float& py) {
    const auto& m = mvp.m;
    float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
    float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
    float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (cw <= 1e-6f) return false;
    px = (cx / cw * 0.5f + 0.5f) * float(width);
    py = (cy / cw * 0.5f + 0.5f) * float(height);
    return true;
}

RenderResult render_scene(const std::vector<SceneInstance>& instances,
                          uint32_t width, uint32_t height, const Camera* cam_in,
                          const casc::Storage* storage,
                          const std::string& texture_root, bool enable_particles) {
    std::vector<const SceneInstance*> live;
    for (const auto& inst : instances)
        if (inst.model && inst.model->vertex_count && !inst.model->indices.empty())
            live.push_back(&inst);
    if (live.empty()) throw std::runtime_error("render_scene: no drawable instances");

    VkInstance rawInst = VK_NULL_HANDLE;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    check(vkCreateInstance(&ici, nullptr, &rawInst), "vkCreateInstance");
    Unique<VkInstance> instance(rawInst, [](VkInstance i) { vkDestroyInstance(i, nullptr); });

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(rawInst, &ndev, nullptr);
    if (ndev == 0) throw std::runtime_error("render_scene: no Vulkan physical device");
    std::vector<VkPhysicalDevice> pds(ndev);
    vkEnumeratePhysicalDevices(rawInst, &ndev, pds.data());
    VkPhysicalDevice pd = pds[0];

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(nqf);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfs.data());
    uint32_t gfx = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; ++i)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx = i; break; }
    if (gfx == UINT32_MAX) throw std::runtime_error("render_scene: no graphics queue");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = gfx;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    check(vkCreateDevice(pd, &dci, nullptr, &dev), "vkCreateDevice");
    Unique<VkDevice> device(dev, [](VkDevice d) { vkDestroyDevice(d, nullptr); });
    VkQueue queue;
    vkGetDeviceQueue(dev, gfx, 0, &queue);

    const VkFormat kColor = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat kDepth = VK_FORMAT_D32_SFLOAT;

    std::vector<TexData> texdata;
    texdata.push_back({1, 1, {255, 255, 255, 255}});
    texdata.push_back({1, 1, {128, 128, 255, 255}});
    texdata.push_back({1, 1, {0, 0, 0, 255}});
    const int kDefaultWhite = 0, kDefaultNormal = 1, kDefaultEmissive = 2;
    std::unordered_map<std::string, int> tex_index;
    auto resolve_texture = [&](const formats::Model& m, int material,
                               const char* usage, int def) -> int {
        std::string path = layer_of(m, material, usage);
        if (path.empty() || !storage) return def;
        auto it = tex_index.find(path);
        if (it != tex_index.end()) return it->second;
        int idx = def;
        std::vector<std::string> roots = {texture_root};
        for (const char* r : {"mods/liberty.sc2mod/base.sc2assets/",
                              "mods/core.sc2mod/base.sc2assets/",
                              "mods/void.sc2mod/base.sc2assets/",
                              "mods/swarm.sc2mod/base.sc2assets/", ""})
            roots.push_back(r);
        for (const std::string& root : roots) {
            try {
                auto bytes = storage->read_by_name(root + path);
                formats::DdsInfo info;
                auto rgba = formats::dds_decode_rgba(bytes, info);
                if (!rgba.empty() && info.width && info.height) {
                    idx = int(texdata.size());
                    texdata.push_back({info.width, info.height, std::move(rgba)});
                    break;
                }
            } catch (const std::exception&) {   }
        }
        tex_index[path] = idx;
        return idx;
    };

    std::unordered_map<const formats::Model*, size_t> mesh_of;
    std::vector<Mesh> meshes;
    Vec3 wmn{1e30f, 1e30f, 1e30f}, wmx{-1e30f, -1e30f, -1e30f};
    for (const SceneInstance* inst : live) {
        const formats::Model& m = *inst->model;
        if (!mesh_of.count(&m)) {
            Mesh mesh;
            std::vector<uint32_t> tri = resolve_indices(m, mesh.draws);
            if (tri.empty()) throw std::runtime_error("render_scene: no triangles");
            std::vector<Vertex> verts = build_vertices(m, tri);
            mesh.draw_tex.resize(mesh.draws.size());
            for (size_t i = 0; i < mesh.draws.size(); ++i) {
                int mat = mesh.draws[i].material;
                mesh.draw_tex[i] = {
                    resolve_texture(m, mat, "Diffuse", kDefaultWhite),
                    resolve_texture(m, mat, "Normal", kDefaultNormal),
                    resolve_texture(m, mat, "Emissive", kDefaultEmissive)};
            }
            mesh.vbuf = make_buffer(pd, dev, sizeof(Vertex) * verts.size(),
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, verts.data());
            mesh.ibuf = make_buffer(pd, dev, sizeof(uint32_t) * tri.size(),
                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT, tri.data());
            mesh_of[&m] = meshes.size();
            meshes.push_back(std::move(mesh));
        }
        Vec3 lmn{1e30f, 1e30f, 1e30f}, lmx{-1e30f, -1e30f, -1e30f};
        for (uint32_t i = 0; i < m.vertex_count; ++i)
            for (int a = 0; a < 3; ++a) {
                float c = m.positions[i * 3 + a];
                (&lmn.x)[a] = std::min((&lmn.x)[a], c);
                (&lmx.x)[a] = std::max((&lmx.x)[a], c);
            }
        for (int cx = 0; cx < 2; ++cx)
            for (int cy = 0; cy < 2; ++cy)
                for (int cz = 0; cz < 2; ++cz) {
                    Vec3 corner{cx ? lmx.x : lmn.x, cy ? lmx.y : lmn.y, cz ? lmx.z : lmn.z};
                    Vec3 w = mul_point(inst->world, corner);
                    for (int a = 0; a < 3; ++a) {
                        (&wmn.x)[a] = std::min((&wmn.x)[a], (&w.x)[a]);
                        (&wmx.x)[a] = std::max((&wmx.x)[a], (&w.x)[a]);
                    }
                }
    }

    Camera cam;
    if (cam_in) {
        cam = *cam_in;
    } else {
        Vec3 center{(wmn.x + wmx.x) * 0.5f, (wmn.y + wmx.y) * 0.5f, (wmn.z + wmx.z) * 0.5f};
        float radius = 0.5f * std::sqrt(dot(sub(wmx, wmn), sub(wmx, wmn)));
        if (radius < 1e-4f) radius = 1.0f;
        float dist = radius / std::tan(cam.fovy * 0.5f) * 1.5f;
        Vec3 dir = normalize({0.9f, -1.3f, 0.7f});
        cam.center = center;
        cam.eye = {center.x + dir.x * dist, center.y + dir.y * dist, center.z + dir.z * dist};
        cam.up = {0, 0, 1};
        cam.znear = dist * 0.02f;
        cam.zfar = dist + radius * 4.0f;
    }
    Mat4 view_proj = cam.view_proj(float(width) / float(height));

    std::vector<PVertex> pverts;
    std::vector<PGroup> pgroups;
    if (enable_particles) {
        Vec3 fwd = normalize(sub(cam.center, cam.eye));
        Vec3 right = normalize(cross(fwd, cam.up));
        Vec3 upc = cross(right, fwd);
        std::map<int, std::vector<PVertex>> bytex;
        uint32_t emitter_uid = 0;
        for (const SceneInstance* instp : live) {
            const SceneInstance& inst = *instp;
            const formats::Model& m = *inst.model;
            if (m.particles.empty()) continue;
            uint32_t pseq = inst.seq >= 0 ? uint32_t(inst.seq) : UINT32_MAX;
            float ptime = inst.seq >= 0 ? inst.time_ms : 0.0f;
            std::vector<Mat4> bw = pose_world(m, pseq, ptime);
            float tsec = inst.time_ms / 1000.0f;
            for (const formats::M3Particle& p : m.particles) {
                ++emitter_uid;
                if (p.lifetime <= 0.0f) continue;
                int ptex = kDefaultWhite;
                if (p.material_ref_index < m.material_refs.size())
                    ptex = resolve_texture(
                        m, int(m.material_refs[p.material_ref_index].index), "Diffuse",
                        kDefaultWhite);
                Mat4 mb = p.bone < bw.size() ? inst.world * bw[p.bone] : inst.world;
                Vec3 origin{mb.m[12], mb.m[13], mb.m[14]};
                Vec3 axis = normalize({mb.m[8], mb.m[9], mb.m[10]});
                if (dot(axis, axis) < 0.5f) axis = upc;
                Vec3 t1 = normalize(cross(axis, right));
                if (dot(t1, t1) < 0.5f) t1 = normalize(cross(axis, upc));
                Vec3 t2 = cross(axis, t1);
                int n = particle_count(p);
                for (int i = 0; i < n; ++i) {
                    float u1, u2, u3;
                    particle_rng(emitter_uid, i, u1, u2, u3);
                    float phase = u1 * p.lifetime;
                    float age = std::fmod(tsec + phase, p.lifetime);
                    if (age < 0.0f) age += p.lifetime;
                    float life01 = age / p.lifetime;
                    float ct = 1.0f - u2 * (1.0f - 0.9f);
                    float st = std::sqrt(std::max(0.0f, 1.0f - ct * ct));
                    float az = u3 * 6.2831853f;
                    Vec3 dir{axis.x * ct + (t1.x * std::cos(az) + t2.x * std::sin(az)) * st,
                             axis.y * ct + (t1.y * std::cos(az) + t2.y * std::sin(az)) * st,
                             axis.z * ct + (t1.z * std::cos(az) + t2.z * std::sin(az)) * st};
                    float travel = p.emission_speed * age;
                    Vec3 c{origin.x + dir.x * travel, origin.y + dir.y * travel,
                           origin.z + dir.z * travel};
                    float size = p.size_start + (p.size_end - p.size_start) * life01;
                    if (size <= 0.0f) continue;
                    float fade = std::sin(3.14159265f * life01);
                    if (fade <= 0.0f) continue;
                    const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                    PVertex q[4];
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
                    std::vector<PVertex>& out = bytex[ptex];
                    for (int vi : {0, 1, 2, 0, 2, 3}) out.push_back(q[vi]);
                }
            }
        }
        for (auto& kv : bytex) {
            if (kv.second.empty()) continue;
            pgroups.push_back({kv.first, uint32_t(pverts.size()),
                               uint32_t(kv.second.size())});
            pverts.insert(pverts.end(), kv.second.begin(), kv.second.end());
        }
    }
    const bool draw_particles = enable_particles && !pverts.empty();

    std::vector<Buffer> bonebufs(live.size());
    for (size_t i = 0; i < live.size(); ++i) {
        const SceneInstance& inst = *live[i];
        std::vector<Mat4> bone_mats;
        if (inst.seq >= 0 && !inst.model->bones.empty())
            bone_mats = pose(*inst.model, uint32_t(inst.seq), inst.time_ms);
        else
            for (size_t b = 0; b < std::max<size_t>(1, inst.model->bones.size()); ++b)
                bone_mats.push_back(Mat4::identity());
        if (bone_mats.empty()) bone_mats.push_back(Mat4::identity());
        std::vector<float> flat;
        flat.reserve(bone_mats.size() * 16);
        for (const Mat4& mm : bone_mats)
            for (int k = 0; k < 16; ++k) flat.push_back(mm.m[k]);
        bonebufs[i] = make_buffer(pd, dev, flat.size() * 4,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, flat.data());
    }

    Image color = make_image(pd, dev, kColor,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, width, height);
    Image depth = make_image(pd, dev, kDepth, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                             VK_IMAGE_ASPECT_DEPTH_BIT, width, height);

    std::vector<Image> textures;
    std::vector<Buffer> staging;
    for (const auto& td : texdata) {
        textures.push_back(make_image(pd, dev, kColor,
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      VK_IMAGE_ASPECT_COLOR_BIT, td.w, td.h));
        staging.push_back(make_buffer(pd, dev, td.rgba.size(),
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT, td.rgba.data()));
    }

    VkSamplerCreateInfo smi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smi.magFilter = smi.minFilter = VK_FILTER_LINEAR;
    smi.addressModeU = smi.addressModeV = smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    smi.maxLod = 1.0f;
    VkSampler rawSampler = VK_NULL_HANDLE;
    check(vkCreateSampler(dev, &smi, nullptr, &rawSampler), "vkCreateSampler");
    Unique<VkSampler> sampler(rawSampler, [dev](VkSampler s) { vkDestroySampler(dev, s, nullptr); });

    VkDescriptorSetLayoutBinding texb[kNumSlots]{};
    for (int s = 0; s < kNumSlots; ++s) {
        texb[s].binding = uint32_t(s);
        texb[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texb[s].descriptorCount = 1;
        texb[s].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo texli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    texli.bindingCount = kNumSlots;
    texli.pBindings = texb;
    VkDescriptorSetLayout rawTexLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev, &texli, nullptr, &rawTexLayout), "vkCreateDescriptorSetLayout");
    Unique<VkDescriptorSetLayout> texLayout(rawTexLayout, [dev](VkDescriptorSetLayout l) {
        vkDestroyDescriptorSetLayout(dev, l, nullptr); });

    VkDescriptorSetLayoutBinding boneb{};
    boneb.binding = 0;
    boneb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    boneb.descriptorCount = 1;
    boneb.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo boneli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    boneli.bindingCount = 1;
    boneli.pBindings = &boneb;
    VkDescriptorSetLayout rawBoneLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev, &boneli, nullptr, &rawBoneLayout), "vkCreateDescriptorSetLayout(bone)");
    Unique<VkDescriptorSetLayout> boneLayout(rawBoneLayout, [dev](VkDescriptorSetLayout l) {
        vkDestroyDescriptorSetLayout(dev, l, nullptr); });

    Unique<VkDescriptorSetLayout> partLayout;
    VkDescriptorSetLayout rawPartLayout = VK_NULL_HANDLE;
    if (draw_particles) {
        VkDescriptorSetLayoutBinding pb{};
        pb.binding = 0;
        pb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pb.descriptorCount = 1;
        pb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo pli2{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        pli2.bindingCount = 1;
        pli2.pBindings = &pb;
        check(vkCreateDescriptorSetLayout(dev, &pli2, nullptr, &rawPartLayout),
              "vkCreateDescriptorSetLayout(particle)");
        partLayout = Unique<VkDescriptorSetLayout>(rawPartLayout, [dev](VkDescriptorSetLayout l) {
            vkDestroyDescriptorSetLayout(dev, l, nullptr); });
    }

    uint32_t mat_sets = 0;
    for (const Mesh& mesh : meshes) mat_sets += uint32_t(mesh.draws.size());
    uint32_t part_sets = draw_particles ? uint32_t(pgroups.size()) : 0;
    VkDescriptorPoolSize psizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         kNumSlots * (mat_sets ? mat_sets : 1) + part_sets},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, uint32_t(live.size())}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = mat_sets + uint32_t(live.size()) + part_sets;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = psizes;
    VkDescriptorPool rawPool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(dev, &dpi, nullptr, &rawPool), "vkCreateDescriptorPool");
    Unique<VkDescriptorPool> dpool(rawPool, [dev](VkDescriptorPool p) {
        vkDestroyDescriptorPool(dev, p, nullptr); });

    for (Mesh& mesh : meshes) {
        mesh.draw_set.assign(mesh.draws.size(), VK_NULL_HANDLE);
        for (size_t d = 0; d < mesh.draws.size(); ++d) {
            VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dsa.descriptorPool = rawPool;
            dsa.descriptorSetCount = 1;
            dsa.pSetLayouts = &rawTexLayout;
            check(vkAllocateDescriptorSets(dev, &dsa, &mesh.draw_set[d]),
                  "vkAllocateDescriptorSets(material)");
            VkDescriptorImageInfo dii[kNumSlots];
            VkWriteDescriptorSet w[kNumSlots];
            for (int s = 0; s < kNumSlots; ++s) {
                dii[s] = {rawSampler, textures[mesh.draw_tex[d][s]].view.get(),
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                w[s] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w[s].dstSet = mesh.draw_set[d];
                w[s].dstBinding = uint32_t(s);
                w[s].descriptorCount = 1;
                w[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[s].pImageInfo = &dii[s];
            }
            vkUpdateDescriptorSets(dev, kNumSlots, w, 0, nullptr);
        }
    }
    std::vector<VkDescriptorSet> bone_set(live.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < live.size(); ++i) {
        VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsa.descriptorPool = rawPool;
        dsa.descriptorSetCount = 1;
        dsa.pSetLayouts = &rawBoneLayout;
        check(vkAllocateDescriptorSets(dev, &dsa, &bone_set[i]), "vkAllocateDescriptorSets(bone)");
        VkDescriptorBufferInfo bdi{bonebufs[i].buf.get(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet bw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        bw.dstSet = bone_set[i]; bw.dstBinding = 0; bw.descriptorCount = 1;
        bw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bw.pBufferInfo = &bdi;
        vkUpdateDescriptorSets(dev, 1, &bw, 0, nullptr);
    }

    std::vector<VkDescriptorSet> part_set;
    if (draw_particles) {
        part_set.assign(pgroups.size(), VK_NULL_HANDLE);
        for (size_t g = 0; g < pgroups.size(); ++g) {
            VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dsa.descriptorPool = rawPool;
            dsa.descriptorSetCount = 1;
            dsa.pSetLayouts = &rawPartLayout;
            check(vkAllocateDescriptorSets(dev, &dsa, &part_set[g]),
                  "vkAllocateDescriptorSets(particle)");
            VkDescriptorImageInfo dii{rawSampler, textures[pgroups[g].tex].view.get(),
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = part_set[g];
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &dii;
            vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        }
    }

    VkAttachmentDescription att[2]{};
    att[0].format = kColor; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    att[1].format = kDepth; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 2; rpi.pAttachments = att;
    rpi.subpassCount = 1; rpi.pSubpasses = &subp;
    VkRenderPass rawPass = VK_NULL_HANDLE;
    check(vkCreateRenderPass(dev, &rpi, nullptr, &rawPass), "vkCreateRenderPass");
    Unique<VkRenderPass> renderPass(rawPass, [dev](VkRenderPass p) { vkDestroyRenderPass(dev, p, nullptr); });

    VkImageView fbViews[2] = {color.view.get(), depth.view.get()};
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass = rawPass; fbi.attachmentCount = 2; fbi.pAttachments = fbViews;
    fbi.width = width; fbi.height = height; fbi.layers = 1;
    VkFramebuffer rawFb = VK_NULL_HANDLE;
    check(vkCreateFramebuffer(dev, &fbi, nullptr, &rawFb), "vkCreateFramebuffer");
    Unique<VkFramebuffer> framebuffer(rawFb, [dev](VkFramebuffer f) { vkDestroyFramebuffer(dev, f, nullptr); });

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80};
    VkDescriptorSetLayout set_layouts[2] = {rawTexLayout, rawBoneLayout};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = set_layouts;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VkPipelineLayout rawLayout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(dev, &pli, nullptr, &rawLayout), "vkCreatePipelineLayout");
    Unique<VkPipelineLayout> layout(rawLayout, [dev](VkPipelineLayout l) { vkDestroyPipelineLayout(dev, l, nullptr); });

    Unique<VkShaderModule> vs = make_shader(dev, kMeshVertSpv, sizeof(kMeshVertSpv));
    Unique<VkShaderModule> fs = make_shader(dev, kMeshFragSpv, sizeof(kMeshFragSpv));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs.get(); stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs.get(); stages[1].pName = "main";

    VkVertexInputBindingDescription vib{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription via[5] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
        {4, 0, VK_FORMAT_R32G32B32A32_UINT, 48}};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
    vin.vertexAttributeDescriptionCount = 5; vin.pVertexAttributeDescriptions = via;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, float(width), float(height), 0, 1};
    VkRect2D scis{{0, 0}, {width, height}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scis;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2; gpi.pStages = stages;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vps; gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms; gpi.pDepthStencilState = &ds; gpi.pColorBlendState = &cb;
    gpi.layout = rawLayout; gpi.renderPass = rawPass;
    VkPipeline rawPipeline = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &rawPipeline),
          "vkCreateGraphicsPipelines");
    Unique<VkPipeline> pipeline(rawPipeline, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });

    Unique<VkPipelineLayout> partPipeLayout;
    Unique<VkShaderModule> pvs, pfs;
    Unique<VkPipeline> partPipeline;
    Buffer pbuf;
    if (draw_particles) {
        pbuf = make_buffer(pd, dev, sizeof(PVertex) * pverts.size(),
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, pverts.data());

        VkPushConstantRange ppcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
        VkPipelineLayoutCreateInfo ppli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ppli.setLayoutCount = 1;
        ppli.pSetLayouts = &rawPartLayout;
        ppli.pushConstantRangeCount = 1;
        ppli.pPushConstantRanges = &ppcr;
        VkPipelineLayout rawPartPipeLayout = VK_NULL_HANDLE;
        check(vkCreatePipelineLayout(dev, &ppli, nullptr, &rawPartPipeLayout),
              "vkCreatePipelineLayout(particle)");
        partPipeLayout = Unique<VkPipelineLayout>(rawPartPipeLayout, [dev](VkPipelineLayout l) {
            vkDestroyPipelineLayout(dev, l, nullptr); });

        pvs = make_shader(dev, kParticleVertSpv, sizeof(kParticleVertSpv));
        pfs = make_shader(dev, kParticleFragSpv, sizeof(kParticleFragSpv));
        VkPipelineShaderStageCreateInfo pstages[2]{};
        pstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        pstages[0].module = pvs.get();
        pstages[0].pName = "main";
        pstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        pstages[1].module = pfs.get();
        pstages[1].pName = "main";

        VkVertexInputBindingDescription pvib{0, sizeof(PVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription pvia[3] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20}};
        VkPipelineVertexInputStateCreateInfo pvin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
        VkPipelineColorBlendStateCreateInfo pcb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        pcb.attachmentCount = 1;
        pcb.pAttachments = &pcba;
        VkPipelineDepthStencilStateCreateInfo pds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        pds.depthTestEnable = VK_TRUE;
        pds.depthWriteEnable = VK_FALSE;
        pds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkGraphicsPipelineCreateInfo pgpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pgpi.stageCount = 2;
        pgpi.pStages = pstages;
        pgpi.pVertexInputState = &pvin;
        pgpi.pInputAssemblyState = &ia;
        pgpi.pViewportState = &vps;
        pgpi.pRasterizationState = &rs;
        pgpi.pMultisampleState = &ms;
        pgpi.pDepthStencilState = &pds;
        pgpi.pColorBlendState = &pcb;
        pgpi.layout = rawPartPipeLayout;
        pgpi.renderPass = rawPass;
        VkPipeline rawPartPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pgpi, nullptr, &rawPartPipeline),
              "vkCreateGraphicsPipelines(particle)");
        partPipeline = Unique<VkPipeline>(rawPartPipeline, [dev](VkPipeline p) {
            vkDestroyPipeline(dev, p, nullptr); });
    }

    VkDeviceSize rbytes = VkDeviceSize(width) * height * 4;
    Buffer readback = make_buffer(pd, dev, rbytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, nullptr);

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = gfx;
    VkCommandPool rawCmdPool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(dev, &cpi, nullptr, &rawCmdPool), "vkCreateCommandPool");
    Unique<VkCommandPool> cmdPool(rawCmdPool, [dev](VkCommandPool p) { vkDestroyCommandPool(dev, p, nullptr); });
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = rawCmdPool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(dev, &cbi, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &bbi), "vkBeginCommandBuffer");

    for (size_t i = 0; i < textures.size(); ++i) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = textures[i].img.get();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy rgn{};
        rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rgn.imageExtent = {texdata[i].w, texdata[i].h, 1};
        vkCmdCopyBufferToImage(cmd, staging[i].buf.get(), textures[i].img.get(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    VkClearValue clears[2];
    clears[0].color = {{0.06f, 0.07f, 0.09f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = rawPass; rbi.framebuffer = rawFb;
    rbi.renderArea = {{0, 0}, {width, height}};
    rbi.clearValueCount = 2; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rawPipeline);

    for (size_t i = 0; i < live.size(); ++i) {
        const SceneInstance& inst = *live[i];
        const Mesh& mesh = meshes[mesh_of[inst.model]];
        Mat4 mvp = view_proj * inst.world;
        float pc[20];
        std::memcpy(pc, mvp.m.data(), 64);
        pc[16] = inst.team[0]; pc[17] = inst.team[1];
        pc[18] = inst.team[2]; pc[19] = inst.team[3];
        vkCmdPushConstants(cmd, rawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, 80, pc);
        VkDeviceSize off = 0;
        VkBuffer vb = mesh.vbuf.buf.get();
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
        vkCmdBindIndexBuffer(cmd, mesh.ibuf.buf.get(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rawLayout, 1, 1,
                                &bone_set[i], 0, nullptr);
        for (size_t d = 0; d < mesh.draws.size(); ++d) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rawLayout, 0, 1,
                                    &mesh.draw_set[d], 0, nullptr);
            vkCmdDrawIndexed(cmd, mesh.draws[d].index_count, 1, mesh.draws[d].first_index, 0, 0);
        }
    }

    if (draw_particles) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, partPipeline.get());
        float pmvp[16];
        std::memcpy(pmvp, view_proj.m.data(), 64);
        vkCmdPushConstants(cmd, partPipeLayout.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, 64, pmvp);
        VkDeviceSize poff = 0;
        VkBuffer pvb = pbuf.buf.get();
        vkCmdBindVertexBuffers(cmd, 0, 1, &pvb, &poff);
        for (size_t g = 0; g < pgroups.size(); ++g) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    partPipeLayout.get(), 0, 1, &part_set[g], 0, nullptr);
            vkCmdDraw(cmd, pgroups[g].count, 1, pgroups[g].first, 0);
        }
    }
    vkCmdEndRenderPass(cmd);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, color.img.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buf.get(), 1, &region);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence rawFence = VK_NULL_HANDLE;
    check(vkCreateFence(dev, &fi, nullptr, &rawFence), "vkCreateFence");
    Unique<VkFence> fence(rawFence, [dev](VkFence f) { vkDestroyFence(dev, f, nullptr); });
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    check(vkQueueSubmit(queue, 1, &si, rawFence), "vkQueueSubmit");
    check(vkWaitForFences(dev, 1, &rawFence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    RenderResult out;
    out.width = width; out.height = height; out.mvp = view_proj;
    out.rgba.resize(size_t(rbytes));
    void* mapped = nullptr;
    check(vkMapMemory(dev, readback.mem.get(), 0, rbytes, 0, &mapped), "vkMapMemory(readback)");
    std::memcpy(out.rgba.data(), mapped, size_t(rbytes));
    vkUnmapMemory(dev, readback.mem.get());
    return out;
}

}
