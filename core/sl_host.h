#pragma once
#include <vulkan/vulkan.h>

namespace dlssnr {

// Opaque Streamline host wrapper. Uses WWM's sl.interposer.dll + its NR plugin.
struct SLHost;

SLHost* SLHostCreate(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                     const wchar_t* pluginDir, uint32_t applicationId);
bool SLHostEvaluate(SLHost* host, VkCommandBuffer cmd,
                    VkImage color, VkImageView colorView, VkFormat colorFormat,
                    VkImage output, VkImageView outputView, VkFormat outputFormat,
                    VkImage mvec, VkImageView mvecView, VkFormat mvecFormat,
                    VkImage depth, VkImageView depthView, VkFormat depthFormat,
                    uint32_t width, uint32_t height);
void SLHostDestroy(SLHost* host);

}  // namespace dlssnr
