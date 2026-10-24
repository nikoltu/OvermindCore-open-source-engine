#include "sc2/render/frame_prof.hpp"
#include "sc2/render/game_window.hpp"

#include "sc2/core/version.hpp"

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
#include <windowsx.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/render/device.hpp"
#include "sc2/render/scene.hpp"
#include "sc2/render/scene_renderer.hpp"
#include "sc2/render/texture_decode.hpp"
#include "sc2/render/vk_helpers.hpp"
#include "sc2/render/vk_raii.hpp"
#include "sc2/render/wsi_device.hpp"

static const uint32_t kOverlayVertSpv[] =
#include "overlay.vert.spv.h"
    ;
static const uint32_t kOverlayFragSpv[] =
#include "overlay.frag.spv.h"
    ;

namespace sc2::render {

namespace {

[[noreturn]] void unavailable(const char* what) {
    throw WindowUnavailable(std::string("run_game_window: ") + what);
}

struct InputAccum {
    float mouse_x = 0, mouse_y = 0;
    bool left = false, right = false, middle = false, shift = false;
    bool pan_left = false, pan_right = false, pan_up = false, pan_down = false;
    bool orbit_left = false, orbit_right = false;
    bool train = false;
    bool build = false;
    bool debug = false;
    bool hold = false;
    bool stop = false;
    bool patrol = false;
    bool ctrl = false;
    bool num[10] = {};
    float wheel = 0;
    bool quit = false;
    bool esc = false;
    char key_char = 0;
    std::vector<std::string> key_tokens;
    bool alt = false;
    std::string text;
    bool resized = false;
    bool minimized = false;
};

std::atomic<bool> g_os_cursor_hidden{false};

LRESULT CALLBACK game_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* in = reinterpret_cast<InputAccum*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
        case WM_CLOSE:
            if (in) in->quit = true;
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            if (in) {
                if (wp == SIZE_MINIMIZED) {
                    in->minimized = true;
                } else {
                    in->minimized = false;
                    in->resized = true;
                }
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && g_os_cursor_hidden.load()) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE:
            if (in) { in->mouse_x = float(GET_X_LPARAM(lp)); in->mouse_y = float(GET_Y_LPARAM(lp)); }
            return 0;
        case WM_LBUTTONDOWN: if (in) in->left = true; return 0;
        case WM_LBUTTONUP:   if (in) in->left = false; return 0;
        case WM_RBUTTONDOWN: if (in) in->right = true; return 0;
        case WM_RBUTTONUP:   if (in) in->right = false; return 0;
        case WM_MBUTTONDOWN: if (in) in->middle = true; return 0;
        case WM_MBUTTONUP:   if (in) in->middle = false; return 0;
        case WM_MOUSEWHEEL:
            if (in) in->wheel += float(GET_WHEEL_DELTA_WPARAM(wp)) / 120.0f;
            return 0;
        case WM_CHAR: {
            static std::uint32_t high = 0;
            std::uint32_t cp = std::uint32_t(wp) & 0xFFFFu;
            if (cp >= 0xD800u && cp <= 0xDBFFu) { high = cp; return 0; }
            if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                if (!high) return 0;
                cp = 0x10000u + ((high - 0xD800u) << 10) + (cp - 0xDC00u);
            }
            high = 0;
            if (!in || cp < 0x20u || cp == 0x7Fu) return 0;
            if (cp < 0x80u) in->text += char(cp);
            else if (cp < 0x800u) { in->text += char(0xC0u | (cp >> 6)); in->text += char(0x80u | (cp & 0x3Fu)); }
            else if (cp < 0x10000u) {
                in->text += char(0xE0u | (cp >> 12));
                in->text += char(0x80u | ((cp >> 6) & 0x3Fu));
                in->text += char(0x80u | (cp & 0x3Fu));
            } else {
                in->text += char(0xF0u | (cp >> 18));
                in->text += char(0x80u | ((cp >> 12) & 0x3Fu));
                in->text += char(0x80u | ((cp >> 6) & 0x3Fu));
                in->text += char(0x80u | (cp & 0x3Fu));
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP: {
            const bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
            if (in && down && (lp & (LPARAM(1) << 30)) == 0) {
                const char* tok = nullptr;
                char fbuf[4] = {};
                if (wp >= VK_F1 && wp <= VK_F12) {
                    const int n = int(wp - VK_F1) + 1;
                    fbuf[0] = 'F';
                    if (n >= 10) { fbuf[1] = '1'; fbuf[2] = char('0' + n - 10); }
                    else fbuf[1] = char('0' + n);
                    tok = fbuf;
                } else switch (wp) {
                    case VK_TAB: tok = "Tab"; break;
                    case VK_BACK: tok = "Backspace"; break;
                    case VK_SPACE: tok = "Space"; break;
                    case VK_INSERT: tok = "Insert"; break;
                    case VK_DELETE: tok = "Delete"; break;
                    case VK_HOME: tok = "Home"; break;
                    case VK_END: tok = "End"; break;
                    case VK_PRIOR: tok = "PageUp"; break;
                    case VK_NEXT: tok = "PageDown"; break;
                    case VK_ADD: tok = "NumPadPlus"; break;
                    case VK_SUBTRACT: tok = "NumPadMinus"; break;
                    case VK_PAUSE: tok = "Pause"; break;
                    case VK_RETURN: tok = "Enter"; break;
                    case VK_OEM_2: tok = "Slash"; break;
                    case VK_OEM_5: tok = "BackSlash"; break;
                    case VK_OEM_4: tok = "BracketOpen"; break;
                    case VK_OEM_6: tok = "BracketClose"; break;
                    case VK_OEM_7: tok = "Apostrophe"; break;
                    default: break;
                }
                if (tok) in->key_tokens.emplace_back(tok);
            }
            if (in && wp == VK_MENU) in->alt = down;
            if (in && down && wp >= 'A' && wp <= 'Z') in->key_char = char(wp);
            if (in) switch (wp) {
                case VK_UP: in->pan_up = down; break;
                case VK_DOWN: in->pan_down = down; break;
                case VK_LEFT: in->pan_left = down; break;
                case VK_RIGHT: in->pan_right = down; break;
                case VK_INSERT: in->orbit_left = down; break;
                case VK_DELETE: in->orbit_right = down; break;
                case 'T': in->train = down; break;
                case 'B': in->build = down; break;
                case 'H': in->hold = down; break;
                case 'X': in->stop = down; break;
                case 'P': in->patrol = down; break;
                case VK_F4: in->debug = down; break;
                case VK_CONTROL: in->ctrl = down; break;
                case VK_SHIFT: in->shift = down; break;
                case VK_ESCAPE: if (down) in->esc = true; break;
                default:
                    if (wp >= '0' && wp <= '9') in->num[wp - '0'] = down;
                    break;
            }
            if ((msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && wp != VK_F10) break;
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

const wchar_t* game_window_class(HINSTANCE hinst) {
    static const wchar_t* kName = L"sc2_game_window";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = game_wndproc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                 0, 0, LR_DEFAULTSIZE | LR_SHARED));
        wc.hIconSm = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                   16, 16, LR_SHARED));
        wc.lpszClassName = kName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            unavailable("RegisterClassEx failed");
        registered = true;
    }
    return kName;
}

}

void set_os_cursor_hidden(bool hidden) { g_os_cursor_hidden.store(hidden); }

WindowResult run_game_window(const GameFrameFn& frame, const casc::Storage* storage,
                             uint32_t width, uint32_t height,
                             const std::string& texture_root, uint32_t max_frames,
                             const std::function<data::EnvLight()>& env_provider, float shadow_clip, const std::function<const FogField*()>& fog_provider) {
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    const wchar_t* cls = game_window_class(hinst);
    RECT rc{0, 0, LONG(width), LONG(height)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    std::wstring wtitle(L"sc2 game ");
    for (const char* p = sc2::kVersion; *p; ++p) wtitle.push_back(wchar_t(*p));
    HWND rawWin = CreateWindowExW(0, cls, wtitle.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                  nullptr, nullptr, hinst, nullptr);
    if (!rawWin) unavailable("CreateWindowEx failed");
    Unique<HWND> window(rawWin, [](HWND h) { DestroyWindow(h); });
    InputAccum accum;
    accum.mouse_x = float(width) / 2.0f;
    accum.mouse_y = float(height) / 2.0f;
    SetWindowLongPtrW(rawWin, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&accum));
    ShowWindow(rawWin, SW_SHOWNA);

    uint32_t val_errors = 0;
    WsiDevice wsi = create_wsi_device(hinst, rawWin, "run_game_window: ", &val_errors);
    VkSurfaceKHR rawSurf = wsi.surface.get();
    VkPhysicalDevice pd = wsi.pd;
    uint32_t gfx = wsi.gfx_family;
    VkDevice dev = wsi.device.get();
    VkQueue queue = wsi.queue;
    Device::offer_window_device(wsi.instance.get(), pd, dev, wsi.side_queue, gfx);
    struct OfferGuard {
        VkDevice dev;
        ~OfferGuard() {
            if (Device::shared_is_adopted()) {
                vkDeviceWaitIdle(dev);
                release_scene_sessions();
            }
            Device::withdraw_window_device();
        }
    } offer_guard{dev};

    {
        VkSurfaceCapabilitiesKHR caps0{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, rawSurf, &caps0),
              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        VkExtent2D e0 = caps0.currentExtent;
        if (e0.width == 0xFFFFFFFFu) {
            e0.width = std::clamp(width, caps0.minImageExtent.width, caps0.maxImageExtent.width);
            e0.height = std::clamp(height, caps0.minImageExtent.height, caps0.maxImageExtent.height);
        }
        if (e0.width == 0 || e0.height == 0) unavailable("surface extent is zero (minimized)");
        width = e0.width;
        height = e0.height;
    }
    uint32_t nfmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, rawSurf, &nfmt, nullptr);
    if (nfmt == 0) unavailable("surface reports no formats");
    std::vector<VkSurfaceFormatKHR> fmts(nfmt);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, rawSurf, &nfmt, fmts.data());
    VkSurfaceFormatKHR fmt = fmts[0];
    for (const auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            fmt = f;
            break;
        }

    SceneRenderer::Tunables tun;
    tun.camera_fit_shadow = true;
    tun.shadow_clip = shadow_clip;
    tun.clear_color[0] = tun.clear_color[1] = tun.clear_color[2] = 0.0f;
    mem_mark("window: device + swapchain");
    SceneRenderer scene_renderer(pd, dev, queue, gfx, width, height, fmt.format, storage,
                                 texture_root, tun);
    mem_mark("window: scene renderer");
    bool first_present_marked = false;

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = gfx;
    VkCommandPool rawCmdPool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(dev, &cpi, nullptr, &rawCmdPool), "vkCreateCommandPool");
    Unique<VkCommandPool> cmdPool(rawCmdPool, [dev](VkCommandPool p) { vkDestroyCommandPool(dev, p, nullptr); });

    VkSamplerCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    smi.magFilter = smi.minFilter = VK_FILTER_LINEAR;
    smi.addressModeU = smi.addressModeV = smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    smi.maxLod = 1.0f;
    VkSampler rawSampler = VK_NULL_HANDLE;
    check(vkCreateSampler(dev, &smi, nullptr, &rawSampler), "vkCreateSampler");
    Unique<VkSampler> sampler(rawSampler, [dev](VkSampler s) { vkDestroySampler(dev, s, nullptr); });

    VkAttachmentDescription att{};
    att.format = fmt.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subp{};
    subp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subp.colorAttachmentCount = 1;
    subp.pColorAttachments = &colorRef;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &subp;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    VkRenderPass rawPass = VK_NULL_HANDLE;
    check(vkCreateRenderPass(dev, &rpi, nullptr, &rawPass), "vkCreateRenderPass");
    Unique<VkRenderPass> renderPass(rawPass, [dev](VkRenderPass p) { vkDestroyRenderPass(dev, p, nullptr); });

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
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

    const VkFormat kTexColor = VK_FORMAT_R8G8B8A8_UNORM;
    VkDescriptorSetLayoutBinding ovlB{};
    ovlB.binding = 0;
    ovlB.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ovlB.descriptorCount = 1;
    ovlB.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo ovlLi{};
    ovlLi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ovlLi.bindingCount = 1;
    ovlLi.pBindings = &ovlB;
    VkDescriptorSetLayout rawOvlSetLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev, &ovlLi, nullptr, &rawOvlSetLayout), "vkCreateDescriptorSetLayout(overlay)");
    Unique<VkDescriptorSetLayout> ovlSetLayout(rawOvlSetLayout, [dev](VkDescriptorSetLayout l) {
        vkDestroyDescriptorSetLayout(dev, l, nullptr); });
    VkPipelineLayoutCreateInfo ovlPli{};
    ovlPli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ovlPli.setLayoutCount = 1;
    ovlPli.pSetLayouts = &rawOvlSetLayout;
    VkPipelineLayout rawOvlLayout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(dev, &ovlPli, nullptr, &rawOvlLayout), "vkCreatePipelineLayout(overlay)");
    Unique<VkPipelineLayout> ovlLayout(rawOvlLayout, [dev](VkPipelineLayout l) { vkDestroyPipelineLayout(dev, l, nullptr); });

    Unique<VkShaderModule> ovlVs = make_shader(dev, kOverlayVertSpv, sizeof(kOverlayVertSpv));
    Unique<VkShaderModule> ovlFs = make_shader(dev, kOverlayFragSpv, sizeof(kOverlayFragSpv));
    VkPipelineShaderStageCreateInfo ovlStages[2]{};
    ovlStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ovlStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    ovlStages[0].module = ovlVs.get();
    ovlStages[0].pName = "main";
    ovlStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ovlStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    ovlStages[1].module = ovlFs.get();
    ovlStages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo ovlVin =
        vk_init<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    VkPipelineDepthStencilStateCreateInfo ovlDss{};
    ovlDss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ovlDss.depthTestEnable = VK_FALSE;
    ovlDss.depthWriteEnable = VK_FALSE;
    VkPipelineColorBlendAttachmentState ovlCba{};
    ovlCba.blendEnable = VK_TRUE;
    ovlCba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ovlCba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ovlCba.colorBlendOp = VK_BLEND_OP_ADD;
    ovlCba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ovlCba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ovlCba.alphaBlendOp = VK_BLEND_OP_ADD;
    ovlCba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo ovlCb{};
    ovlCb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    ovlCb.attachmentCount = 1;
    ovlCb.pAttachments = &ovlCba;
    VkGraphicsPipelineCreateInfo ovlGpi{};
    ovlGpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ovlGpi.stageCount = 2;
    ovlGpi.pStages = ovlStages;
    ovlGpi.pVertexInputState = &ovlVin;
    ovlGpi.pInputAssemblyState = &ia;
    ovlGpi.pViewportState = &vps;
    ovlGpi.pRasterizationState = &rs;
    ovlGpi.pMultisampleState = &ms;
    ovlGpi.pDepthStencilState = &ovlDss;
    ovlGpi.pColorBlendState = &ovlCb;
    ovlGpi.pDynamicState = &dyn;
    ovlGpi.layout = rawOvlLayout;
    ovlGpi.renderPass = rawPass;
    VkPipeline rawOvlPipeline = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ovlGpi, nullptr, &rawOvlPipeline),
          "vkCreateGraphicsPipelines(overlay)");
    Unique<VkPipeline> ovlPipeline(rawOvlPipeline, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });

    Image ovlTex = make_image(pd, dev, kTexColor,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, width, height);
    Buffer ovlStaging = make_buffer(pd, dev, VkDeviceSize(width) * height * 4,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT, nullptr);
    VkDescriptorPoolSize ovlPsz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo ovlDpi{};
    ovlDpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ovlDpi.maxSets = 1;
    ovlDpi.poolSizeCount = 1;
    ovlDpi.pPoolSizes = &ovlPsz;
    VkDescriptorPool rawOvlPool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(dev, &ovlDpi, nullptr, &rawOvlPool), "vkCreateDescriptorPool(overlay)");
    Unique<VkDescriptorPool> ovlPool(rawOvlPool, [dev](VkDescriptorPool p) { vkDestroyDescriptorPool(dev, p, nullptr); });
    VkDescriptorSet ovlSet = VK_NULL_HANDLE;
    {
        VkDescriptorSetAllocateInfo dsa{};
        dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsa.descriptorPool = rawOvlPool;
        dsa.descriptorSetCount = 1;
        dsa.pSetLayouts = &rawOvlSetLayout;
        check(vkAllocateDescriptorSets(dev, &dsa, &ovlSet), "vkAllocateDescriptorSets(overlay)");
        VkDescriptorImageInfo dii{rawSampler, ovlTex.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = ovlSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &dii;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = rawCmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(dev, &cbi, &cmd), "vkAllocateCommandBuffers");

    VkSemaphoreCreateInfo semi{};

    semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore aRaw = VK_NULL_HANDLE;
    check(vkCreateSemaphore(dev, &semi, nullptr, &aRaw), "vkCreateSemaphore(acquire)");
    Unique<VkSemaphore> acquireSem(aRaw, [dev](VkSemaphore s) { vkDestroySemaphore(dev, s, nullptr); });
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fRaw = VK_NULL_HANDLE;
    check(vkCreateFence(dev, &fi, nullptr, &fRaw), "vkCreateFence");
    Unique<VkFence> fence(fRaw, [dev](VkFence x) { vkDestroyFence(dev, x, nullptr); });

    Unique<VkSwapchainKHR> swapchain(VK_NULL_HANDLE, [dev](VkSwapchainKHR s) {
        if (s) vkDestroySwapchainKHR(dev, s, nullptr); });
    std::vector<VkImage> swap_images;
    std::vector<Unique<VkImageView>> swap_views;
    std::vector<Unique<VkFramebuffer>> framebuffers;
    std::vector<Unique<VkSemaphore>> submitSem;
    uint32_t nimg = 0;
    uint32_t sc_w = width, sc_h = height;

    auto recreate_swapchain = [&]() -> bool {
        vkDeviceWaitIdle(dev);
        VkSurfaceCapabilitiesKHR caps{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, rawSurf, &caps),
              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR(recreate)");
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFFu) {
            extent.width = std::clamp(sc_w, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(sc_h, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) return false;

        framebuffers.clear();
        swap_views.clear();
        submitSem.clear();
        swap_images.clear();
        VkSwapchainKHR old = swapchain.get();

        uint32_t want_images = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && want_images > caps.maxImageCount) want_images = caps.maxImageCount;
        VkSwapchainCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface = rawSurf;
        sci.minImageCount = want_images;
        sci.imageFormat = fmt.format;
        sci.imageColorSpace = fmt.colorSpace;
        sci.imageExtent = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = old;
        VkSwapchainKHR rawSwap = VK_NULL_HANDLE;
        if (vkCreateSwapchainKHR(dev, &sci, nullptr, &rawSwap) != VK_SUCCESS)
            unavailable("vkCreateSwapchainKHR failed");
        swapchain = Unique<VkSwapchainKHR>(rawSwap, [dev](VkSwapchainKHR s) {
            if (s) vkDestroySwapchainKHR(dev, s, nullptr); });

        uint32_t n = 0;
        vkGetSwapchainImagesKHR(dev, rawSwap, &n, nullptr);
        swap_images.resize(n);
        vkGetSwapchainImagesKHR(dev, rawSwap, &n, swap_images.data());
        nimg = n;

        swap_views.resize(nimg);
        framebuffers.resize(nimg);
        submitSem.resize(nimg);
        for (uint32_t i = 0; i < nimg; ++i) {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = swap_images[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = fmt.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView rawv = VK_NULL_HANDLE;
            check(vkCreateImageView(dev, &vi, nullptr, &rawv), "vkCreateImageView(swapchain)");
            swap_views[i] = Unique<VkImageView>(rawv, [dev](VkImageView v) { vkDestroyImageView(dev, v, nullptr); });

            VkImageView fbView = rawv;
            VkFramebufferCreateInfo fbi{};
            fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbi.renderPass = rawPass;
            fbi.attachmentCount = 1;
            fbi.pAttachments = &fbView;
            fbi.width = extent.width;
            fbi.height = extent.height;
            fbi.layers = 1;
            VkFramebuffer rawFb = VK_NULL_HANDLE;
            check(vkCreateFramebuffer(dev, &fbi, nullptr, &rawFb), "vkCreateFramebuffer");
            framebuffers[i] = Unique<VkFramebuffer>(rawFb, [dev](VkFramebuffer f) { vkDestroyFramebuffer(dev, f, nullptr); });

            VkSemaphore sRaw = VK_NULL_HANDLE;
            check(vkCreateSemaphore(dev, &semi, nullptr, &sRaw), "vkCreateSemaphore(submit)");
            submitSem[i] = Unique<VkSemaphore>(sRaw, [dev](VkSemaphore s) { vkDestroySemaphore(dev, s, nullptr); });
        }
        sc_w = extent.width;
        sc_h = extent.height;
        if (sc_w != scene_renderer.width() || sc_h != scene_renderer.height()) {
            scene_renderer.resize(sc_w, sc_h);
            ovlTex = make_image(pd, dev, kTexColor, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, sc_w, sc_h);
            ovlStaging = make_buffer(pd, dev, VkDeviceSize(sc_w) * sc_h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, nullptr);
            VkDescriptorImageInfo odii{rawSampler, ovlTex.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet ow{};
            ow.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ow.dstSet = ovlSet;
            ow.dstBinding = 0;
            ow.descriptorCount = 1;
            ow.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            ow.pImageInfo = &odii;
            vkUpdateDescriptorSets(dev, 1, &ow, 0, nullptr);
        }
        return true;
    };

    if (!recreate_swapchain()) unavailable("surface extent is zero (minimized)");

    LARGE_INTEGER freq, prev_t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev_t);
    InputAccum prev = accum;

    WindowResult result;
    result.created = true;
    result.width = width;
    result.height = height;
    result.validation_active = bool(wsi.messenger);

    std::vector<SceneInstance> scene;
    std::vector<uint8_t> overlay;
    Camera cam;
    bool running = true;

    while (running) {
        SC2_PROF_SCOPE("loop_iter");
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
            if (m.message == WM_QUIT) accum.quit = true;
        }
        if (accum.quit) running = false;

        VkSurfaceCapabilitiesKHR cur_caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, rawSurf, &cur_caps);
        VkExtent2D cur_ext = cur_caps.currentExtent;
        const bool zero_surface =
            (cur_ext.width == 0 || cur_ext.height == 0) && cur_ext.width != 0xFFFFFFFFu;
        if (accum.minimized || zero_surface) {
            if (!running) break;
            Sleep(10);
            QueryPerformanceCounter(&prev_t);
            continue;
        }

        if (accum.resized ||
            (cur_ext.width != 0xFFFFFFFFu && (cur_ext.width != sc_w || cur_ext.height != sc_h))) {
            accum.resized = false;
            if (!recreate_swapchain()) { Sleep(10); continue; }
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt_ms = float(double(now.QuadPart - prev_t.QuadPart) * 1000.0 / double(freq.QuadPart));
        prev_t = now;
        if (dt_ms > 250.0f) dt_ms = 250.0f;

        float msx = 1.0f, msy = 1.0f;
        if (RECT crc; GetClientRect(window.get(), &crc)) {
            const LONG cw = crc.right - crc.left, ch = crc.bottom - crc.top;
            if (cw > 0) msx = float(sc_w) / float(cw);
            if (ch > 0) msy = float(sc_h) / float(ch);
        }

        InputState in;
        in.view_w = sc_w;
        in.view_h = sc_h;
        in.mouse_x = accum.mouse_x * msx;
        in.mouse_y = accum.mouse_y * msy;
        in.mouse_dx = (accum.mouse_x - prev.mouse_x) * msx;
        in.mouse_dy = (accum.mouse_y - prev.mouse_y) * msy;
        in.wheel = accum.wheel;
        in.left_down = accum.left;
        in.left_pressed = accum.left && !prev.left;
        in.left_released = !accum.left && prev.left;
        in.right_down = accum.right;
        in.right_pressed = accum.right && !prev.right;
        in.right_released = !accum.right && prev.right;
        in.middle_down = accum.middle;
        in.shift = accum.shift;
        in.key_pan_left = accum.pan_left;
        in.key_pan_right = accum.pan_right;
        in.key_pan_up = accum.pan_up;
        in.key_pan_down = accum.pan_down;
        in.key_orbit_left = accum.orbit_left;
        in.key_orbit_right = accum.orbit_right;
        in.key_train = accum.train;
        in.key_build = accum.build;
        in.key_debug = accum.debug;
        in.key_hold = accum.hold;
        in.key_stop = accum.stop;
        in.key_patrol = accum.patrol;
        in.key_ctrl = accum.ctrl;
        for (int d = 0; d < 10; ++d) in.key_num[d] = accum.num[d];
        in.key_char = accum.key_char;
        in.key_tokens.swap(accum.key_tokens);
        in.key_alt = accum.alt;
        in.text.swap(accum.text);
        accum.text.clear();
        in.quit = accum.quit || accum.esc;
        prev = accum;
        accum.wheel = 0;
        accum.esc = false;
        accum.key_char = 0;
        accum.key_tokens.clear();

        scene.clear();
        overlay.clear();
        double cb_ms = 0.0;
        bool cont;
        { SC2_PROF_SCOPE("frame_cb"); cont = frame(in, dt_ms, scene, cam, overlay); }
        if (result.frames_presented == 0) mem_mark("first frame: step + overlay (callback)");
        (void)cb_ms;
        if (!cont || accum.quit) running = false;
        const bool has_overlay = overlay.size() == size_t(sc_w) * size_t(sc_h) * 4;

        if (env_provider) scene_renderer.set_environment(env_provider());

        VkFence raw_fence = fence.get();
        { SC2_PROF_SCOPE("fence_wait");
          vkWaitForFences(dev, 1, &raw_fence, VK_TRUE, UINT64_MAX); }
        SC2_PROF_SCOPE("submit_half");
        if (fog_provider) {
            const FogField* f = fog_provider();
            scene_renderer.set_fog(f ? *f : FogField{});
        }

        if (!running && result.frames_presented == 0 && max_frames == 0) break;

        std::vector<const SceneInstance*> live;
        for (const SceneInstance& s : scene)
            if (s.model && s.model->vertex_count && !s.model->indices.empty()) live.push_back(&s);

        std::unordered_map<const formats::Model*, bool> model_ready;
        auto ready_to_draw = [&](const formats::Model* m) {
            if (!world_async_loads() || !storage || result.frames_presented == 0 || scene_renderer.has_mesh(m)) return true;
            auto it = model_ready.find(m);
            if (it != model_ready.end()) return it->second;
            const bool r = SceneRenderer::request_model_textures(*m, storage, texture_root, 0);
            model_ready.emplace(m, r);
            return r;
        };
        live.erase(std::remove_if(live.begin(), live.end(), [&](const SceneInstance* s) { return !ready_to_draw(s->model); }), live.end());
        {
            std::size_t waiting = 0;
            for (const auto& kv : model_ready) waiting += kv.second ? 0 : 1;
            frame_prof_count("world.models_waiting", double(waiting));
        }

        std::vector<const SceneInstance*> light_only;
        for (const SceneInstance& s : scene)
            if (s.model && !(s.model->vertex_count && !s.model->indices.empty()) && !s.model->lights.empty()) light_only.push_back(&s);
        { SC2_PROF_SCOPE("prepare"); scene_renderer.prepare(live, &light_only); }
        if (result.frames_presented == 0) mem_mark("first frame: world meshes + textures (prepare)");

        std::vector<SceneRenderer::ParticleVertex> pverts;
        std::vector<SceneRenderer::ParticleGroup> pgroups;
        std::vector<const SceneInstance*> emitting = live;
        for (const SceneInstance& s : scene)
            if (s.model && !(s.model->vertex_count && !s.model->indices.empty()) &&
                (!s.model->particles.empty() || !s.model->ribbons.empty()) && ready_to_draw(s.model))
                emitting.push_back(&s);
        { SC2_PROF_SCOPE("particles"); build_particles(scene_renderer, emitting, cam, pverts, pgroups); }
        frame_prof_count("instances", double(scene.size()));

        VkSwapchainKHR cur_swap = swapchain.get();
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(dev, cur_swap, UINT64_MAX, acquireSem.get(), VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate_swapchain();
            continue;
        }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            recreate_swapchain();
            continue;
        }
        vkResetFences(dev, 1, &raw_fence);

        check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bbi{};
        bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        check(vkBeginCommandBuffer(cmd, &bbi), "vkBeginCommandBuffer");

        scene_renderer.record(cmd, live, cam, pverts, pgroups);

        VkImageMemoryBarrier lb{};
        lb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        lb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        lb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        lb.srcQueueFamilyIndex = lb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        lb.image = scene_renderer.ldr_image();
        lb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        lb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        lb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &lb);

        VkImageMemoryBarrier sb{};
        sb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        sb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        sb.srcQueueFamilyIndex = sb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sb.image = swap_images[idx];
        sb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        sb.srcAccessMask = 0;
        sb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &sb);

        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[0] = {0, 0, 0};
        bl.srcOffsets[1] = {int32_t(sc_w), int32_t(sc_h), 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.dstOffsets[0] = {0, 0, 0};
        bl.dstOffsets[1] = {int32_t(sc_w), int32_t(sc_h), 1};
        vkCmdBlitImage(cmd, scene_renderer.ldr_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       swap_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);

        if (has_overlay) {
            void* p = nullptr;
            check(vkMapMemory(dev, ovlStaging.mem.get(), 0, overlay.size(), 0, &p), "vkMapMemory(overlay)");
            std::memcpy(p, overlay.data(), overlay.size());
            vkUnmapMemory(dev, ovlStaging.mem.get());
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.image = ovlTex.img.get();
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy rgn{};
            rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rgn.imageExtent = {sc_w, sc_h, 1};
            vkCmdCopyBufferToImage(cmd, ovlStaging.buf.get(), ovlTex.img.get(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);

            VkRenderPassBeginInfo rbi{};

            rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rbi.renderPass = rawPass;
            rbi.framebuffer = framebuffers[idx].get();
            rbi.renderArea = {{0, 0}, {sc_w, sc_h}};
            rbi.clearValueCount = 0;
            rbi.pClearValues = nullptr;
            vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport ovp{0, 0, float(sc_w), float(sc_h), 0, 1};
            VkRect2D oscis{{0, 0}, {sc_w, sc_h}};
            vkCmdSetViewport(cmd, 0, 1, &ovp);
            vkCmdSetScissor(cmd, 0, 1, &oscis);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rawOvlPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rawOvlLayout, 0, 1, &ovlSet, 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
        } else {
            VkImageMemoryBarrier pb{};
            pb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            pb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            pb.srcQueueFamilyIndex = pb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pb.image = swap_images[idx];
            pb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            pb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            pb.dstAccessMask = 0;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &pb);
        }
        check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSemaphore waitSem = acquireSem.get(), sigSem = submitSem[idx].get();
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &waitSem;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sigSem;
        check(vkQueueSubmit(queue, 1, &si, raw_fence), "vkQueueSubmit");

        VkPresentInfoKHR ppi{};

        ppi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        ppi.waitSemaphoreCount = 1;
        ppi.pWaitSemaphores = &sigSem;
        ppi.swapchainCount = 1;
        ppi.pSwapchains = &cur_swap;
        ppi.pImageIndices = &idx;
        VkResult pr = VK_SUCCESS;
        { SC2_PROF_SCOPE("present"); pr = vkQueuePresentKHR(queue, &ppi); }
        if (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) ++result.frames_presented;
        if (result.frames_presented == 1 && !first_present_marked) {
            first_present_marked = true;
            mem_mark("first frame: record + submit + present");
        }
        frame_prof_tick();
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || accum.resized) {
            accum.resized = false;
            recreate_swapchain();
        } else if (pr != VK_SUCCESS) {
            recreate_swapchain();
        }

        if (max_frames && result.frames_presented >= max_frames) running = false;
    }

    vkDeviceWaitIdle(dev);
    result.validation_errors = val_errors;
    return result;
}

}
