// SPDX-License-Identifier: MIT
//
// NGX-free synchronization-validation calibration for FL-17.
//
// Each case records one command shape seen in the #24 diagnostics on FrameLab-owned synthetic
// resources. Cases that must stay clean are submitted and read back. Cases that intentionally omit
// a dependency are positive controls and must produce exactly one SyncVal hazard. The verdict is
// "observed equals expected" for every case. This fixture does not exercise NGX, Streamline or DLSS
// and cannot attribute the native diagnostics on its own.
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t width = 16;
constexpr uint32_t height = 16;
constexpr VkDeviceSize imageBytes = width * height * 4;
constexpr VkDeviceSize bufferBytes = 4096;
constexpr uint32_t clearWord = 0x5a3c1e0f;
constexpr uint32_t firstFill = 0x11111111;
constexpr uint32_t secondFill = 0x2222abcd;
constexpr const char* layerName = "VK_LAYER_KHRONOS_validation";
constexpr const char* region = "framelab.syncval.control";
constexpr const char* hazardWaw = "SYNC-HAZARD-WRITE-AFTER-WRITE";

void require(VkResult result, const char* action)
{
    if (result == VK_SUCCESS) return;
    std::fprintf(stderr, "%s: VkResult %d\n", action, result);
    throw std::runtime_error(action);
}

std::string version(uint32_t v)
{
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}

struct Message {
    uint32_t severity{};
    std::string id;
    std::string text;
};

struct Capture {
    bool inCase = false;
    std::vector<Message> messages;
    unsigned outside = 0;
    unsigned errors = 0;
    unsigned warnings = 0;
};

VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
    auto& capture = *static_cast<Capture*>(user);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++capture.errors;
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++capture.warnings;
    const char* text = data && data->pMessage ? data->pMessage : "(no message)";
    // Same line shape as the reference sample, so one parser reads both logs.
    std::printf("SAMPLE_VALIDATION severity=%u %s\n", unsigned(severity), text);
    std::fflush(stdout);
    if (capture.inCase) {
        capture.messages.push_back({unsigned(severity),
                                    data && data->pMessageIdName ? data->pMessageIdName : "", text});
    } else {
        ++capture.outside;
    }
    return VK_FALSE;
}

bool hasLayer(uint32_t& spec, uint32_t& implementation)
{
    uint32_t count = 0;
    require(vkEnumerateInstanceLayerProperties(&count, nullptr), "count instance layers");
    std::vector<VkLayerProperties> layers(count);
    require(vkEnumerateInstanceLayerProperties(&count, layers.data()), "enumerate instance layers");
    for (const auto& layer : layers) {
        if (std::strcmp(layer.layerName, layerName) == 0) {
            spec = layer.specVersion;
            implementation = layer.implementationVersion;
            return true;
        }
    }
    return false;
}

bool layerExposes(const char* extension)
{
    uint32_t count = 0;
    require(vkEnumerateInstanceExtensionProperties(layerName, &count, nullptr), "count layer extensions");
    std::vector<VkExtensionProperties> extensions(count);
    require(vkEnumerateInstanceExtensionProperties(layerName, &count, extensions.data()), "enumerate layer extensions");
    for (const auto& e : extensions)
        if (std::strcmp(e.extensionName, extension) == 0) return true;
    return false;
}

struct Instance {
    VkInstance handle{};
    VkDebugUtilsMessengerEXT messenger{};
    Capture capture;
    bool extraProperties = false;
    PFN_vkSetDebugUtilsObjectNameEXT setName{};
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel{};
    PFN_vkCmdEndDebugUtilsLabelEXT endLabel{};

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
        extraProperties = layerExposes(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
        std::vector<const char*> extensions{VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
        if (extraProperties) extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
        else extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "FrameLab SyncVal control";
        app.apiVersion = VK_API_VERSION_1_3;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = validation;
        debug.pUserData = &capture;

        // Settings are explicit so that per-case counts do not depend on the launch environment.
        const VkBool32 on = VK_TRUE;
        const uint32_t duplicateLimit = 10000;
        const std::array settings{
            VkLayerSettingEXT{layerName, "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on},
            VkLayerSettingEXT{layerName, "syncval_message_extra_properties", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on},
            VkLayerSettingEXT{layerName, "duplicate_message_limit", VK_LAYER_SETTING_TYPE_UINT32_EXT, 1, &duplicateLimit},
        };
        VkLayerSettingsCreateInfoEXT layerSettings{VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT};
        layerSettings.settingCount = uint32_t(settings.size());
        layerSettings.pSettings = settings.data();
        layerSettings.pNext = &debug;
        const auto synchronization = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        features.enabledValidationFeatureCount = 1;
        features.pEnabledValidationFeatures = &synchronization;
        features.pNext = &debug;

        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pNext = extraProperties ? static_cast<const void*>(&layerSettings) : &features;
        create.pApplicationInfo = &app;
        create.enabledLayerCount = 1;
        create.ppEnabledLayerNames = &layerName;
        create.enabledExtensionCount = uint32_t(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        require(vkCreateInstance(&create, nullptr, &handle), "create validated Vulkan instance");
        const auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(handle, "vkCreateDebugUtilsMessengerEXT"));
        setName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(handle, "vkSetDebugUtilsObjectNameEXT"));
        beginLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(handle, "vkCmdBeginDebugUtilsLabelEXT"));
        endLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(handle, "vkCmdEndDebugUtilsLabelEXT"));
        if (!createDebug || !setName || !beginLabel || !endLabel)
            throw std::runtime_error("debug utils entry points unavailable");
        require(createDebug(handle, &debug, nullptr, &messenger), "set validation callback");
    }
};

enum class Expect { none, waw };

struct Sync1 {
    VkPipelineStageFlags src, dst;
    VkAccessFlags dstAccess;
};
struct Sync2 {
    VkPipelineStageFlags2 dst;
    VkAccessFlags2 dstAccess;
};

struct Fixture {
    Instance& instance;
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family = 0;
    bool synchronization2 = false;
    VkImage image{};
    VkBuffer buffer{}, host{};
    VkDeviceMemory imageMemory{}, bufferMemory{}, hostMemory{};
    VkCommandPool pool{};
    VkFence fence{};

    explicit Fixture(Instance& owner) : instance(owner) {}
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    ~Fixture()
    {
        if (!device) return;
        vkDeviceWaitIdle(device);
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        vkDestroyBuffer(device, host, nullptr);
        vkFreeMemory(device, imageMemory, nullptr);
        vkFreeMemory(device, bufferMemory, nullptr);
        vkFreeMemory(device, hostMemory, nullptr);
        vkDestroyDevice(device, nullptr);
    }

    void allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags properties, VkDeviceMemory& memory)
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

    void name(VkObjectType type, uint64_t handle, const char* text)
    {
        VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        info.objectType = type;
        info.objectHandle = handle;
        info.pObjectName = text;
        require(instance.setName(device, &info), "name synthetic object");
    }

    void create(VkPhysicalDevice selected, uint32_t selectedFamily, bool sync2)
    {
        physical = selected;
        family = selectedFamily;
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceSynchronization2Features feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        feature.synchronization2 = sync2;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.pNext = sync2 ? &feature : nullptr;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queueInfo;
        require(vkCreateDevice(physical, &info, nullptr, &device), "create control device");
        synchronization2 = sync2;
        vkGetDeviceQueue(device, family, 0, &queue);

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UINT;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        require(vkCreateImage(device, &imageInfo, nullptr, &image), "create synthetic image");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, imageMemory);
        require(vkBindImageMemory(device, image, imageMemory, 0), "bind image memory");

        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = bufferBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        require(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer), "create synthetic buffer");
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bufferMemory);
        require(vkBindBufferMemory(device, buffer, bufferMemory, 0), "bind buffer memory");

        bufferInfo.size = bufferBytes > imageBytes ? bufferBytes : imageBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        require(vkCreateBuffer(device, &bufferInfo, nullptr, &host), "create readback buffer");
        vkGetBufferMemoryRequirements(device, host, &requirements);
        allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, hostMemory);
        require(vkBindBufferMemory(device, host, hostMemory, 0), "bind readback memory");

        name(VK_OBJECT_TYPE_IMAGE, uint64_t(image), "framelab.syncval.control.image");
        name(VK_OBJECT_TYPE_BUFFER, uint64_t(buffer), "framelab.syncval.control.buffer");

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        require(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "create command pool");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        require(vkCreateFence(device, &fenceInfo, nullptr, &fence), "create fence");
    }

    VkCommandBuffer begin()
    {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer command{};
        require(vkAllocateCommandBuffers(device, &alloc, &command), "allocate commands");
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        require(vkBeginCommandBuffer(command, &info), "begin commands");
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = region;
        instance.beginLabel(command, &label);
        return command;
    }

    void end(VkCommandBuffer command)
    {
        instance.endLabel(command);
        require(vkEndCommandBuffer(command), "end commands");
    }

    void submit(VkCommandBuffer command, VkFence signal = VK_NULL_HANDLE)
    {
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &command;
        require(vkQueueSubmit(queue, 1, &info, signal), "submit control commands");
    }

    void retire(std::initializer_list<VkCommandBuffer> commands)
    {
        require(vkQueueWaitIdle(queue), "wait for control queue");
        for (auto command : commands) vkFreeCommandBuffers(device, pool, 1, &command);
    }

    // --- image cases -------------------------------------------------------------------------
    static constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    void transitionSync1(VkCommandBuffer command, const Sync1& s, VkImageLayout layout)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.dstAccessMask = s.dstAccess;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = layout;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = range;
        vkCmdPipelineBarrier(command, s.src, s.dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void transitionSync2(VkCommandBuffer command, const Sync2& s, VkImageLayout layout)
    {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstStageMask = s.dst;
        barrier.dstAccessMask = s.dstAccess;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = layout;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = range;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
    }

    void clear(VkCommandBuffer command, VkImageLayout layout)
    {
        VkClearColorValue value{};
        for (unsigned i = 0; i < 4; ++i) value.uint32[i] = (clearWord >> (8 * i)) & 0xff;
        vkCmdClearColorImage(command, image, layout, &value, 1, &range);
    }

    void copyImageToHost(VkCommandBuffer command, VkImageLayout layout)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = range;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, host, 1, &copy);
        hostBarrier(command);
    }

    void hostBarrier(VkCommandBuffer command)
    {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                             nullptr, 0, nullptr);
    }

    bool hostWordsEqual(VkDeviceSize bytes, uint32_t expected)
    {
        void* data{};
        require(vkMapMemory(device, hostMemory, 0, bytes, 0, &data), "map readback");
        const auto* words = static_cast<const uint32_t*>(data);
        bool equal = true;
        for (VkDeviceSize i = 0; i < bytes / 4; ++i) equal &= words[i] == expected;
        vkUnmapMemory(device, hostMemory);
        return equal;
    }

    // Records the transition and clear. A clean case is submitted and its pixels are checked.
    std::string imageCase(const std::function<void(VkCommandBuffer)>& transition, VkImageLayout layout, Expect expect)
    {
        const auto command = begin();
        transition(command);
        clear(command, layout);
        if (expect == Expect::waw) {
            end(command);
            vkFreeCommandBuffers(device, pool, 1, &command);  // record-time control; never executed
            return "n/a";
        }
        copyImageToHost(command, layout);
        end(command);
        submit(command);
        retire({command});
        uint32_t texel = clearWord;  // little-endian R8G8B8A8_UINT
        return hostWordsEqual(imageBytes, texel) ? "pass" : "fail";
    }

    // --- buffer cases ------------------------------------------------------------------------
    void fill(VkCommandBuffer command, uint32_t value) { vkCmdFillBuffer(command, buffer, 0, bufferBytes, value); }

    void transferBarrier(VkCommandBuffer command)
    {
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                             &barrier, 0, nullptr);
    }

    // Synchronization2 buffer barrier limited to one stage on both sides.
    void stageBarrier2(VkCommandBuffer command, VkPipelineStageFlags2 stage)
    {
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.srcStageMask = barrier.dstStageMask = stage;
        barrier.srcAccessMask = barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.size = VK_WHOLE_SIZE;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
    }

    std::string fillBarrierFill(VkPipelineStageFlags2 stage, bool execute)
    {
        const auto command = begin();
        fill(command, firstFill);
        stageBarrier2(command, stage);
        fill(command, secondFill);
        if (!execute) {
            end(command);
            vkFreeCommandBuffers(device, pool, 1, &command);  // record-time control; never executed
            return "n/a";
        }
        copyBufferToHost(command);
        end(command);
        submit(command);
        retire({command});
        return hostWordsEqual(bufferBytes, secondFill) ? "pass" : "fail";
    }

    void copyBufferToHost(VkCommandBuffer command)
    {
        transferBarrier(command);
        const VkBufferCopy copy{0, 0, bufferBytes};
        vkCmdCopyBuffer(command, buffer, host, 1, &copy);
        hostBarrier(command);
    }

    enum class Between { queueOrder, hostFence, leadingBarrier };

    // Two submissions on one queue, each filling the same buffer, mirroring a private buffer that is
    // filled again by a later frame. Only the dependency between the submissions changes.
    std::string crossSubmission(Between between)
    {
        const auto first = begin();
        fill(first, firstFill);
        end(first);
        const auto second = begin();
        if (between == Between::leadingBarrier) transferBarrier(second);
        fill(second, secondFill);
        const bool verify = between != Between::queueOrder;
        if (verify) copyBufferToHost(second);
        end(second);
        if (between == Between::hostFence) {
            submit(first, fence);
            require(vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000), "wait for first submission");
            require(vkResetFences(device, 1, &fence), "reset fence");
        } else {
            submit(first);
        }
        submit(second);
        retire({first, second});
        if (!verify) return "n/a";
        return hostWordsEqual(bufferBytes, secondFill) ? "pass" : "fail";
    }
};

struct Case {
    const char* name;
    Expect expect;
    const char* timing;  // where SyncVal is expected to evaluate the dependency
    bool sync2;
    // Extra-property lines that a positive control must contain (checked when available).
    std::vector<const char*> properties;
    std::function<std::string(Fixture&)> run;
    // Layer revisions that classify an access differently report differently. A positive control
    // expects its hazard only for layer spec versions in [hazardSince, hazardUntil).
    uint32_t hazardSince = 0;
    uint32_t hazardUntil = UINT32_MAX;
};

std::vector<Case> cases()
{
    const auto dst = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const auto general = VK_IMAGE_LAYOUT_GENERAL;
    const auto sync1 = [](Sync1 s, VkImageLayout layout, Expect expect) {
        return [=](Fixture& f) {
            return f.imageCase([&](VkCommandBuffer c) { f.transitionSync1(c, s, layout); }, layout, expect);
        };
    };
    const auto sync2 = [](Sync2 s, VkImageLayout layout, Expect expect) {
        return [=](Fixture& f) {
            return f.imageCase([&](VkCommandBuffer c) { f.transitionSync2(c, s, layout); }, layout, expect);
        };
    };
    const std::vector<const char*> transitionClear{
        "hazard_type = WRITE_AFTER_WRITE", "prior_access = SYNC_IMAGE_LAYOUT_TRANSITION",
        "command = vkCmdClearColorImage", "prior_debug_region = framelab.syncval.control"};
    auto transitionClear1 = transitionClear;
    transitionClear1.push_back("prior_command = vkCmdPipelineBarrier");
    auto transitionClear2 = transitionClear;
    transitionClear2.push_back("prior_command = vkCmdPipelineBarrier2");

    return {
        {"image-transition-clear/sync1-transfer-write", Expect::none, "record", false, {},
         sync1({VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT}, dst,
               Expect::none)},
        {"image-transition-clear/sync2-clear-write", Expect::none, "record", true, {},
         sync2({VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT}, dst, Expect::none)},
        {"image-transition-clear/sync2-all-commands", Expect::none, "record", true, {},
         sync2({VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT}, general, Expect::none)},
        {"image-transition-clear/sync1-no-dst-access", Expect::waw, "record", false, transitionClear1,
         sync1({VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0}, dst, Expect::waw)},
        {"image-transition-clear/sync1-bottom-of-pipe", Expect::waw, "record", false, transitionClear1,
         sync1({VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0}, dst, Expect::waw)},
        {"image-transition-clear/sync1-compute-write", Expect::waw, "record", false, transitionClear1,
         sync1({VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT},
               general, Expect::waw)},
        {"image-transition-clear/sync2-copy-stage", Expect::waw, "record", true, transitionClear2,
         sync2({VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT}, dst, Expect::waw)},
        {"buffer-fill-fill/no-barrier", Expect::waw, "record", false,
         {"hazard_type = WRITE_AFTER_WRITE", "command = vkCmdFillBuffer", "prior_command = vkCmdFillBuffer"},
         [](Fixture& f) {
             const auto command = f.begin();
             f.fill(command, firstFill);
             f.fill(command, secondFill);
             f.end(command);
             vkFreeCommandBuffers(f.device, f.pool, 1, &command);  // record-time control; never executed
             return std::string("n/a");
         }},
        {"buffer-fill-fill/transfer-barrier", Expect::none, "record", false, {},
         [](Fixture& f) {
             const auto command = f.begin();
             f.fill(command, firstFill);
             f.transferBarrier(command);
             f.fill(command, secondFill);
             f.copyBufferToHost(command);
             f.end(command);
             f.submit(command);
             f.retire({command});
             return std::string(f.hostWordsEqual(bufferBytes, secondFill) ? "pass" : "fail");
         }},
        // Khronos validation layers before SDK 1.3.280 (commit 8d93fca) classified vkCmdFillBuffer as a
        // COPY-stage write; later layers use VK_PIPELINE_STAGE_2_CLEAR_BIT.
        {"buffer-fill-fill/sync2-copy-stage", Expect::waw, "record", true,
         {"hazard_type = WRITE_AFTER_WRITE", "command = vkCmdFillBuffer", "prior_command = vkCmdFillBuffer",
          "write_barriers = 0"},
         [](Fixture& f) { return f.fillBarrierFill(VK_PIPELINE_STAGE_2_COPY_BIT, false); }, VK_MAKE_API_VERSION(0, 1, 3, 280)},
        // The same layers report this spec-conforming CLEAR-stage barrier as a hazard.
        {"buffer-fill-fill/sync2-clear-stage", Expect::waw, "record", true, {},
         [](Fixture& f) { return f.fillBarrierFill(VK_PIPELINE_STAGE_2_CLEAR_BIT, true); }, 0,
         VK_MAKE_API_VERSION(0, 1, 3, 280)},
        {"buffer-fill-submit/queue-order-only", Expect::waw, "submit", false,
         {"message_type = SubmitTimeError", "hazard_type = WRITE_AFTER_WRITE", "command = vkCmdFillBuffer",
          "prior_command = vkCmdFillBuffer"},
         [](Fixture& f) { return f.crossSubmission(Fixture::Between::queueOrder); }},
        {"buffer-fill-submit/host-fence-wait", Expect::none, "submit", false, {},
         [](Fixture& f) { return f.crossSubmission(Fixture::Between::hostFence); }},
        {"buffer-fill-submit/leading-barrier", Expect::none, "submit", false, {},
         [](Fixture& f) { return f.crossSubmission(Fixture::Between::leadingBarrier); }},
    };
}

const char* expectName(Expect e) { return e == Expect::waw ? "WRITE_AFTER_WRITE" : "none"; }

struct Options {
    int device = -1;
    std::string filter;
    bool list = false;
};

Options parse(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--device" && i + 1 < argc) options.device = std::atoi(argv[++i]);
        else if (arg == "--case" && i + 1 < argc) options.filter = argv[++i];
        else if (arg == "--list") options.list = true;
        else throw std::runtime_error("usage: vulkan_syncval_control [--list] [--device N] [--case NAME]");
    }
    return options;
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parse(argc, argv);
        const auto all = cases();
        if (!options.filter.empty()) {
            bool known = false;
            for (const auto& c : all) known |= options.filter == c.name;
            if (!known) throw std::runtime_error("unknown case: " + options.filter);
        }
        if (options.list) {
            for (const auto& c : all) {
                std::printf("%s expect=%s timing=%s sync2=%d", c.name, expectName(c.expect), c.timing, int(c.sync2));
                if (c.hazardSince) std::printf(" hazard_since=%s", version(c.hazardSince).c_str());
                if (c.hazardUntil != UINT32_MAX) std::printf(" hazard_until=%s", version(c.hazardUntil).c_str());
                std::printf("\n");
            }
            return 0;
        }
        uint32_t spec = 0, implementation = 0;
        if (!hasLayer(spec, implementation)) {
            std::puts("SKIP VK_LAYER_KHRONOS_validation unavailable");
            return 77;
        }
        Instance instance;
        instance.initialize();
        std::printf("SYNCVAL_CONTROL_LAYER name=%s spec=%s implementation=%u extra_properties=%d\n", layerName,
                    version(spec).c_str(), implementation, int(instance.extraProperties));

        uint32_t deviceCount = 0;
        require(vkEnumeratePhysicalDevices(instance.handle, &deviceCount, nullptr), "count physical devices");
        std::vector<VkPhysicalDevice> devices(deviceCount);
        require(vkEnumeratePhysicalDevices(instance.handle, &deviceCount, devices.data()), "enumerate devices");

        Fixture fixture(instance);
        bool selected = false;
        for (uint32_t index = 0; index < deviceCount && !selected; ++index) {
            if (options.device >= 0 && uint32_t(options.device) != index) continue;
            VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            props.pNext = &driver;
            vkGetPhysicalDeviceProperties2(devices[index], &props);
            if (props.properties.apiVersion < VK_API_VERSION_1_2) {
                std::printf("SKIP device %u: Vulkan 1.2 unavailable\n", index);
                continue;
            }
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &count, families.data());
            uint32_t family = count;
            for (uint32_t i = 0; i < count && family == count; ++i)
                if (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) family = i;
            if (family == count) {
                std::printf("SKIP device %u: no graphics or compute queue\n", index);
                continue;
            }
            VkPhysicalDeviceSynchronization2Features sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &sync2;
            const bool core13 = props.properties.apiVersion >= VK_API_VERSION_1_3;
            if (core13) vkGetPhysicalDeviceFeatures2(devices[index], &features);
            std::printf("SYNCVAL_CONTROL_DEVICE index=%u name=\"%s\" api=%s vendor=0x%04x device=0x%04x driver_id=%u "
                        "driver=\"%s\" driver_info=\"%s\" queue_family=%u synchronization2=%d\n",
                        index, props.properties.deviceName, version(props.properties.apiVersion).c_str(),
                        props.properties.vendorID, props.properties.deviceID, unsigned(driver.driverID),
                        driver.driverName, driver.driverInfo, family, int(core13 && sync2.synchronization2));
            fixture.create(devices[index], family, core13 && sync2.synchronization2);
            selected = true;
        }
        if (!selected) {
            std::puts("SKIP no suitable Vulkan device");
            return 77;
        }

        unsigned ran = 0, passed = 0, failed = 0, skipped = 0;
        for (const auto& c : all) {
            if (!options.filter.empty() && options.filter != c.name) continue;
            if (c.sync2 && !fixture.synchronization2) {
                std::printf("SYNCVAL_CONTROL_SKIP case=%s reason=synchronization2-unavailable\n", c.name);
                ++skipped;
                continue;
            }
            const bool inRange = spec >= c.hazardSince && spec < c.hazardUntil;
            const Expect expect = c.expect == Expect::waw && !inRange ? Expect::none : c.expect;
            std::printf("SYNCVAL_CONTROL_BEGIN case=%s expect=%s timing=%s\n", c.name, expectName(expect), c.timing);
            std::fflush(stdout);
            instance.capture.messages.clear();
            instance.capture.inCase = true;
            const std::string readback = c.run(fixture);
            instance.capture.inCase = false;
            const auto& messages = instance.capture.messages;

            unsigned hazards = 0;
            bool unexpected = false, absent = false;
            std::string missing;
            for (const auto& m : messages) {
                if (m.id == hazardWaw) ++hazards;
                else unexpected = true;
                if (m.id != hazardWaw) continue;
                // Layers without the extra-properties section still get a strict hazard-count verdict.
                if (m.text.find("[Extra properties]") == std::string::npos) {
                    absent = true;
                    continue;
                }
                for (const char* p : c.properties)
                    if (m.text.find(p) == std::string::npos) missing += missing.empty() ? p : std::string(";") + p;
            }
            const unsigned want = expect == Expect::waw ? 1u : 0u;
            const bool ok = !unexpected && hazards == want && messages.size() == want && missing.empty() &&
                            readback != "fail";
            std::printf("SYNCVAL_CONTROL_END case=%s expect=%s messages=%zu hazards=%u readback=%s verdict=%s",
                        c.name, expectName(expect), messages.size(), hazards, readback.c_str(),
                        ok ? "PASS" : "FAIL");
            if (absent) std::printf(" properties=absent");
            if (!missing.empty()) std::printf(" missing_properties=\"%s\"", missing.c_str());
            std::printf("\n");
            ++ran;
            ok ? ++passed : ++failed;
        }
        const unsigned outside = instance.capture.outside;
        std::printf("SYNCVAL_CONTROL_SUMMARY cases=%u passed=%u failed=%u skipped=%u outside=%u\n", ran, passed, failed,
                    skipped, outside);
        std::printf("SAMPLE_VALIDATION_SUMMARY errors=%u warnings=%u\n", instance.capture.errors,
                    instance.capture.warnings);
        if (ran == 0) return 77;
        return failed == 0 && outside == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SyncVal control failed: %s\n", error.what());
        return 1;
    }
}
