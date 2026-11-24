// SPDX-License-Identifier: MIT
// Copyright (c) 2026 5headsmartbtw
#pragma once
#include "FrameInputs.hpp"
#include <vulkan/vulkan.h>

namespace frame_inputs {
struct VulkanImage {
    Id id=0; VkImage image{}; VkImageView view{}; VkFormat format=VK_FORMAT_UNDEFINED;
    VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspects=0;
    std::uint32_t queueFamily=VK_QUEUE_FAMILY_IGNORED;
    Extent extent{};
};
struct VulkanFrameInputs {
    FrameInputs frame;
    Id deviceId=0,queueId=0,commandStreamId=0;
    VkDevice device{};VkQueue queue{};std::uint32_t queueFamily=VK_QUEUE_FAMILY_IGNORED;
    VkCommandBuffer producerCommands{};
    VulkanImage color,depth,motionVectors;
};
inline VkFormat vulkanFormat(Format f) {
    switch(f){
      case Format::rgba16_float:return VK_FORMAT_R16G16B16A16_SFLOAT;
      case Format::rgba8_unorm:return VK_FORMAT_R8G8B8A8_UNORM;
      case Format::bgra8_unorm:return VK_FORMAT_B8G8R8A8_UNORM;
      case Format::rg16_float:return VK_FORMAT_R16G16_SFLOAT;
      case Format::rg32_float:return VK_FORMAT_R32G32_SFLOAT;
      case Format::r32_float:return VK_FORMAT_R32_SFLOAT;
      case Format::d32_float:return VK_FORMAT_D32_SFLOAT;
      case Format::d24s8:return VK_FORMAT_D24_UNORM_S8_UINT;
      default:return VK_FORMAT_UNDEFINED;
    }
}
inline VkImageLayout vulkanLayout(State s){switch(s){case State::shader_read:return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;case State::color_attachment:return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;case State::depth_attachment:return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;case State::general:return VK_IMAGE_LAYOUT_GENERAL;default:return VK_IMAGE_LAYOUT_UNDEFINED;}}
inline Result validateVulkan(const VulkanFrameInputs& b,VkCommandBuffer consumer,VkDevice device,VkQueue queue,std::uint32_t family) {
    if(auto r=validate(b.frame);!r)return r;
    const auto& f=b.frame;
    if(!consumer||!b.device||b.device!=device||b.deviceId!=f.device||b.queue!=queue||!queue||b.queueFamily!=family||b.queueId!=f.sync.queue)
        return {Error::binding,"Vulkan device/queue binding mismatch"};
    // This backend implements only ordered recording in the SAME command buffer/queue.
    // External waits and queue-family ownership transfers must be implemented before accepting them.
    if(f.sync.completion!=Completion::same_command_stream||b.producerCommands!=consumer||b.commandStreamId!=f.sync.commandStream)
        return {Error::unsupported,"Vulkan completion mode/command stream unsupported"};
    const Image* neutral[]={&f.color,&f.depth,&f.motionVectors};const VulkanImage* native[]={&b.color,&b.depth,&b.motionVectors};
    for(int i=0;i<3;++i){const auto& n=*neutral[i];const auto& v=*native[i];
        if(n.id!=v.id||!v.image||!v.view||v.extent!=n.extent||v.format!=vulkanFormat(n.format)||v.layout!=vulkanLayout(n.state)||v.queueFamily!=family||n.ownerQueue!=b.queueId)
            return {Error::binding,"Vulkan image identity/format/layout/ownership mismatch"};
        const auto expected=i==1?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
        if(v.aspects!=expected)return {Error::binding,"Vulkan image view aspect mismatch"};
    }
    if(b.color.image==b.depth.image||b.color.image==b.motionVectors.image||b.depth.image==b.motionVectors.image)return {Error::binding,"Vulkan images alias"};
    if(f.hudless||f.ui||f.renderExtent!=f.outputExtent||f.color.format!=Format::rgba16_float||f.depth.format!=Format::d24s8||f.motionVectors.format!=Format::rg16_float)
        return {Error::unsupported,"Direct NGX profile requires native-size RGBA16F/D24S8/RG16F without UI inputs"};
    if(f.color.state!=State::shader_read||f.depth.state!=State::depth_attachment||f.motionVectors.state!=State::color_attachment)
        return {Error::unsupported,"Direct NGX input state profile unsupported"};
    if(f.camera.projectionSpace!=ClipSpace::y_down_zero_to_one||f.camera.reprojectionSpace!=ClipSpace::y_up_zero_to_one)
        return {Error::unsupported,"Direct NGX camera convention pair unsupported"};
    return {};
}
} // namespace frame_inputs
