// Thin C wrapper around NVIDIA NGX (DLSS) for PhotonMod.
//
// NGX is shipped as a static library, so it cannot be called from Java directly. This DLL links it and exports a
// handful of plain C functions that the mod calls through the Java Foreign Function API. All Vulkan objects
// (instance, devices, images, views, command buffers) belong to the game; they are passed in as raw handles.
//
// Build (x64 Native Tools / or via build_dlss.ps1):
//   cl /LD /MT /O2 /EHsc /std:c++17 photon_dlss.cpp /I ngx\include /I %VULKAN_SDK%\Include ngx\lib\nvsdk_ngx_s.lib

#include <vulkan/vulkan.h>
#include <cwchar>
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_helpers_dlssd_vk.h"

// NGX_DLSSD_GET_OPTIMAL_SETTINGS lives in the D3D helper header (the same for Vulkan): a copy without D3D
static inline NVSDK_NGX_Result PD_DLSSD_GET_OPTIMAL_SETTINGS(NVSDK_NGX_Parameter* params, unsigned w, unsigned h,
                                                             NVSDK_NGX_PerfQuality_Value q, unsigned* outW, unsigned* outH) {
    void* callback = NULL;
    NVSDK_NGX_Parameter_GetVoidPointer(params, NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback, &callback);
    if (!callback) return NVSDK_NGX_Result_FAIL_OutOfDate;
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Width, w);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Height, h);
    NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_PerfQualityValue, q);
    NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_RTXValue, false);
    NVSDK_NGX_Result r = ((PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback) callback)(params);
    if (NVSDK_NGX_FAILED(r)) return r;
    NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_OutWidth, outW);
    NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_OutHeight, outH);
    return r;
}

#define PD_EXPORT extern "C" __declspec(dllexport)

static VkDevice g_device = VK_NULL_HANDLE;
static NVSDK_NGX_Parameter* g_params = nullptr;
static NVSDK_NGX_Handle* g_dlss = nullptr;
static bool g_init = false;
static int g_last = 0;   // last NVSDK_NGX_Result

// image passed from Java: view, image, VkFormat, width, height, aspect mask (1 = color, 2 = depth)
struct PdImage {
    unsigned long long view;
    unsigned long long image;
    int format;
    int width;
    int height;
    int aspect;
};

static NVSDK_NGX_Resource_VK toResource(const PdImage* im, bool readWrite) {
    VkImageSubresourceRange range = {};
    range.aspectMask = (VkImageAspectFlags) im->aspect;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;
    return NVSDK_NGX_Create_ImageView_Resource_VK((VkImageView) im->view, (VkImage) im->image, range,
                                                  (VkFormat) im->format, (unsigned) im->width, (unsigned) im->height,
                                                  readWrite);
}

/** Last NGX result code (0x1 = success, 0xBAD0xxxx = errors), for the log. */
PD_EXPORT int pd_last_result() {
    return g_last;
}

/**
 * Starts NGX on the game's Vulkan device. dllDir = folder with nvngx_dlss.dll, dataDir = folder for NGX logs.
 * Returns 1 if DLSS is available on this GPU and driver, 0 if not, negative on error.
 */
PD_EXPORT int pd_init(unsigned long long instance, unsigned long long physicalDevice, unsigned long long device,
                      unsigned long long gipa, unsigned long long gdpa,
                      const wchar_t* dllDir, const wchar_t* dataDir) {
    if (g_init) return 1;
    const wchar_t* paths[1] = { dllDir };
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = nullptr;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
            "8f2c6a1e-4b3d-4e5f-9a7b-1c2d3e4f5a6b", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dataDir,
            (VkInstance) instance, (VkPhysicalDevice) physicalDevice, (VkDevice) device,
            (PFN_vkGetInstanceProcAddr) gipa, (PFN_vkGetDeviceProcAddr) gdpa, &info, NVSDK_NGX_Version_API);
    g_last = (int) r;
    if (NVSDK_NGX_FAILED(r)) return -1;
    g_device = (VkDevice) device;

    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_params);
    g_last = (int) r;
    if (NVSDK_NGX_FAILED(r) || g_params == nullptr) {
        NVSDK_NGX_VULKAN_Shutdown1(g_device);
        return -2;
    }
    g_init = true;

    int available = 0;
    r = NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    g_last = (int) r;
    return (NVSDK_NGX_SUCCEED(r) && available) ? 1 : 0;
}

/** Render size DLSS recommends for an output size and a quality mode. Returns 1 on success. */
PD_EXPORT int pd_optimal(int outW, int outH, int quality, int* renderW, int* renderH) {
    if (!g_init) return 0;
    unsigned rw = 0, rh = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
    float sharpness = 0.f;
    NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_params, (unsigned) outW, (unsigned) outH,
                                                       (NVSDK_NGX_PerfQuality_Value) quality,
                                                       &rw, &rh, &maxW, &maxH, &minW, &minH, &sharpness);
    g_last = (int) r;
    if (NVSDK_NGX_FAILED(r) || rw == 0 || rh == 0) return 0;
    *renderW = (int) rw;
    *renderH = (int) rh;
    return 1;
}

/** Releases the DLSS feature (before making it again for another size or mode). */
PD_EXPORT void pd_release() {
    if (g_dlss) {
        NVSDK_NGX_VULKAN_ReleaseFeature(g_dlss);
        g_dlss = nullptr;
    }
}

/** Makes the DLSS feature, recorded into the command buffer. flags = NVSDK_NGX_DLSS_Feature_Flags. */
PD_EXPORT int pd_create(unsigned long long cmd, int renderW, int renderH, int outW, int outH, int quality, int flags) {
    if (!g_init) return 0;
    pd_release();
    NVSDK_NGX_DLSS_Create_Params p = {};
    p.Feature.InWidth = (unsigned) renderW;
    p.Feature.InHeight = (unsigned) renderH;
    p.Feature.InTargetWidth = (unsigned) outW;
    p.Feature.InTargetHeight = (unsigned) outH;
    p.Feature.InPerfQualityValue = (NVSDK_NGX_PerfQuality_Value) quality;
    p.InFeatureCreateFlags = flags;
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(g_device, (VkCommandBuffer) cmd, 1, 1, &g_dlss, g_params, &p);
    g_last = (int) r;
    return NVSDK_NGX_SUCCEED(r) ? 1 : 0;
}

/**
 * Runs DLSS for one frame, recorded into the command buffer. Jitter is in render pixels, motion vectors are
 * multiplied by (mvScaleX, mvScaleY) to get render pixels.
 */
PD_EXPORT int pd_evaluate(unsigned long long cmd, const PdImage* color, const PdImage* depth, const PdImage* motion,
                          const PdImage* output, float jitterX, float jitterY, float mvScaleX, float mvScaleY,
                          int reset) {
    if (!g_init || !g_dlss) return 0;
    NVSDK_NGX_Resource_VK rColor = toResource(color, false);
    NVSDK_NGX_Resource_VK rDepth = toResource(depth, false);
    NVSDK_NGX_Resource_VK rMotion = toResource(motion, false);
    NVSDK_NGX_Resource_VK rOut = toResource(output, true);

    NVSDK_NGX_VK_DLSS_Eval_Params e = {};
    e.Feature.pInColor = &rColor;
    e.Feature.pInOutput = &rOut;
    e.Feature.InSharpness = 0.f;
    e.pInDepth = &rDepth;
    e.pInMotionVectors = &rMotion;
    e.InJitterOffsetX = jitterX;
    e.InJitterOffsetY = jitterY;
    e.InRenderSubrectDimensions.Width = (unsigned) color->width;
    e.InRenderSubrectDimensions.Height = (unsigned) color->height;
    e.InReset = reset;
    e.InMVScaleX = mvScaleX;
    e.InMVScaleY = mvScaleY;
    e.InPreExposure = 1.f;
    e.InExposureScale = 1.f;
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT((VkCommandBuffer) cmd, g_dlss, g_params, &e);
    g_last = (int) r;
    return NVSDK_NGX_SUCCEED(r) ? 1 : 0;
}

// ---------------- DLSS Ray Reconstruction (DLSSD): denoise + upscale in one pass ----------------
static NVSDK_NGX_Handle* g_rr = nullptr;

/** 1 if DLSS Ray Reconstruction is available on this GPU and driver. */
PD_EXPORT int pd_rr_available() {
    if (!g_init) return 0;
    int available = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
    g_last = (int) r;
    return (NVSDK_NGX_SUCCEED(r) && available) ? 1 : 0;
}

/** Render size Ray Reconstruction expects for an output size and a quality mode. Returns 1 on success. */
PD_EXPORT int pd_rr_optimal(int outW, int outH, int quality, int* renderW, int* renderH) {
    if (!g_init) return 0;
    unsigned rw = 0, rh = 0;
    NVSDK_NGX_Result r = PD_DLSSD_GET_OPTIMAL_SETTINGS(g_params, (unsigned) outW, (unsigned) outH,
                                                       (NVSDK_NGX_PerfQuality_Value) quality, &rw, &rh);
    g_last = (int) r;
    if (NVSDK_NGX_FAILED(r) || rw == 0 || rh == 0) return 0;
    *renderW = (int) rw;
    *renderH = (int) rh;
    return 1;
}

/** Releases the Ray Reconstruction feature. */
PD_EXPORT void pd_rr_release() {
    if (g_rr) {
        NVSDK_NGX_VULKAN_ReleaseFeature(g_rr);
        g_rr = nullptr;
    }
}

/**
 * Makes the Ray Reconstruction feature, recorded into the command buffer. It must use the capability parameter
 * block (g_params): a fresh block fails. Roughness is packed in normals.w, depth is the hardware (reversed) depth.
 * At native size (render = output) the quality is DLAA (the upscaling modes are rejected at 1:1).
 */
PD_EXPORT int pd_rr_create(unsigned long long cmd, int renderW, int renderH, int outW, int outH, int quality, int flags) {
    if (!g_init) return 0;
    pd_rr_release();
    NVSDK_NGX_DLSSD_Create_Params p = {};
    p.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    p.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
    p.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
    p.InWidth = (unsigned) renderW;
    p.InHeight = (unsigned) renderH;
    p.InTargetWidth = (unsigned) outW;
    p.InTargetHeight = (unsigned) outH;
    p.InPerfQualityValue = (renderW == outW && renderH == outH) ? NVSDK_NGX_PerfQuality_Value_DLAA
                                                              : (NVSDK_NGX_PerfQuality_Value) quality;
    p.InFeatureCreateFlags = flags;
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSSD_EXT1(g_device, (VkCommandBuffer) cmd, 1, 1, &g_rr, g_params, &p);
    g_last = (int) r;
    return NVSDK_NGX_SUCCEED(r) ? 1 : 0;
}

/**
 * Runs Ray Reconstruction for one frame, recorded into the command buffer: noisy HDR color, hardware depth, motion
 * vectors, diffuse and specular albedo, world normals (roughness in w), all at render size; output at window size.
 * Matrices: 16 floats each, world -> view and view -> clip, in NGX's row-vector layout.
 */
PD_EXPORT int pd_rr_evaluate(unsigned long long cmd, const PdImage* color, const PdImage* depth, const PdImage* motion,
                             const PdImage* diffuse, const PdImage* specular, const PdImage* normals,
                             const PdImage* output, float jitterX, float jitterY, float mvScaleX, float mvScaleY,
                             int reset, float frameMs, float* worldToView, float* viewToClip) {
    if (!g_init || !g_rr) return 0;
    NVSDK_NGX_Resource_VK rColor = toResource(color, false);
    NVSDK_NGX_Resource_VK rDepth = toResource(depth, false);
    NVSDK_NGX_Resource_VK rMotion = toResource(motion, false);
    NVSDK_NGX_Resource_VK rDiffuse = toResource(diffuse, false);
    NVSDK_NGX_Resource_VK rSpecular = toResource(specular, false);
    NVSDK_NGX_Resource_VK rNormals = toResource(normals, false);
    NVSDK_NGX_Resource_VK rOut = toResource(output, true);

    NVSDK_NGX_VK_DLSSD_Eval_Params e = {};
    e.pInColor = &rColor;
    e.pInOutput = &rOut;
    e.pInDepth = &rDepth;
    e.pInMotionVectors = &rMotion;
    e.pInDiffuseAlbedo = &rDiffuse;
    e.pInSpecularAlbedo = &rSpecular;
    e.pInNormals = &rNormals;
    e.pInWorldToViewMatrix = worldToView;
    e.pInViewToClipMatrix = viewToClip;
    e.InJitterOffsetX = jitterX;
    e.InJitterOffsetY = jitterY;
    e.InMVScaleX = mvScaleX;
    e.InMVScaleY = mvScaleY;
    e.InReset = reset;
    e.InRenderSubrectDimensions.Width = (unsigned) color->width;
    e.InRenderSubrectDimensions.Height = (unsigned) color->height;
    e.InFrameTimeDeltaInMsec = frameMs;
    e.InPreExposure = 1.f;
    e.InExposureScale = 1.f;
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSSD_EXT((VkCommandBuffer) cmd, g_rr, g_params, &e);
    g_last = (int) r;
    return NVSDK_NGX_SUCCEED(r) ? 1 : 0;
}

/** Shuts NGX down (on exit). */
PD_EXPORT void pd_shutdown() {
    pd_rr_release();
    pd_release();
    if (g_init) {
        if (g_params) NVSDK_NGX_VULKAN_DestroyParameters(g_params);
        g_params = nullptr;
        NVSDK_NGX_VULKAN_Shutdown1(g_device);
        g_init = false;
    }
}
