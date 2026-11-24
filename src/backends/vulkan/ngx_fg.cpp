// SPDX-License-Identifier: MIT
#define NOMINMAX
#include <framelab/vulkan_fg.h>

#include "direct_ngx_fg.hpp"
#include "../../core/fg_stream_internal.hpp"

#include <atomic>
#include <memory>
#include <new>

namespace fi = frame_inputs;
namespace {
std::atomic_flag activeBackend;
enum class Work { none, resize, evaluate };
enum class Phase { idle, recorded, record_failed, submitted };

fi::VulkanImage image(const FrameLabVulkanFgImage& in)
{
    return {in.resource_id, in.image, in.view, in.format, in.layout,
            in.view_aspects, in.queue_family, {in.extent.width, in.extent.height}};
}

FrameLabStatus resultFrom(VkResult result)
{
    if (result == VK_SUCCESS) return FRAMELAB_OK;
    if (result == VK_ERROR_DEVICE_LOST) return FRAMELAB_DEVICE_LOST;
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
        return FRAMELAB_OUT_OF_MEMORY;
    return FRAMELAB_NATIVE_FAILURE;
}
} // namespace

struct FrameLabVulkanFg {
    std::shared_ptr<const void> deviceLease;
    DirectNgxFG engine;
    FrameLabFgStream* stream{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{}, families{};
    uint64_t deviceId{}, queueId{};
    VkSemaphore completion{}, producer{};
    uint64_t lastTicket{}, waitValue{};
    VkImageLayout priorOutputLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkExtent2D extent{};
    Work work = Work::none;
    Phase phase = Phase::idle;
    bool ready = false;
    bool lost = false;

    ~FrameLabVulkanFg()
    {
        engine.shutdown();
        if (completion) vkDestroySemaphore(device, completion, nullptr);
        framelab_fg_stream_destroy(stream);
        activeBackend.clear();
    }

    void resetHistory()
    {
        engine.invalidateHistory();
        framelab_fg_invalidate(stream);
    }

    void releasePending()
    {
        phase = Phase::idle;
        work = Work::none;
        producer = VK_NULL_HANDLE;
        waitValue = 0;
    }
};

FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_create(const FrameLabVulkanFgConfig* config,
                                                       FrameLabVulkanFg** out)
{
    if (!out) return FRAMELAB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!config) return FRAMELAB_INVALID_ARGUMENT;
    if (config->struct_size < sizeof *config || config->abi_version != FRAMELAB_ABI_VERSION)
        return FRAMELAB_ABI_MISMATCH;
    const auto& c = *config;
    if (!c.instance || !c.physical_device || !c.device || !c.queue || !c.queue_id || !c.device_id ||
        !c.device_lease || !c.project_id || !*c.project_id || !c.engine_version ||
        !c.ngx_runtime_directory || !*c.ngx_runtime_directory || !c.log_directory || !*c.log_directory)
        return FRAMELAB_INVALID_ARGUMENT;
    if (activeBackend.test_and_set()) return FRAMELAB_BUSY;
    std::unique_ptr<FrameLabVulkanFg> fg(new (std::nothrow) FrameLabVulkanFg);
    if (!fg) {
        activeBackend.clear();
        return FRAMELAB_OUT_OF_MEMORY;
    }
    try {
        fg->deviceLease = framelab::detail::retainLease(c.device_lease);
        fg->device = c.device;
        fg->queue = c.queue;
        fg->deviceId = c.device_id;
        fg->queueId = c.queue_id;
        fg->family = c.queue_family;
        vkGetPhysicalDeviceQueueFamilyProperties(c.physical_device, &fg->families, nullptr);
        if (c.queue_family >= fg->families) return FRAMELAB_INVALID_SYNC;
        std::vector<VkQueueFamilyProperties> families(fg->families);
        vkGetPhysicalDeviceQueueFamilyProperties(c.physical_device, &fg->families, families.data());
        constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((families[c.queue_family].queueFlags & required) != required) return FRAMELAB_UNSUPPORTED;
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &timeline;
        vkGetPhysicalDeviceFeatures2(c.physical_device, &features);
        if (!timeline.timelineSemaphore) return FRAMELAB_UNSUPPORTED;

        const auto stream = framelab_fg_stream_create(FRAMELAB_ABI_VERSION, &fg->stream);
        if (stream != FRAMELAB_OK) return stream;
        fg->engine.initialize(c.instance, c.physical_device, c.device, c.queue, c.queue_family,
                              c.ngx_runtime_directory, c.log_directory,
                              c.project_id, c.engine_version, c.report, c.report_user);
        if (NVSDK_NGX_FAILED(fg->engine.lastNative)) return FRAMELAB_NATIVE_FAILURE;
        if (!fg->engine.available) return fg->engine.errors && fg->engine.lastNative == 0 ?
                                          FRAMELAB_NATIVE_FAILURE : FRAMELAB_UNSUPPORTED;
        if (fg->engine.errors) return FRAMELAB_NATIVE_FAILURE;
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        const VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        const auto created = resultFrom(vkCreateSemaphore(c.device, &info, nullptr, &fg->completion));
        if (created != FRAMELAB_OK) return created;
        *out = fg.release();
        return FRAMELAB_OK;
    } catch (const std::bad_alloc&) {
        return FRAMELAB_OUT_OF_MEMORY;
    } catch (...) {
        return FRAMELAB_NATIVE_FAILURE;
    }
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_destroy(FrameLabVulkanFg* fg)
{
    if (fg && fg->phase != Phase::idle) return FRAMELAB_BUSY;
    delete fg;
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_record_resize(
    FrameLabVulkanFg* fg, VkCommandBuffer commands, FrameLabExtent extent, uint32_t diagnostics)
{
    if (!fg || !commands || diagnostics > 1) return FRAMELAB_INVALID_ARGUMENT;
    if (fg->lost) return FRAMELAB_DEVICE_LOST;
    if (fg->phase != Phase::idle) return FRAMELAB_BUSY;
    if (!extent.width || !extent.height) return FRAMELAB_INVALID_DIMENSIONS;
    if (fg->engine.errors) return FRAMELAB_NATIVE_FAILURE;
    fg->ready = false;
    fg->work = Work::resize;
    fg->phase = Phase::record_failed;
    fg->extent = {extent.width, extent.height};
    fg->resetHistory();
    try {
        fg->engine.resize(commands, fg->extent, diagnostics != 0);
        if (!fg->engine.ready()) return FRAMELAB_NATIVE_FAILURE;
        fg->phase = Phase::recorded;
        return FRAMELAB_OK;
    } catch (const std::bad_alloc&) {
        return FRAMELAB_OUT_OF_MEMORY;
    } catch (...) {
        return FRAMELAB_NATIVE_FAILURE;
    }
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_record(
    FrameLabVulkanFg* fg, const FrameLabFgFrame* frame, const FrameLabVulkanFgBindings* b,
    uint32_t* out_generated)
{
    if (!out_generated) return FRAMELAB_INVALID_ARGUMENT;
    *out_generated = 0;
    if (!fg || !frame || !b) return FRAMELAB_INVALID_ARGUMENT;
    if (fg->lost) return FRAMELAB_DEVICE_LOST;
    if (fg->phase != Phase::idle) return FRAMELAB_BUSY;
    if (!fg->ready) return FRAMELAB_INVALID_PHASE;
    if (b->struct_size < sizeof *b || b->abi_version != FRAMELAB_ABI_VERSION)
        return FRAMELAB_ABI_MISMATCH;
    if (!b->commands || !b->command_stream_id) return FRAMELAB_INVALID_SYNC;
    const auto prepared = framelab_fg_prepare(fg->stream, frame);
    if (prepared != FRAMELAB_OK) {
        fg->resetHistory();
        return prepared;
    }
    const auto reject = [&](FrameLabStatus status) {
        framelab_fg_cancel(fg->stream);
        fg->engine.invalidateHistory();
        return status;
    };
    const auto& input = *framelab::detail::preparedInputs(fg->stream);
    if (input.outputExtent != fi::Extent{fg->extent.width, fg->extent.height})
        return reject(FRAMELAB_INVALID_DIMENSIONS);
    if (input.frameIndex == UINT64_MAX) return reject(FRAMELAB_INVALID_SEQUENCE);
    if (input.device != fg->deviceId) return reject(FRAMELAB_INVALID_IDENTITY);
    if (b->color.image == fg->engine.output || b->depth.image == fg->engine.output ||
        b->motion_vectors.image == fg->engine.output)
        return reject(FRAMELAB_INVALID_RESOURCE_STATE);

    fi::VulkanFrameInputs typed{
        input, fg->deviceId, fg->queueId, b->command_stream_id, fg->device, fg->queue,
        fg->family, b->producer_commands, image(b->color), image(b->depth), image(b->motion_vectors)
    };
    const auto mode = input.sync.completion;
    const bool external = mode == fi::Completion::binary_signal || mode == fi::Completion::timeline_signal;
    fi::Result layout;
    if (external) {
        if (b->producer_commands || !b->producer_semaphore || b->producer_semaphore == fg->completion ||
            input.sync.signal != b->producer_semaphore_id || b->producer_queue_family >= fg->families)
            return reject(FRAMELAB_INVALID_SYNC);
        if (input.sync.queue == fg->queueId && b->producer_queue_family != fg->family)
            return reject(FRAMELAB_INVALID_SYNC);
        layout = fi::validateVulkanImages(typed, b->producer_queue_family, input.sync.queue);
    } else {
        if (mode != fi::Completion::same_command_stream) return reject(FRAMELAB_UNSUPPORTED);
        if (b->producer_commands != b->commands || b->producer_semaphore || b->producer_semaphore_id ||
            b->producer_queue_family != fg->family || input.sync.commandStream != b->command_stream_id)
            return reject(FRAMELAB_INVALID_SYNC);
        layout = fi::validateVulkan(typed, b->commands, fg->device, fg->queue, fg->family);
    }
    if (!layout) return reject(layout.error == fi::Error::unsupported ?
                              FRAMELAB_UNSUPPORTED : FRAMELAB_INVALID_RESOURCE_STATE);
    fg->producer = external ? b->producer_semaphore : VK_NULL_HANDLE;
    fg->waitValue = external ? input.sync.value : 0;
    fg->priorOutputLayout = fg->engine.currentLayout();
    fg->work = Work::evaluate;
    fg->phase = Phase::record_failed;
    try {
        const auto generated = fg->engine.evaluate(b->commands, typed,
            external ? &b->producer_queue_family : nullptr, true);
        if (fg->engine.errors) return FRAMELAB_NATIVE_FAILURE;
        if (!fg->engine.hasInFlightInputs()) {
            fg->releasePending();
            return reject(FRAMELAB_RESET_REQUIRED);
        }
        fg->phase = Phase::recorded;
        *out_generated = generated;
        return FRAMELAB_OK;
    } catch (const std::bad_alloc&) {
        return FRAMELAB_OUT_OF_MEMORY;
    } catch (...) {
        return FRAMELAB_NATIVE_FAILURE;
    }
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_submission(
    const FrameLabVulkanFg* fg, FrameLabVulkanFgSubmission* out)
{
    if (!fg || !out) return FRAMELAB_INVALID_ARGUMENT;
    if (out->struct_size < sizeof *out || out->abi_version != FRAMELAB_ABI_VERSION)
        return FRAMELAB_ABI_MISMATCH;
    if (fg->phase != Phase::recorded) return FRAMELAB_INVALID_PHASE;
    out->wait_semaphore = fg->producer;
    out->wait_value = fg->waitValue;
    out->wait_stage = fg->producer ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : 0;
    out->completion_timeline = fg->completion;
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_mark_submitted(FrameLabVulkanFg* fg, uint64_t value)
{
    if (!fg || value == UINT64_MAX) return FRAMELAB_INVALID_ARGUMENT;
    if (fg->phase != Phase::recorded) return FRAMELAB_INVALID_PHASE;
    if (value <= fg->lastTicket) return FRAMELAB_INVALID_SYNC;
    if (fg->work == Work::evaluate) framelab_fg_mark_submitted(fg->stream);
    fg->lastTicket = value;
    fg->phase = Phase::submitted;
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_cancel(FrameLabVulkanFg* fg)
{
    if (!fg) return FRAMELAB_INVALID_ARGUMENT;
    if (fg->phase != Phase::recorded && fg->phase != Phase::record_failed) return FRAMELAB_INVALID_PHASE;
    if (fg->work == Work::evaluate) {
        fg->engine.abandonRecording(fg->priorOutputLayout);
        framelab_fg_cancel(fg->stream);
    } else {
        fg->engine.releaseImages();
    }
    fg->releasePending();
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_retire(FrameLabVulkanFg* fg)
{
    if (!fg) return FRAMELAB_INVALID_ARGUMENT;
    if (fg->phase != Phase::submitted) return FRAMELAB_INVALID_PHASE;
    uint64_t reached = 0;
    auto completed = resultFrom(vkGetSemaphoreCounterValue(fg->device, fg->completion, &reached));
    if (completed == FRAMELAB_DEVICE_LOST) fg->lost = true;
    else if (completed != FRAMELAB_OK) return completed;
    else if (reached < fg->lastTicket) return FRAMELAB_NOT_READY;

    if (fg->work == Work::evaluate) {
        try {
            if (fg->lost) fg->engine.abandonRecording(fg->priorOutputLayout);
            else fg->engine.onGpuCompleted();
        } catch (const std::bad_alloc&) {
            fg->engine.abandonRecording(fg->engine.currentLayout());
            completed = FRAMELAB_OUT_OF_MEMORY;
        } catch (...) {
            fg->engine.abandonRecording(fg->engine.currentLayout());
            completed = FRAMELAB_NATIVE_FAILURE;
        }
        framelab_fg_retire(fg->stream);
        if (completed != FRAMELAB_OK) fg->resetHistory();
    } else {
        fg->ready = !fg->lost;
    }
    fg->releasePending();
    return completed;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_invalidate(FrameLabVulkanFg* fg)
{
    if (!fg) return FRAMELAB_INVALID_ARGUMENT;
    if (fg->phase != Phase::idle) return FRAMELAB_BUSY;
    fg->resetHistory();
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_output(
    const FrameLabVulkanFg* fg, FrameLabVulkanFgOutput* out)
{
    if (!fg || !out) return FRAMELAB_INVALID_ARGUMENT;
    if (fg->lost) return FRAMELAB_DEVICE_LOST;
    if (!fg->ready) return FRAMELAB_INVALID_PHASE;
    *out = {fg->engine.output, fg->engine.outputView, VK_FORMAT_R16G16B16A16_SFLOAT,
            fg->engine.currentLayout(), {fg->extent.width, fg->extent.height}};
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_stats(
    const FrameLabVulkanFg* fg, FrameLabVulkanFgStats* out)
{
    if (!fg || !out) return FRAMELAB_INVALID_ARGUMENT;
    *out = {fg->engine.evaluations, fg->engine.verified, fg->engine.errors,
            static_cast<uint32_t>(fg->engine.lastNative)};
    return FRAMELAB_OK;
}
