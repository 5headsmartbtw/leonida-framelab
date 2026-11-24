// SPDX-License-Identifier: MIT
#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t width = 8;
constexpr uint32_t height = 8;
constexpr uint32_t expected = 0x1234abcd;
constexpr VkDeviceSize byteCount = width * height * sizeof(uint32_t);

void require(VkResult result, const char* action)
{
    if (result == VK_SUCCESS) return;
    std::fprintf(stderr, "%s: VkResult %d\n", action, result);
    throw std::runtime_error(action);
}

VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
    ++*static_cast<std::atomic_uint*>(user);
    std::fprintf(stderr, "Validation: %s\n", data->pMessage);
    return VK_FALSE;
}

struct Instance {
    VkInstance handle{};
    VkDebugUtilsMessengerEXT messenger{};
    std::atomic_uint messages{};
    ~Instance()
    {
        if (messenger) {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(handle, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(handle, messenger, nullptr);
        }
        if (handle) vkDestroyInstance(handle, nullptr);
    }
    void initialize()
    {
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "FrameLab synthetic handoff";
        app.apiVersion = VK_API_VERSION_1_2;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = validation;
        debug.pUserData = &messages;
        const auto synchronization = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        features.enabledValidationFeatureCount = 1;
        features.pEnabledValidationFeatures = &synchronization;
        features.pNext = &debug;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pNext = &features;
        create.pApplicationInfo = &app;
        create.enabledLayerCount = 1;
        create.ppEnabledLayerNames = &layer;
        create.enabledExtensionCount = 2;
        create.ppEnabledExtensionNames = extensions;
        require(vkCreateInstance(&create, nullptr, &handle), "create validated Vulkan instance");
        const auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(handle, "vkCreateDebugUtilsMessengerEXT"));
        if (!createDebug) throw std::runtime_error("debug utils entry point unavailable");
        require(createDebug(handle, &debug, nullptr, &messenger), "set validation callback");
    }
};

struct Trial {
    VkDevice device{};
    VkImage image{};
    VkBuffer readback{};
    VkDeviceMemory imageMemory{}, hostMemory{};
    VkCommandPool producePool{}, consumePool{};
    VkSemaphore semaphore{};
    VkFence fence{};

    ~Trial()
    {
        if (!device) return;
        vkDeviceWaitIdle(device);
        vkDestroyFence(device, fence, nullptr);
        vkDestroySemaphore(device, semaphore, nullptr);
        vkDestroyCommandPool(device, producePool, nullptr);
        vkDestroyCommandPool(device, consumePool, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkDestroyBuffer(device, readback, nullptr);
        vkFreeMemory(device, imageMemory, nullptr);
        vkFreeMemory(device, hostMemory, nullptr);
        vkDestroyDevice(device, nullptr);
    }

    void allocate(VkPhysicalDevice physical, VkMemoryRequirements requirements,
                  VkMemoryPropertyFlags properties, VkDeviceMemory& memory)
    {
        VkPhysicalDeviceMemoryProperties types{};
        vkGetPhysicalDeviceMemoryProperties(physical, &types);
        for (uint32_t i = 0; i < types.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (types.memoryTypes[i].propertyFlags & properties) == properties) {
                VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                info.allocationSize = requirements.size;
                info.memoryTypeIndex = i;
                require(vkAllocateMemory(device, &info, nullptr, &memory), "allocate Vulkan memory");
                return;
            }
        }
        throw std::runtime_error("suitable memory type unavailable");
    }

    VkCommandBuffer begin(uint32_t family, VkCommandPool& pool)
    {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = family;
        require(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "create command pool");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer command{};
        require(vkAllocateCommandBuffers(device, &alloc, &command), "allocate commands");
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        require(vkBeginCommandBuffer(command, &info), "begin commands");
        return command;
    }

    void run(VkPhysicalDevice physical, uint32_t from, uint32_t to, bool timeline)
    {
        const bool transfer = from != to;
        const float priority[]{1.0f, 1.0f};
        const VkDeviceQueueCreateInfo queues[]{
            {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, from, transfer ? 1u : 2u, priority},
            {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, to, 1, priority}
        };
        VkPhysicalDeviceTimelineSemaphoreFeatures feature{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        feature.timelineSemaphore = timeline;
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create.pNext = &feature;
        create.queueCreateInfoCount = transfer ? 2 : 1;
        create.pQueueCreateInfos = queues;
        require(vkCreateDevice(physical, &create, nullptr, &device), "create queue pair");
        VkQueue producer{}, consumer{};
        vkGetDeviceQueue(device, from, 0, &producer);
        vkGetDeviceQueue(device, to, transfer ? 0 : 1, &consumer);
        if (!producer || !consumer || producer == consumer)
            throw std::runtime_error("expected two distinct queues");

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32_UINT;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        require(vkCreateImage(device, &imageInfo, nullptr, &image), "create synthetic input image");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        allocate(physical, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, imageMemory);
        require(vkBindImageMemory(device, image, imageMemory, 0), "bind image memory");
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = byteCount;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        require(vkCreateBuffer(device, &bufferInfo, nullptr, &readback), "create readback buffer");
        vkGetBufferMemoryRequirements(device, readback, &requirements);
        allocate(physical, requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, hostMemory);
        require(vkBindBufferMemory(device, readback, hostMemory, 0), "bind readback memory");

        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const auto produce = begin(from, producePool);
        VkImageMemoryBarrier layout{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        layout.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        layout.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        layout.srcQueueFamilyIndex = layout.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        layout.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        layout.image = image;
        layout.subresourceRange = range;
        vkCmdPipelineBarrier(produce, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &layout);
        VkClearColorValue value{};
        value.uint32[0] = expected;
        vkCmdClearColorImage(produce, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
        layout.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        layout.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        layout.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        layout.dstAccessMask = transfer ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
        if (transfer) {
            layout.srcQueueFamilyIndex = from;
            layout.dstQueueFamilyIndex = to;
        }
        vkCmdPipelineBarrier(produce, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             transfer ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &layout);
        require(vkEndCommandBuffer(produce), "close producer commands");

        const auto consume = begin(to, consumePool);
        if (transfer) {
            layout.srcAccessMask = 0;
            layout.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(consume, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &layout);
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(consume, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        require(vkEndCommandBuffer(consume), "close consumer commands");

        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        semInfo.pNext = timeline ? &type : nullptr;
        require(vkCreateSemaphore(device, &semInfo, nullptr, &semaphore), "create producer semaphore");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        require(vkCreateFence(device, &fenceInfo, nullptr, &fence), "create consumer fence");
        const uint64_t ticket = 7;
        VkTimelineSemaphoreSubmitInfo tickets{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        tickets.signalSemaphoreValueCount = 1;
        tickets.pSignalSemaphoreValues = &ticket;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.pNext = timeline ? &tickets : nullptr;
        submit.commandBufferCount = submit.signalSemaphoreCount = 1;
        submit.pCommandBuffers = &produce;
        submit.pSignalSemaphores = &semaphore;
        require(vkQueueSubmit(producer, 1, &submit, VK_NULL_HANDLE), "submit producer");
        tickets.signalSemaphoreValueCount = 0;
        tickets.waitSemaphoreValueCount = 1;
        tickets.pWaitSemaphoreValues = &ticket;
        submit.signalSemaphoreCount = 0;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &semaphore;
        // Covers the acquire barrier as well as the copy on the consumer queue.
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        submit.pWaitDstStageMask = &waitStage;
        submit.pCommandBuffers = &consume;
        require(vkQueueSubmit(consumer, 1, &submit, fence), "submit waiting consumer");
        require(vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000), "wait for consumer fence");

        void* data{};
        require(vkMapMemory(device, hostMemory, 0, byteCount, 0, &data), "map consumer result");
        const auto* pixels = static_cast<const uint32_t*>(data);
        bool identical = true;
        for (uint32_t i = 0; i < width * height; ++i) identical &= pixels[i] == expected;
        vkUnmapMemory(device, hostMemory);
        if (!identical) throw std::runtime_error("consumer image differed from producer");
        std::printf("PASS %s: family %u -> %u, %u pixels, consumer fence completed\n",
                    timeline ? "timeline" : "binary", from, to, width * height);
    }
};
} // namespace

int main()
{
    try {
        Instance instance;
        instance.initialize();
        uint32_t deviceCount = 0;
        require(vkEnumeratePhysicalDevices(instance.handle, &deviceCount, nullptr), "count physical devices");
        std::vector<VkPhysicalDevice> devices(deviceCount);
        require(vkEnumeratePhysicalDevices(instance.handle, &deviceCount, devices.data()), "enumerate devices");
        bool ran = false;
        for (auto physical : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(physical, &props);
            if (props.apiVersion < VK_API_VERSION_1_2) continue;
            VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &timeline;
            vkGetPhysicalDeviceFeatures2(physical, &features);
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());

            std::printf("Adapter: %s\n", props.deviceName);
            bool same = false, different = false;
            for (uint32_t from = 0; from < count; ++from) {
                if (!(families[from].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                for (uint32_t to = 0; to < count; ++to) {
                    constexpr auto transfer = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
                    if (!(families[to].queueFlags & transfer)) continue;
                    auto& completed = from == to ? same : different;
                    if (completed || (from == to && families[from].queueCount < 2)) continue;
                    Trial{}.run(physical, from, to, false);
                    if (timeline.timelineSemaphore) Trial{}.run(physical, from, to, true);
                    else std::puts("SKIP timeline semaphore unavailable");
                    completed = ran = true;
                }
            }
            if (!same) std::puts("SKIP two queues in one graphics family unavailable");
            if (!different) std::puts("SKIP distinct transfer-capable family unavailable");
            if (ran) break;
        }
        if (instance.messages) return 1;
        return ran ? 0 : 77;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Handoff failed: %s\n", error.what());
        return 1;
    }
}
