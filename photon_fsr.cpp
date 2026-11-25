// photon_fsr.dll: a small C wrapper around the AMD FidelityFX API (FSR upscaler, Vulkan), called from Java
// (PhotonFsr) through the Foreign Function API. The signed AMD DLLs (amd_fidelityfx_loader_vk.dll and
// amd_fidelityfx_upscaler_vk.dll) are loaded at run time from the directory given to pf_init.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <cmath>

#include "ffx/api/include/ffx_api.h"
#include "ffx/api/include/ffx_api_types.h"
#include "ffx/api/include/ffx_api_loader.h"
#include "ffx/api/include/vk/ffx_api_vk.h"
#include "ffx/upscalers/include/ffx_upscale.h"

#define PF_API extern "C" __declspec(dllexport)

static HMODULE g_loader = nullptr;
static ffxFunctions g_ffx = {};
static VkDeviceContext g_dev = {};
static ffxContext g_ctx = nullptr;
static int g_last = 0;
static char g_msg[1024] = {0};

static void onMessage(uint32_t type, const wchar_t* message) {
    if (!message) return;
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, message, -1, buf, sizeof(buf), nullptr, nullptr);
    snprintf(g_msg, sizeof(g_msg), "%s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", buf);
}

/** Loads the AMD loader DLL from dir and keeps the game's device. 1 = ok. */
PF_API int pf_init(void* device, void* physicalDevice, void* getDeviceProcAddr, const wchar_t* dir) {
    if (!g_loader) {
        SetDllDirectoryW(dir);
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%s\\amd_fidelityfx_loader_vk.dll", dir);
        g_loader = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g_loader) {
            g_last = (int) GetLastError();
            snprintf(g_msg, sizeof(g_msg), "cannot load amd_fidelityfx_loader_vk.dll (error %d)", g_last);
            return 0;
        }
        ffxLoadFunctions(&g_ffx, g_loader);
        if (!g_ffx.CreateContext || !g_ffx.Dispatch || !g_ffx.DestroyContext) {
            snprintf(g_msg, sizeof(g_msg), "the loader DLL has no ffx functions");
            return 0;
        }
    }
    g_dev.vkDevice = (VkDevice) device;
    g_dev.vkPhysicalDevice = (VkPhysicalDevice) physicalDevice;
    g_dev.vkDeviceProcAddr = (PFN_vkGetDeviceProcAddr) getDeviceProcAddr;
    return 1;
}

PF_API void pf_release() {
    if (g_ctx && g_ffx.DestroyContext) g_ffx.DestroyContext(&g_ctx, nullptr);
    g_ctx = nullptr;
}

/** Makes the upscaler context for these sizes (the old one is destroyed). 1 = ok. */
PF_API int pf_create(int maxRenderW, int maxRenderH, int outW, int outH, int flags) {
    pf_release();
    ffxCreateBackendVKDesc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext = g_dev;

    ffxCreateContextDescUpscaleVersion version = {};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    backend.header.pNext = &version.header;

    ffxCreateContextDescUpscale desc = {};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    desc.flags = (uint32_t) flags;
    desc.maxRenderSize = {(uint32_t) maxRenderW, (uint32_t) maxRenderH};
    desc.maxUpscaleSize = {(uint32_t) outW, (uint32_t) outH};
    desc.fpMessage = onMessage;

    g_last = (int) g_ffx.CreateContext(&g_ctx, &desc.header, nullptr);
    if (g_last != FFX_API_RETURN_OK) {
        g_ctx = nullptr;
        return 0;
    }
    return 1;
}

/** Name of the provider in use (e.g. "FSR 4.0.x" or "FSR 3.1.x"), written into out. */
PF_API int pf_version(char* out, int cap) {
    if (!g_ctx || !g_ffx.Query) return 0;
    ffxQueryGetProviderVersion q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (g_ffx.Query(&g_ctx, &q.header) != FFX_API_RETURN_OK || !q.versionName) return 0;
    snprintf(out, cap, "%s", q.versionName);
    return 1;
}

// One image as given from Java: { VkImage, VkFormat, width, height, FFX usage, FFX state } = 32 bytes
struct PfImage {
    uint64_t image;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t usage;
    uint32_t state;
    uint32_t pad;
};

static FfxApiResource resourceOf(const PfImage* p) {
    FfxApiResource r = {};
    if (!p || !p->image) return r;
    r.resource = (void*) p->image;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffxApiGetSurfaceFormatVK((VkFormat) p->format);
    r.description.width = p->width;
    r.description.height = p->height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = p->usage;
    r.state = p->state;
    return r;
}

/**
 * Records the upscale into cmd. cfg (floats): 0 jitterX, 1 jitterY, 2 mvScaleX, 3 mvScaleY, 4 renderW, 5 renderH,
 * 6 outW, 7 outH, 8 sharpness (0 = off), 9 frame time ms, 10 reset, 11 near, 12 far, 13 vertical fov (radians),
 * 14 dispatch flags. 1 = ok.
 */
PF_API int pf_dispatch(void* cmd, const PfImage* color, const PfImage* depth, const PfImage* motion,
                       const PfImage* output, const float* cfg) {
    if (!g_ctx) return 0;
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmd;
    d.color = resourceOf(color);
    d.depth = resourceOf(depth);
    d.motionVectors = resourceOf(motion);
    d.output = resourceOf(output);
    d.jitterOffset = {cfg[0], cfg[1]};
    d.motionVectorScale = {cfg[2], cfg[3]};
    d.renderSize = {(uint32_t) cfg[4], (uint32_t) cfg[5]};
    d.upscaleSize = {(uint32_t) cfg[6], (uint32_t) cfg[7]};
    d.enableSharpening = cfg[8] > 0.0f;
    d.sharpness = cfg[8];
    d.frameTimeDelta = cfg[9];
    d.preExposure = 1.0f;
    d.reset = cfg[10] > 0.5f;
    d.cameraNear = cfg[11];
    d.cameraFar = cfg[12];
    d.cameraFovAngleVertical = cfg[13];
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = (uint32_t) cfg[14];
    g_last = (int) g_ffx.Dispatch(&g_ctx, &d.header);
    return g_last == FFX_API_RETURN_OK ? 1 : 0;
}

PF_API int pf_last_result() {
    return g_last;
}

PF_API int pf_last_message(char* out, int cap) {
    snprintf(out, cap, "%s", g_msg);
    return (int) strlen(g_msg);
}
