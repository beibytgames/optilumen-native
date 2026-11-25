// photon_xess.dll: a small C wrapper around Intel XeSS Super Resolution (Vulkan), called from Java (PhotonXess)
// through the Foreign Function API. libxess.dll is loaded at run time from the directory given to pxs_init.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>

#include "xess/sdk/inc/xess/xess_vk.h"

#define PX_API extern "C" __declspec(dllexport)

typedef xess_result_t (*PFN_xessVKCreateContext)(VkInstance, VkPhysicalDevice, VkDevice, xess_context_handle_t*);
typedef xess_result_t (*PFN_xessVKInit)(xess_context_handle_t, const xess_vk_init_params_t*);
typedef xess_result_t (*PFN_xessVKExecute)(xess_context_handle_t, VkCommandBuffer, const xess_vk_execute_params_t*);
typedef xess_result_t (*PFN_xessGetInputResolution)(xess_context_handle_t, const xess_2d_t*, xess_quality_settings_t, xess_2d_t*);
typedef xess_result_t (*PFN_xessSetVelocityScale)(xess_context_handle_t, float, float);
typedef xess_result_t (*PFN_xessDestroyContext)(xess_context_handle_t);
typedef xess_result_t (*PFN_xessGetVersion)(xess_version_t*);

static HMODULE g_lib = nullptr;
static PFN_xessVKCreateContext pCreate;
static PFN_xessVKInit pInit;
static PFN_xessVKExecute pExecute;
static PFN_xessGetInputResolution pInputRes;
static PFN_xessSetVelocityScale pVelScale;
static PFN_xessDestroyContext pDestroy;
static PFN_xessGetVersion pVersion;
static PFN_vkCmdPipelineBarrier pBarrier;

static xess_context_handle_t g_ctx = nullptr;
static int g_last = 0;

/** Loads libxess.dll from dir and makes the XeSS context on the game's device. 1 = ok. */
PX_API int pxs_init(void* instance, void* physicalDevice, void* device, void* getDeviceProcAddr, const wchar_t* dir) {
    if (!g_lib) {
        SetDllDirectoryW(dir);
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%s\\libxess.dll", dir);
        g_lib = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g_lib) {
            g_last = -100 - (int) GetLastError();
            return 0;
        }
        pCreate = (PFN_xessVKCreateContext) GetProcAddress(g_lib, "xessVKCreateContext");
        pInit = (PFN_xessVKInit) GetProcAddress(g_lib, "xessVKInit");
        pExecute = (PFN_xessVKExecute) GetProcAddress(g_lib, "xessVKExecute");
        pInputRes = (PFN_xessGetInputResolution) GetProcAddress(g_lib, "xessGetInputResolution");
        pVelScale = (PFN_xessSetVelocityScale) GetProcAddress(g_lib, "xessSetVelocityScale");
        pDestroy = (PFN_xessDestroyContext) GetProcAddress(g_lib, "xessDestroyContext");
        pVersion = (PFN_xessGetVersion) GetProcAddress(g_lib, "xessGetVersion");
        if (!pCreate || !pInit || !pExecute || !pInputRes) {
            g_last = -99;
            return 0;
        }
    }
    pBarrier = (PFN_vkCmdPipelineBarrier) ((PFN_vkGetDeviceProcAddr) getDeviceProcAddr)((VkDevice) device, "vkCmdPipelineBarrier");
    if (!g_ctx) {
        g_last = (int) pCreate((VkInstance) instance, (VkPhysicalDevice) physicalDevice, (VkDevice) device, &g_ctx);
        if (g_last != XESS_RESULT_SUCCESS) {
            g_ctx = nullptr;
            return 0;
        }
    }
    return 1;
}

PX_API int pxs_version(int* major, int* minor, int* patch) {
    if (!pVersion) return 0;
    xess_version_t v = {};
    if (pVersion(&v) != XESS_RESULT_SUCCESS) return 0;
    *major = v.major;
    *minor = v.minor;
    *patch = v.patch;
    return 1;
}

/** Initializes XeSS for this output size and quality (again when they change). 1 = ok. */
PX_API int pxs_create(int outW, int outH, int quality, int flags) {
    if (!g_ctx) return 0;
    xess_vk_init_params_t p = {};
    p.outputResolution = {(uint32_t) outW, (uint32_t) outH};
    p.qualitySetting = (xess_quality_settings_t) quality;
    p.initFlags = (uint32_t) flags;
    g_last = (int) pInit(g_ctx, &p);
    return g_last == XESS_RESULT_SUCCESS ? 1 : 0;
}

PX_API int pxs_input_size(int outW, int outH, int quality, int* w, int* h) {
    if (!g_ctx) return 0;
    xess_2d_t o = {(uint32_t) outW, (uint32_t) outH}, i = {};
    if (pInputRes(g_ctx, &o, (xess_quality_settings_t) quality, &i) != XESS_RESULT_SUCCESS) return 0;
    *w = (int) i.x;
    *h = (int) i.y;
    return 1;
}

// One image as given from Java: { VkImageView, VkImage, VkFormat, width, height, aspect } = 32 bytes
struct PxImage {
    uint64_t view;
    uint64_t image;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t aspect;
};

static xess_vk_image_view_info infoOf(const PxImage* p) {
    xess_vk_image_view_info i = {};
    i.imageView = (VkImageView) p->view;
    i.image = (VkImage) p->image;
    i.subresourceRange = {p->aspect, 0, 1, 0, 1};
    i.format = (VkFormat) p->format;
    i.width = p->width;
    i.height = p->height;
    return i;
}

static void layout(VkCommandBuffer cmd, const PxImage* p, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage) p->image;
    b.subresourceRange = {p->aspect, 0, 1, 0, 1};
    pBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

/**
 * Records XeSS into cmd. Inputs are moved from GENERAL to SHADER_READ_ONLY_OPTIMAL for it and back afterwards.
 * cfg: 0 jitterX, 1 jitterY, 2 inputW, 3 inputH, 4 velocity scale x, 5 velocity scale y, 6 reset. 1 = ok.
 */
PX_API int pxs_execute(void* cmd, const PxImage* color, const PxImage* velocity, const PxImage* depth,
                       const PxImage* output, const float* cfg) {
    if (!g_ctx) return 0;
    VkCommandBuffer cb = (VkCommandBuffer) cmd;
    if (pVelScale) pVelScale(g_ctx, cfg[4], cfg[5]);
    layout(cb, color, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    layout(cb, velocity, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    layout(cb, depth, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    xess_vk_execute_params_t e = {};
    e.colorTexture = infoOf(color);
    e.velocityTexture = infoOf(velocity);
    e.depthTexture = infoOf(depth);
    e.outputTexture = infoOf(output);
    e.jitterOffsetX = cfg[0];
    e.jitterOffsetY = cfg[1];
    e.exposureScale = 1.0f;
    e.resetHistory = cfg[6] > 0.5f ? 1u : 0u;
    e.inputWidth = (uint32_t) cfg[2];
    e.inputHeight = (uint32_t) cfg[3];
    g_last = (int) pExecute(g_ctx, cb, &e);

    layout(cb, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    layout(cb, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    layout(cb, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    return g_last == XESS_RESULT_SUCCESS ? 1 : 0;
}

PX_API int pxs_last_result() {
    return g_last;
}
