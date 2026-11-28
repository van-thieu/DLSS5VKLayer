#pragma once
#include <vulkan/vulkan.h>

namespace dlssnr {

// Opaque Streamline host wrapper. Uses WWM's sl.interposer.dll + its NR plugin.
struct SLHost;

// Model tuning passed into DLSSNROptions before every slEvaluateFeature.
struct SLNrOptions {
    float intensity = 1.0f;
    float localTone = 1.0f;
    float localStructure = 1.0f;
    float skinStructure = -1.0f;
    float globalTone = 1.0f;
    uint32_t style = 0;
    uint32_t preset = 0;
    uint32_t autoMask = 0;
};

SLHost* SLHostCreate(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                     const wchar_t* pluginDir, uint32_t applicationId);
bool SLHostEvaluate(SLHost* host, VkCommandBuffer cmd,
                    VkImage color, VkImageView colorView, VkFormat colorFormat,
                    VkImage output, VkImageView outputView, VkFormat outputFormat,
                    VkImage mvec, VkImageView mvecView, VkFormat mvecFormat,
                    VkImage depth, VkImageView depthView, VkFormat depthFormat,
                    uint32_t width, uint32_t height, const SLNrOptions& options);
void SLHostDestroy(SLHost* host);

}  // namespace dlssnr
