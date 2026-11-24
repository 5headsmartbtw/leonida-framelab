// SPDX-License-Identifier: MIT
// Copyright (c) 2026 5headsmartbtw
#include "frame_fixture.hpp"
#include "../fg/FrameInputsVulkan.hpp"
#include <iostream>
using namespace frame_inputs;
#define CHECK(x) do{++checks;if(!(x)){std::cerr<<"FAILED line "<<__LINE__<<": "<<#x<<'\n';return 1;}}while(false)
int main(){unsigned checks=0;VulkanFrameInputs b;b.frame=test_fixture::valid();b.deviceId=b.queueId=b.commandStreamId=1;
    // Opaque handles are never called: these are binding-validation tests, not GPU execution.
    b.device=reinterpret_cast<VkDevice>(std::uintptr_t(1));b.queue=reinterpret_cast<VkQueue>(std::uintptr_t(2));b.queueFamily=0;b.producerCommands=reinterpret_cast<VkCommandBuffer>(std::uintptr_t(3));
    b.color={1,(VkImage)11,(VkImageView)12,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT,0,{1280,720}};
    b.depth={2,(VkImage)13,(VkImageView)14,VK_FORMAT_D24_UNORM_S8_UINT,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_ASPECT_DEPTH_BIT,0,{1280,720}};
    b.motionVectors={3,(VkImage)15,(VkImageView)16,VK_FORMAT_R16G16_SFLOAT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT,0,{1280,720}};
    const auto valid=[&](const VulkanFrameInputs& f){return validateVulkan(f,b.producerCommands,b.device,b.queue,0);};CHECK(valid(b));
    auto bad=b;bad.color.id=9;CHECK(!valid(bad));bad=b;bad.depth.view={};CHECK(!valid(bad));bad=b;bad.motionVectors.format=VK_FORMAT_R32G32_SFLOAT;CHECK(!valid(bad));
    bad=b;bad.depth.layout=VK_IMAGE_LAYOUT_GENERAL;CHECK(!valid(bad));bad=b;bad.color.queueFamily=1;CHECK(!valid(bad));bad=b;bad.queueFamily=1;CHECK(!valid(bad));
    bad=b;bad.frame.sync.completion=Completion::host_complete;CHECK(valid(bad).error==Error::unsupported);
    bad=b;bad.producerCommands=reinterpret_cast<VkCommandBuffer>(std::uintptr_t(33));CHECK(!valid(bad));bad=b;bad.commandStreamId=5;CHECK(!valid(bad));
    bad=b;bad.deviceId=7;CHECK(!valid(bad));bad=b;bad.depth.aspects|=VK_IMAGE_ASPECT_STENCIL_BIT;CHECK(!valid(bad));
    bad=b;bad.depth.image=bad.color.image;CHECK(!valid(bad));bad=b;bad.frame.hudless=bad.frame.color;CHECK(valid(bad).error==Error::unsupported);
    bad=b;bad.depth.extent.width=640;CHECK(!valid(bad));
    bad=b;bad.frame.camera.projectionSpace=ClipSpace::y_up_zero_to_one;CHECK(valid(bad).error==Error::unsupported);
    std::cout<<"PASS Vulkan binding checks="<<checks<<'\n';return 0;
}
