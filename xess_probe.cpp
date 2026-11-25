// Prints what Intel XeSS needs from the Vulkan instance and device on this computer (extensions and features).
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include "xess/sdk/inc/xess/xess_vk.h"

static void dumpBools(const char* name, const void* s, size_t size) {
    const VkBool32* b = (const VkBool32*) ((const char*) s + 16);
    size_t n = (size - 16) / 4;
    printf("  %s:", name);
    for (size_t i = 0; i < n; i++) if (b[i]) printf(" %zu", i);
    printf("\n");
}

int main() {
    uint32_t ic = 0; const char* const* iext = nullptr; uint32_t minApi = 0;
    xess_result_t r = xessVKGetRequiredInstanceExtensions(&ic, &iext, &minApi);
    printf("instance extensions (%d), min api %u.%u:\n", r, VK_API_VERSION_MAJOR(minApi), VK_API_VERSION_MINOR(minApi));
    for (uint32_t i = 0; i < ic; i++) printf("  %s\n", iext[i]);

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = ic;
    ici.ppEnabledExtensionNames = iext;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { printf("no instance\n"); return 1; }
    uint32_t pc = 1; VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &pc, &pd);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s\n", props.deviceName);

    uint32_t dc = 0; const char* const* dext = nullptr;
    r = xessVKGetRequiredDeviceExtensions(inst, pd, &dc, &dext);
    printf("device extensions (%d):\n", r);
    for (uint32_t i = 0; i < dc; i++) printf("  %s\n", dext[i]);

    void* features = nullptr;
    r = xessVKGetRequiredDeviceFeatures(inst, pd, &features);
    printf("device features (%d):\n", r);
    for (VkBaseOutStructure* s = (VkBaseOutStructure*) features; s; s = s->pNext) {
        switch (s->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
                dumpBools("features2 (VkPhysicalDeviceFeatures index)", s, 16 + sizeof(VkPhysicalDeviceFeatures)); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
                dumpBools("vulkan11", s, sizeof(VkPhysicalDeviceVulkan11Features)); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
                dumpBools("vulkan12", s, sizeof(VkPhysicalDeviceVulkan12Features)); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
                dumpBools("vulkan13", s, sizeof(VkPhysicalDeviceVulkan13Features)); break;
            default: printf("  sType %d\n", (int) s->sType);
        }
    }
    return 0;
}
