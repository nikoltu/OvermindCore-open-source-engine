#include "sc2/render/window.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/formats/dds.hpp"
#include "sc2/render/anim.hpp"
#include "sc2/render/vk_helpers.hpp"
#include "sc2/render/vk_raii.hpp"
#include "mesh_build.hpp"

static const uint32_t kMeshVertSpv[] =
#include "mesh.vert.spv.h"
    ;
static const uint32_t kMeshFragSpv[] =
#include "mesh.frag.spv.h"
    ;

namespace sc2::render {

namespace {

[[noreturn]] void unavailable(const char* what) {
    throw WindowUnavailable(std::string("render_window: ") + what);
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

bool has_layer(const std::vector<VkLayerProperties>& layers, const char* name) {
    for (const auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

bool has_ext(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (user) ++*static_cast<uint32_t*>(user);
        std::fprintf(stderr, "[vk-validation] %s\n",
                     data && data->pMessage ? data->pMessage : "(no message)");
    }
    return VK_FALSE;
}

const wchar_t* window_class(HINSTANCE hinst) {
    static const wchar_t* kName = L"sc2_render_window";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            if (msg == WM_CLOSE) { PostQuitMessage(0); return 0; }
            return DefWindowProcW(h, msg, wp, lp);
        };
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            unavailable("RegisterClassEx failed");
        registered = true;
    }
    return kName;
}

}

WindowResult render_window(const std::vector<SceneInstance>& instances,
                           const Camera* cam_in, const casc::Storage* storage,
                           uint32_t width, uint32_t height, uint32_t frames,
                           const std::string& texture_root) {
    std::vector<const SceneInstance*> live;
    for (const auto& inst : instances)
        if (inst.model && inst.model->vertex_count && !inst.model->indices.empty())
            live.push_back(&inst);
    if (live.empty()) throw std::runtime_error("render_window: no drawable instances");

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    const wchar_t* cls = window_class(hinst);
    RECT rc{0, 0, LONG(width), LONG(height)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND rawWin = CreateWindowExW(0, cls, L"sc2 render", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                                  rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);
    if (!rawWin) unavailable("CreateWindowEx failed");
    Unique<HWND> window(rawWin, [](HWND h) { DestroyWindow(h); });
    ShowWindow(rawWin, SW_SHOWNA);

    uint32_t nlayers = 0;
    vkEnumerateInstanceLayerProperties(&nlayers, nullptr);
    std::vector<VkLayerProperties> layers(nlayers);
    if (nlayers) vkEnumerateInstanceLayerProperties(&nlayers, layers.data());
    uint32_t niexts = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &niexts, nullptr);
    std::vector<VkExtensionProperties> iexts(niexts);
    if (niexts) vkEnumerateInstanceExtensionProperties(nullptr, &niexts, iexts.data());

    if (!has_ext(iexts, VK_KHR_SURFACE_EXTENSION_NAME) ||
        !has_ext(iexts, VK_KHR_WIN32_SURFACE_EXTENSION_NAME))
        unavailable("win32 surface instance extensions unavailable");

    std::vector<const char*> want_ext = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    std::vector<const char*> want_layer;
    const bool validation =
        has_layer(layers, "VK_LAYER_KHRONOS_validation") &&
        has_ext(iexts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validation) {
        want_layer.push_back("VK_LAYER_KHRONOS_validation");
        want_ext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(want_ext.size());
    ici.ppEnabledExtensionNames = want_ext.data();
    ici.enabledLayerCount = uint32_t(want_layer.size());
    ici.ppEnabledLayerNames = want_layer.empty() ? nullptr : want_layer.data();
    VkInstance rawInst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &rawInst) != VK_SUCCESS)
        unavailable("vkCreateInstance (WSI) failed");
    Unique<VkInstance> instance(rawInst, [](VkInstance i) { vkDestroyInstance(i, nullptr); });

    uint32_t val_errors = 0;
    Unique<VkDebugUtilsMessengerEXT> messenger;
    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(rawInst, "vkCreateDebugUtilsMessengerEXT"));
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(rawInst, "vkDestroyDebugUtilsMessengerEXT"));
        if (create && destroy) {
            VkDebugUtilsMessengerCreateInfoEXT mi{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = debug_cb;
            mi.pUserData = &val_errors;
            VkDebugUtilsMessengerEXT raw = VK_NULL_HANDLE;
            if (create(rawInst, &mi, nullptr, &raw) == VK_SUCCESS)
                messenger = Unique<VkDebugUtilsMessengerEXT>(
                    raw, [rawInst, destroy](VkDebugUtilsMessengerEXT m) {
                        destroy(rawInst, m, nullptr);
                    });
        }
    }

    VkWin32SurfaceCreateInfoKHR wsci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    wsci.hinstance = hinst;
    wsci.hwnd = rawWin;
    VkSurfaceKHR rawSurf = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(rawInst, &wsci, nullptr, &rawSurf) != VK_SUCCESS)
        unavailable("vkCreateWin32SurfaceKHR failed");
    Unique<VkSurfaceKHR> surface(rawSurf, [rawInst](VkSurfaceKHR s) {
        vkDestroySurfaceKHR(rawInst, s, nullptr);
    });

    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t gfx = UINT32_MAX;
    if (!pick_present_device(rawInst, rawSurf, pd, gfx)) unavailable("no graphics+present queue family");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = gfx;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devext;
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(pd, &dci, nullptr, &dev) != VK_SUCCESS)
        unavailable("vkCreateDevice (swapchain) failed");
    Unique<VkDevice> device(dev, [](VkDevice d) { vkDestroyDevice(d, nullptr); });
    VkQueue queue;
    vkGetDeviceQueue(dev, gfx, 0, &queue);

    VkSurfaceCapabilitiesKHR caps{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, rawSurf, &caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    uint32_t nfmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, rawSurf, &nfmt, nullptr);
    if (nfmt == 0) unavailable("surface reports no formats");
    std::vector<VkSurfaceFormatKHR> fmts(nfmt);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, rawSurf, &nfmt, fmts.data());
    VkSurfaceFormatKHR fmt = fmts[0];
    for (const auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { fmt = f; break; }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) unavailable("surface extent is zero (minimized)");
    width = extent.width;
    height = extent.height;

    uint32_t want_images = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && want_images > caps.maxImageCount)
        want_images = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = rawSurf;
    sci.minImageCount = want_images;
    sci.imageFormat = fmt.format;
    sci.imageColorSpace = fmt.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    VkSwapchainKHR rawSwap = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(dev, &sci, nullptr, &rawSwap) != VK_SUCCESS)
        unavailable("vkCreateSwapchainKHR failed");
    Unique<VkSwapchainKHR> swapchain(rawSwap, [dev](VkSwapchainKHR s) {
        vkDestroySwapchainKHR(dev, s, nullptr);
    });

    uint32_t nimg = 0;
    vkGetSwapchainImagesKHR(dev, rawSwap, &nimg, nullptr);
    std::vector<VkImage> swap_images(nimg);
    vkGetSwapchainImagesKHR(dev, rawSwap, &nimg, swap_images.data());

    const VkFormat kDepth = VK_FORMAT_D32_SFLOAT;
    std::vector<Unique<VkImageView>> swap_views(nimg);
    std::vector<Image> depths(nimg);
    for (uint32_t i = 0; i < nimg; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = swap_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView rawv = VK_NULL_HANDLE;
        check(vkCreateImageView(dev, &vi, nullptr, &rawv), "vkCreateImageView(swapchain)");
        swap_views[i] = Unique<VkImageView>(rawv, [dev](VkImageView v) {
            vkDestroyImageView(dev, v, nullptr);
        });
        depths[i] = make_image(pd, dev, kDepth, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                               VK_IMAGE_ASPECT_DEPTH_BIT, width, height);
    }

    std::vector<TexData> texdata;
    texdata.push_back({1, 1, {255, 255, 255, 255}});
    texdata.push_back({1, 1, {128, 128, 255, 255}});
    texdata.push_back({1, 1, {0, 0, 0, 255}});
    const int kDefaultWhite = 0, kDefaultNormal = 1, kDefaultEmissive = 2;
    std::unordered_map<std::string, int> tex_index;
    std::unordered_map<const void*, int> inline_index;
    auto resolve_texture = [&](const formats::Model& m, int material,
                               const char* usage, int def) -> int {
        if (const formats::M3Layer* layer = layer_ptr_of(m, material, usage)) {
            if (!layer->inline_rgba.empty() && layer->inline_w && layer->inline_h &&
                layer->inline_w <= 16384 && layer->inline_h <= 16384 &&
                layer->inline_rgba.size() >= size_t(layer->inline_w) * layer->inline_h * 4) {
                auto it = inline_index.find(layer);
                if (it != inline_index.end()) return it->second;
                int idx = int(texdata.size());
                texdata.push_back({layer->inline_w, layer->inline_h, layer->inline_rgba});
                inline_index[layer] = idx;
                return idx;
            }
        }
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
            if (tri.empty()) throw std::runtime_error("render_window: no triangles");
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

    const VkFormat kTexColor = VK_FORMAT_R8G8B8A8_UNORM;
    std::vector<Image> textures;
    std::vector<Buffer> staging;
    for (const auto& td : texdata) {
        textures.push_back(make_image(pd, dev, kTexColor,
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

    uint32_t mat_sets = 0;
    for (const Mesh& mesh : meshes) mat_sets += uint32_t(mesh.draws.size());
    VkDescriptorPoolSize psizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kNumSlots * (mat_sets ? mat_sets : 1)},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, uint32_t(live.size())}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = mat_sets + uint32_t(live.size());
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

    VkAttachmentDescription att[2]{};
    att[0].format = fmt.format; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
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
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = 0;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 2; rpi.pAttachments = att;
    rpi.subpassCount = 1; rpi.pSubpasses = &subp;
    rpi.dependencyCount = 2; rpi.pDependencies = deps;
    VkRenderPass rawPass = VK_NULL_HANDLE;
    check(vkCreateRenderPass(dev, &rpi, nullptr, &rawPass), "vkCreateRenderPass");
    Unique<VkRenderPass> renderPass(rawPass, [dev](VkRenderPass p) { vkDestroyRenderPass(dev, p, nullptr); });

    std::vector<Unique<VkFramebuffer>> framebuffers(nimg);
    for (uint32_t i = 0; i < nimg; ++i) {
        VkImageView fbViews[2] = {swap_views[i].get(), depths[i].view.get()};
        VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbi.renderPass = rawPass; fbi.attachmentCount = 2; fbi.pAttachments = fbViews;
        fbi.width = width; fbi.height = height; fbi.layers = 1;
        VkFramebuffer rawFb = VK_NULL_HANDLE;
        check(vkCreateFramebuffer(dev, &fbi, nullptr, &rawFb), "vkCreateFramebuffer");
        framebuffers[i] = Unique<VkFramebuffer>(rawFb, [dev](VkFramebuffer f) {
            vkDestroyFramebuffer(dev, f, nullptr); });
    }

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

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = gfx;
    VkCommandPool rawCmdPool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(dev, &cpi, nullptr, &rawCmdPool), "vkCreateCommandPool");
    Unique<VkCommandPool> cmdPool(rawCmdPool, [dev](VkCommandPool p) { vkDestroyCommandPool(dev, p, nullptr); });

    {
        VkCommandBufferAllocateInfo ub{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ub.commandPool = rawCmdPool; ub.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ub.commandBufferCount = 1;
        VkCommandBuffer up;
        check(vkAllocateCommandBuffers(dev, &ub, &up), "vkAllocateCommandBuffers(upload)");
        VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(up, &bbi), "vkBeginCommandBuffer(upload)");
        for (size_t i = 0; i < textures.size(); ++i) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.image = textures[i].img.get();
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy rgn{};
            rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rgn.imageExtent = {texdata[i].w, texdata[i].h, 1};
            vkCmdCopyBufferToImage(up, staging[i].buf.get(), textures[i].img.get(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(up, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        }
        check(vkEndCommandBuffer(up), "vkEndCommandBuffer(upload)");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &up;
        check(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit(upload)");
        check(vkQueueWaitIdle(queue), "vkQueueWaitIdle(upload)");
        vkFreeCommandBuffers(dev, rawCmdPool, 1, &up);
    }

    std::vector<VkCommandBuffer> cmds(nimg, VK_NULL_HANDLE);
    {
        VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbi.commandPool = rawCmdPool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = nimg;
        check(vkAllocateCommandBuffers(dev, &cbi, cmds.data()), "vkAllocateCommandBuffers");
    }
    VkClearValue clears[2];
    clears[0].color = {{0.06f, 0.07f, 0.09f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    for (uint32_t i = 0; i < nimg; ++i) {
        VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmds[i], &bbi), "vkBeginCommandBuffer");
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = rawPass; rbi.framebuffer = framebuffers[i].get();
        rbi.renderArea = {{0, 0}, {width, height}};
        rbi.clearValueCount = 2; rbi.pClearValues = clears;
        vkCmdBeginRenderPass(cmds[i], &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmds[i], VK_PIPELINE_BIND_POINT_GRAPHICS, rawPipeline);
        for (size_t j = 0; j < live.size(); ++j) {
            const SceneInstance& inst = *live[j];
            const Mesh& mesh = meshes[mesh_of[inst.model]];
            Mat4 mvp = view_proj * inst.world;
            float pc[20];
            std::memcpy(pc, mvp.m.data(), 64);
            pc[16] = inst.team[0]; pc[17] = inst.team[1];
            pc[18] = inst.team[2]; pc[19] = inst.team[3];
            vkCmdPushConstants(cmds[i], rawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, 80, pc);
            VkDeviceSize off = 0;
            VkBuffer vb = mesh.vbuf.buf.get();
            vkCmdBindVertexBuffers(cmds[i], 0, 1, &vb, &off);
            vkCmdBindIndexBuffer(cmds[i], mesh.ibuf.buf.get(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdBindDescriptorSets(cmds[i], VK_PIPELINE_BIND_POINT_GRAPHICS, rawLayout, 1, 1,
                                    &bone_set[j], 0, nullptr);
            for (size_t d = 0; d < mesh.draws.size(); ++d) {
                vkCmdBindDescriptorSets(cmds[i], VK_PIPELINE_BIND_POINT_GRAPHICS, rawLayout, 0, 1,
                                        &mesh.draw_set[d], 0, nullptr);
                vkCmdDrawIndexed(cmds[i], mesh.draws[d].index_count, 1, mesh.draws[d].first_index, 0, 0);
            }
        }
        vkCmdEndRenderPass(cmds[i]);
        check(vkEndCommandBuffer(cmds[i]), "vkEndCommandBuffer");
    }

    std::vector<Unique<VkSemaphore>> acquireSem(frames), submitSem(frames);
    std::vector<Unique<VkFence>> fences(frames);
    for (uint32_t f = 0; f < frames; ++f) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore a = VK_NULL_HANDLE, b = VK_NULL_HANDLE;
        check(vkCreateSemaphore(dev, &si, nullptr, &a), "vkCreateSemaphore(acquire)");
        acquireSem[f] = Unique<VkSemaphore>(a, [dev](VkSemaphore s) { vkDestroySemaphore(dev, s, nullptr); });
        check(vkCreateSemaphore(dev, &si, nullptr, &b), "vkCreateSemaphore(submit)");
        submitSem[f] = Unique<VkSemaphore>(b, [dev](VkSemaphore s) { vkDestroySemaphore(dev, s, nullptr); });
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        check(vkCreateFence(dev, &fi, nullptr, &fence), "vkCreateFence");
        fences[f] = Unique<VkFence>(fence, [dev](VkFence x) { vkDestroyFence(dev, x, nullptr); });
    }
    std::vector<VkFence> image_fence(nimg, VK_NULL_HANDLE);

    auto pump = [&]() -> bool {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
            if (m.message == WM_QUIT) return false;
        }
        return true;
    };

    WindowResult result;
    result.created = true;
    result.width = width;
    result.height = height;
    result.validation_active = bool(messenger);

    for (uint32_t f = 0; f < frames; ++f) {
        if (!pump()) break;
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(dev, rawSwap, UINT64_MAX,
                                            acquireSem[f].get(), VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) break;
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) break;
        if (image_fence[idx] != VK_NULL_HANDLE)
            vkWaitForFences(dev, 1, &image_fence[idx], VK_TRUE, UINT64_MAX);
        image_fence[idx] = fences[f].get();

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore waitSem = acquireSem[f].get();
        VkSemaphore sigSem = submitSem[f].get();
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &waitSem; si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmds[idx];
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sigSem;
        check(vkQueueSubmit(queue, 1, &si, fences[f].get()), "vkQueueSubmit");

        VkPresentInfoKHR ppi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        ppi.waitSemaphoreCount = 1; ppi.pWaitSemaphores = &sigSem;
        ppi.swapchainCount = 1; ppi.pSwapchains = &rawSwap; ppi.pImageIndices = &idx;
        VkResult pr = vkQueuePresentKHR(queue, &ppi);
        if (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) ++result.frames_presented;
        else break;
    }

    vkDeviceWaitIdle(dev);
    result.validation_errors = val_errors;
    return result;
}

}
