// SPDX-License-Identifier: MIT
#pragma once

#include <framelab/vulkan_sr.h>
#include <sl_consts.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

class PublicVulkanSrSample {
public:
    using Lease = std::unique_ptr<FrameLabLease, decltype(&framelab_lease_destroy)>;
    std::shared_ptr<void> deviceOwner;
    unsigned rejected{}, readbacks{}, resets{};

    bool create(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                uint32_t queueFamily, const std::filesystem::path& logs)
    {
        dev = device;
        physicalDevice = physical;
        family = queueFamily;
        logDir = logs;
        std::wstring path(32768, L'\0');
        const auto count = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (!count || count == path.size()) throw std::runtime_error("cannot find the SR sample executable");
        path.resize(count);
        const auto dll = std::filesystem::path(path).parent_path() / L"sl.interposer.dll";
        const auto owner = lease(deviceOwner);
        const FrameLabVulkanSrConfig config{sizeof config, FRAMELAB_ABI_VERSION, physical, device,
            queue, family, 1, dll.c_str(), owner.get(), report};
        const auto status = framelab_vulkan_sr_create(&config, &sr);
        if (status == FRAMELAB_OK) return true;
        std::printf("PUBLIC_VULKAN_SR_UNAVAILABLE status=%s\n", framelab_status_string(status));
        return false;
    }

    VkExtent2D configure(VkExtent2D size, bool hdr)
    {
        if (!sr) return size;
        const FrameLabVulkanSrOptions options{sizeof options, FRAMELAB_ABI_VERSION,
            {size.width, size.height}, FRAMELAB_SR_PERFORMANCE, uint32_t(hdr)};
        FrameLabExtent input{};
        const auto status = framelab_vulkan_sr_configure(sr, &options, &input);
        if (status != FRAMELAB_OK) {
            std::printf("PUBLIC_VULKAN_SR_UNAVAILABLE options=%s\n", framelab_status_string(status));
            return {};
        }
        reset = true;
        std::printf("PUBLIC_VULKAN_SR_CONFIGURE render=%ux%u output=%ux%u\n",
                    input.width, input.height, size.width, size.height);
        return {input.width, input.height};
    }

    bool active() const { return sr != nullptr; }

    static Lease lease(std::shared_ptr<const void> owner)
    {
        auto payload = std::make_unique<std::shared_ptr<const void>>(std::move(owner));
        FrameLabLease* value{};
        check(framelab_lease_create(payload.get(), [](void* value) {
            delete static_cast<std::shared_ptr<const void>*>(value);
        }, &value));
        payload.release();
        return {value, framelab_lease_destroy};
    }

    bool record(VkCommandBuffer command, uint32_t frame, bool omitDepth,
                const sl::Constants& c, const FrameLabVulkanSrImage& color,
                const FrameLabVulkanSrImage& depth, const FrameLabVulkanSrImage& motion,
                const FrameLabVulkanSrImage& output, bool readback)
    {
        FrameLabVulkanSrFrame input{sizeof input, FRAMELAB_ABI_VERSION, command, frame,
                                    uint32_t(reset), color, depth, motion, output, {}};
        if (omitDepth) input.depth = {};
        input.camera = camera(c);
        const auto result = framelab_vulkan_sr_record(sr, &input);
        if (result == FRAMELAB_MISSING_LEASE && omitDepth) {
            ++rejected;
            reset = true;
            std::printf("PUBLIC_VULKAN_SR_FALLBACK frame=%u missing_depth=1\n", frame);
            return false;
        }
        check(result);
        pending = true;
        capture = readback;
        captureFrame = frame;
        if (reset) {
            ++resets;
            std::printf("PUBLIC_VULKAN_SR_RESET frame=%u\n", frame);
        }
        reset = false;
        if (capture) recordReadback(command, output);
        return true;
    }

    VkResult submit(VkQueue queue, VkSubmitInfo info, VkFence fence)
    {
        if (!pending) return vkQueueSubmit(queue, 1, &info, fence);
        if (info.waitSemaphoreCount != 1 || info.signalSemaphoreCount != 1 || info.pNext)
            throw std::runtime_error("unexpected SR sample submit configuration");
        VkSemaphore timeline{};
        check(framelab_vulkan_sr_completion(sr, &timeline));
        const uint64_t waitValue = 0;
        const VkSemaphore signals[]{info.pSignalSemaphores[0], timeline};
        const uint64_t signalValues[]{0, ++ticket};
        const VkTimelineSemaphoreSubmitInfo values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            nullptr, 1, &waitValue, 2, signalValues};
        info.pNext = &values;
        info.signalSemaphoreCount = 2;
        info.pSignalSemaphores = signals;
        const auto result = vkQueueSubmit(queue, 1, &info, fence);
        if (result != VK_SUCCESS) {
            // The caller resets or discards the command buffer before asking the backend to cancel.
            return result;
        }
        check(framelab_vulkan_sr_mark_submitted(sr, ticket));
        const VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,nullptr,0,1,&timeline,&ticket};
        vkcheck(vkWaitSemaphores(dev, &wait, 30000000000ull));
        check(framelab_vulkan_sr_retire(sr));
        pending = false;
        if (capture) inspectReadback();
        return result;
    }

    void shutdown()
    {
        check(framelab_vulkan_sr_destroy(sr));
        sr = nullptr;
        releaseReadback();
        deviceOwner.reset();
    }

private:
    FrameLabVulkanSr* sr{};
    VkDevice dev{};
    VkPhysicalDevice physicalDevice{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
    VkDeviceSize bytes{};
    uint32_t family{}, captureFrame{};
    uint64_t ticket{};
    bool reset=true, pending=false, capture=false;
    std::filesystem::path logDir;

    static void check(FrameLabStatus status)
    {
        if (status != FRAMELAB_OK) throw std::runtime_error(framelab_status_string(status));
    }

    static void vkcheck(VkResult result)
    {
        if (result != VK_SUCCESS) throw std::runtime_error("SR sample Vulkan result " + std::to_string(result));
    }

    static void FRAMELAB_CALL report(void*, const char* stage, int32_t result)
    {
        if (result || std::strcmp(stage, "slIsFeatureSupported") == 0)
            std::printf("PUBLIC_VULKAN_SR_NATIVE stage=%s result=%d\n", stage, result);
    }

    static FrameLabCamera camera(const sl::Constants& c)
    {
        FrameLabCamera dst{};
        std::memcpy(dst.view_to_clip, &c.cameraViewToClip, sizeof dst.view_to_clip);
        std::memcpy(dst.clip_to_view, &c.clipToCameraView, sizeof dst.clip_to_view);
        std::memcpy(dst.clip_to_lens, &c.clipToLensClip, sizeof dst.clip_to_lens);
        std::memcpy(dst.clip_to_previous, &c.clipToPrevClip, sizeof dst.clip_to_previous);
        std::memcpy(dst.previous_to_clip, &c.prevClipToClip, sizeof dst.previous_to_clip);
        auto vector = [](const sl::float3& v, float (&out)[3]) { out[0]=v.x;out[1]=v.y;out[2]=v.z; };
        vector(c.cameraPos, dst.position);
        vector(c.cameraUp, dst.up);
        vector(c.cameraRight, dst.right);
        vector(c.cameraFwd, dst.forward);
        dst.jitter_pixels[0]=c.jitterOffset.x;dst.jitter_pixels[1]=c.jitterOffset.y;
        dst.motion_vector_scale[0]=c.mvecScale.x;dst.motion_vector_scale[1]=c.mvecScale.y;
        dst.near_plane=c.cameraNear;dst.far_plane=c.cameraFar;dst.fov_radians=c.cameraFOV;dst.aspect=c.cameraAspectRatio;
        dst.projection_space=FRAMELAB_CLIP_Y_DOWN_ZERO_TO_ONE;
        dst.reprojection_space=FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE;
        dst.motion_convention=FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV;
        dst.flags=FRAMELAB_CAMERA_VERIFIED;
        if(c.depthInverted==sl::Boolean::eTrue)dst.flags|=FRAMELAB_CAMERA_DEPTH_INVERTED;
        if(c.cameraMotionIncluded==sl::Boolean::eTrue)dst.flags|=FRAMELAB_CAMERA_MOTION_INCLUDED;
        if(c.orthographicProjection==sl::Boolean::eTrue)dst.flags|=FRAMELAB_CAMERA_ORTHOGRAPHIC;
        return dst;
    }

    void releaseReadback()
    {
        if (mapped) vkUnmapMemory(dev, memory);
        if (buffer) vkDestroyBuffer(dev, buffer, nullptr);
        if (memory) vkFreeMemory(dev, memory, nullptr);
        mapped=nullptr;buffer=VK_NULL_HANDLE;memory=VK_NULL_HANDLE;bytes=0;
    }

    void ensureReadback(VkDeviceSize size)
    {
        if (bytes == size) return;
        releaseReadback();
        const VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,nullptr,0,size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_SHARING_MODE_EXCLUSIVE};
        vkcheck(vkCreateBuffer(dev,&info,nullptr,&buffer));
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(dev,buffer,&requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice,&properties);
        constexpr auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type=0;
        while(type<properties.memoryTypeCount &&
              (!(requirements.memoryTypeBits&(1u<<type))||(properties.memoryTypes[type].propertyFlags&host)!=host))
            ++type;
        if(type==properties.memoryTypeCount)throw std::runtime_error("no coherent SR readback memory");
        const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,nullptr,requirements.size,type};
        vkcheck(vkAllocateMemory(dev,&allocation,nullptr,&memory));
        vkcheck(vkBindBufferMemory(dev,buffer,memory,0));
        vkcheck(vkMapMemory(dev,memory,0,size,0,&mapped));
        bytes=size;
    }

    void recordReadback(VkCommandBuffer cmd,const FrameLabVulkanSrImage& image)
    {
        const auto size=image.extent;
        ensureReadback(VkDeviceSize(size.width)*size.height*8);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout=image.layout;
        barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=image.image;
        barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,nullptr,0,nullptr,1,&barrier);
        const VkBufferImageCopy copy{0,0,0,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{size.width,size.height,1}};
        vkCmdCopyImageToBuffer(cmd,image.image,barrier.newLayout,buffer,1,&copy);
        std::swap(barrier.oldLayout,barrier.newLayout);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        const VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_HOST_BIT,
            0,1,&host,0,nullptr,1,&barrier);
    }

    void inspectReadback()
    {
        const auto* pixels=static_cast<const uint16_t*>(mapped);
        size_t nonfinite=0, different=0;
        const size_t count=size_t(bytes)/8;
        for(size_t i=0;i<count;++i) {
            different+=std::memcmp(pixels,pixels+4*i,8)!=0;
            for(size_t c=0;c<4;++c)nonfinite+=(pixels[4*i+c]&0x7c00)==0x7c00;
        }
        std::printf("PUBLIC_VULKAN_SR_READBACK frame=%u pixels=%zu nonfinite=%zu different=%zu\n",
                    captureFrame,count,nonfinite,different);
        if(nonfinite||!different)throw std::runtime_error("invalid Super Resolution readback");
        const auto path=logDir/(L"sdk-sr-"+std::to_wstring(captureFrame)+L".rgba16f");
        auto file=std::unique_ptr<FILE,decltype(&std::fclose)>(_wfopen(path.c_str(),L"wb"),std::fclose);
        if(!file||std::fwrite(mapped,1,size_t(bytes),file.get())!=bytes)
            throw std::runtime_error("could not write SR readback");
        ++readbacks;
    }
};
