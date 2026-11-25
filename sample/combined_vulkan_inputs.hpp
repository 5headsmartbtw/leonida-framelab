// SPDX-License-Identifier: MIT
#pragma once

#include <vulkan/vulkan.h>
#include <array>

inline bool supportsFrameInputRemap(VkPhysicalDevice device, VkFormat depth, VkFormat motion)
{
    constexpr VkFormatFeatureFlags required=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;
    for(const auto format:{depth,motion}) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device,format,&properties);
        if((properties.optimalTilingFeatures&required)!=required)return false;
    }
    return true;
}

inline void remapFrameInput(VkCommandBuffer cmd,VkImage src,VkImage dst,VkExtent2D sourceExtent,
                            VkExtent2D targetExtent,VkImageLayout attachment,VkImageAspectFlags aspects,
                            VkImageAspectFlags copyAspect,bool destinationReady)
{
    std::array<VkImageMemoryBarrier,2> barriers{};
    for(auto& barrier:barriers) {
        barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange={aspects,0,1,0,1};
        barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    }
    auto& beforeSource=barriers[0];
    beforeSource.image=src;
    beforeSource.oldLayout=attachment;
    beforeSource.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    beforeSource.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    auto& beforeTarget=barriers[1];
    beforeTarget.image=dst;
    beforeTarget.oldLayout=destinationReady?attachment:VK_IMAGE_LAYOUT_UNDEFINED;
    beforeTarget.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    beforeTarget.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,
                         0,nullptr,0,nullptr,uint32_t(barriers.size()),barriers.data());

    VkImageBlit region{};
    region.srcSubresource=region.dstSubresource={copyAspect,0,0,1};
    region.srcOffsets[1]={int32_t(sourceExtent.width),int32_t(sourceExtent.height),1};
    region.dstOffsets[1]={int32_t(targetExtent.width),int32_t(targetExtent.height),1};
    vkCmdBlitImage(cmd,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,&region,VK_FILTER_NEAREST);

    for(auto& barrier:barriers) {
        barrier.oldLayout=barrier.newLayout;
        barrier.newLayout=attachment;
        barrier.srcAccessMask=barrier.dstAccessMask;
        barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,
                         0,nullptr,0,nullptr,uint32_t(barriers.size()),barriers.data());
}
