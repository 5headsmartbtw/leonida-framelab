// SPDX-License-Identifier: MIT
#define NOMINMAX
#include <windows.h>
#include <framelab/vulkan_sr.h>

#include "../../core/fg_stream_internal.hpp"

#include <sl.h>
#include <sl_dlss.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

namespace {
enum class Phase { idle, failed_record, recorded, submitted };

bool same(FrameLabExtent a, FrameLabExtent b) { return a.width == b.width && a.height == b.height; }
bool valid(FrameLabExtent extent) { return extent.width && extent.height; }
template<class T> bool abi(const T& data)
{
    return data.struct_size >= sizeof(T) && data.abi_version == FRAMELAB_ABI_VERSION;
}

FrameLabStatus vulkanResult(VkResult result)
{
    switch (result) {
    case VK_SUCCESS: return FRAMELAB_OK;
    case VK_ERROR_DEVICE_LOST: return FRAMELAB_DEVICE_LOST;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return FRAMELAB_OUT_OF_MEMORY;
    default: return FRAMELAB_NATIVE_FAILURE;
    }
}

template<size_t N> bool finite(const float (&values)[N])
{
    return std::all_of(values, values + N, [](float value) { return std::isfinite(value); });
}

bool validCamera(const FrameLabCamera& c)
{
    return (c.flags & FRAMELAB_CAMERA_VERIFIED) &&
        c.projection_space == FRAMELAB_CLIP_Y_DOWN_ZERO_TO_ONE &&
        c.reprojection_space == FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE &&
        c.motion_convention == FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV &&
        finite(c.view_to_clip) && finite(c.clip_to_view) && finite(c.clip_to_lens) &&
        finite(c.clip_to_previous) && finite(c.previous_to_clip) &&
        finite(c.position) && finite(c.up) && finite(c.right) && finite(c.forward) &&
        finite(c.jitter_pixels) && finite(c.motion_vector_scale) &&
        c.motion_vector_scale[0] != 0 && c.motion_vector_scale[1] != 0 &&
        std::isfinite(c.far_plane) && c.near_plane > 0 && c.far_plane > c.near_plane &&
        c.fov_radians > 0 && c.fov_radians < 3.141593f && std::isfinite(c.aspect) && c.aspect > 0;
}

sl::float4x4 matrix(const float* m)
{
    sl::float4x4 out;
    for (unsigned i = 0; i != 4; ++i)
        out.row[i] = {m[4*i], m[4*i+1], m[4*i+2], m[4*i+3]};
    return out;
}

sl::float3 vector(const float* v) { return {v[0], v[1], v[2]}; }
sl::Boolean flag(bool value) { return value ? sl::Boolean::eTrue : sl::Boolean::eFalse; }

sl::Constants constants(const FrameLabCamera& c, bool reset)
{
    sl::Constants value;
    value.cameraViewToClip = matrix(c.view_to_clip);
    value.clipToCameraView = matrix(c.clip_to_view);
    value.clipToLensClip = matrix(c.clip_to_lens);
    value.clipToPrevClip = matrix(c.clip_to_previous);
    value.prevClipToClip = matrix(c.previous_to_clip);
    value.cameraPinholeOffset = {0, 0};
    value.jitterOffset = {c.jitter_pixels[0], c.jitter_pixels[1]};
    value.mvecScale = {c.motion_vector_scale[0], c.motion_vector_scale[1]};
    value.cameraPos = vector(c.position);
    value.cameraUp = vector(c.up);
    value.cameraRight = vector(c.right);
    value.cameraFwd = vector(c.forward);
    value.cameraNear = c.near_plane;
    value.cameraFar = c.far_plane;
    value.cameraFOV = c.fov_radians;
    value.cameraAspectRatio = c.aspect;
    value.depthInverted = flag(c.flags & FRAMELAB_CAMERA_DEPTH_INVERTED);
    value.cameraMotionIncluded = flag(c.flags & FRAMELAB_CAMERA_MOTION_INCLUDED);
    value.orthographicProjection = flag(c.flags & FRAMELAB_CAMERA_ORTHOGRAPHIC);
    value.motionVectors3D = sl::Boolean::eFalse;
    value.motionVectorsJittered = sl::Boolean::eFalse;
    value.reset = flag(reset);
    return value;
}

sl::Resource resource(const FrameLabVulkanSrImage& image)
{
    sl::Resource out(sl::ResourceType::eTex2d, image.image, nullptr, image.view, image.layout);
    out.width = image.extent.width;
    out.height = image.extent.height;
    out.nativeFormat = image.format;
    out.mipLevels = out.arrayLayers = 1;
    out.usage = image.usage;
    out.flags = 0;
    return out;
}

template<class Fn> Fn* exported(HMODULE module, const char* name)
{
    return reinterpret_cast<Fn*>(GetProcAddress(module, name));
}
} // namespace

struct FrameLabVulkanSr {
    std::shared_ptr<const void> deviceLease;
    std::array<std::shared_ptr<const void>, 4> leases;
    HMODULE module{};
    VkDevice device{};
    VkSemaphore completion{};
    uint32_t family{}, frameIndex{}, lastFrame{};
    uint64_t ticket{};
    bool needReset = true, hasFrame = false, allocated = false, lost = false;
    Phase phase = Phase::idle;
    FrameLabExtent render{}, output{};
    sl::ViewportHandle viewport;
    FrameLabVulkanSrReportFn report{};
    void* user{};
    PFun_slGetNewFrameToken* token{};
    PFun_slSetConstants* setConstants{};
    PFun_slEvaluateFeature* evaluate{};
    PFun_slFreeResources* freeResources{};
    PFun_slDLSSSetOptions* setOptions{};
    PFun_slDLSSGetOptimalSettings* optimalSettings{};

    explicit FrameLabVulkanSr(uint32_t id) : viewport(id) {}
    ~FrameLabVulkanSr()
    {
        if (allocated && !lost) freeResources(sl::kFeatureDLSS, viewport);
        if (completion) vkDestroySemaphore(device, completion, nullptr);
        if (module) FreeLibrary(module);
    }

    bool ok(const char* stage, sl::Result result) const
    {
        if (report) report(user, stage, static_cast<int32_t>(result));
        return result == sl::Result::eOk;
    }
};

FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_create(const FrameLabVulkanSrConfig* c, FrameLabVulkanSr** out)
{
    if (!out) return FRAMELAB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!c) return FRAMELAB_INVALID_ARGUMENT;
    if (!abi(*c)) return FRAMELAB_ABI_MISMATCH;
    if (!c->device || !c->physical_device || !c->queue || !c->initialized_interposer_path ||
        !c->device_and_runtime_lease) return FRAMELAB_INVALID_ARGUMENT;
    try {
        if (!std::filesystem::path(c->initialized_interposer_path).is_absolute()) return FRAMELAB_INVALID_ARGUMENT;
        auto sr = std::make_unique<FrameLabVulkanSr>(c->viewport_id);
        sr->deviceLease = framelab::detail::retainLease(c->device_and_runtime_lease);
        sr->device = c->device;
        sr->family = c->queue_family;
        sr->report = c->report;
        sr->user = c->report_user;
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(c->physical_device, &count, nullptr);
        if (c->queue_family >= count) return FRAMELAB_INVALID_SYNC;
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(c->physical_device, &count, families.data());
        constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((families[c->queue_family].queueFlags & required) != required) return FRAMELAB_UNSUPPORTED;
        if (!GetModuleHandleExW(0, c->initialized_interposer_path, &sr->module)) return FRAMELAB_UNSUPPORTED;
        const auto supported = exported<PFun_slIsFeatureSupported>(sr->module, "slIsFeatureSupported");
        const auto getFunction = exported<PFun_slGetFeatureFunction>(sr->module, "slGetFeatureFunction");
        sr->token = exported<PFun_slGetNewFrameToken>(sr->module, "slGetNewFrameToken");
        sr->setConstants = exported<PFun_slSetConstants>(sr->module, "slSetConstants");
        sr->evaluate = exported<PFun_slEvaluateFeature>(sr->module, "slEvaluateFeature");
        sr->freeResources = exported<PFun_slFreeResources>(sr->module, "slFreeResources");
        if (!supported || !getFunction || !sr->token || !sr->setConstants || !sr->evaluate || !sr->freeResources)
            return FRAMELAB_UNSUPPORTED;
        sl::AdapterInfo adapter;
        adapter.vkPhysicalDevice = c->physical_device;
        if (!sr->ok("slIsFeatureSupported", supported(sl::kFeatureDLSS, adapter))) return FRAMELAB_UNSUPPORTED;
        void* fn = nullptr;
        if (!sr->ok("slDLSSSetOptions lookup", getFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fn)) || !fn)
            return FRAMELAB_UNSUPPORTED;
        sr->setOptions = reinterpret_cast<PFun_slDLSSSetOptions*>(fn);
        fn = nullptr;
        if (!sr->ok("slDLSSGetOptimalSettings lookup", getFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fn)) || !fn)
            return FRAMELAB_UNSUPPORTED;
        sr->optimalSettings = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(fn);
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        const VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        const auto created = vulkanResult(vkCreateSemaphore(sr->device, &info, nullptr, &sr->completion));
        if (created != FRAMELAB_OK) return created;
        *out = sr.release();
        return FRAMELAB_OK;
    } catch (const std::bad_alloc&) {
        return FRAMELAB_OUT_OF_MEMORY;
    } catch (...) {
        return FRAMELAB_NATIVE_FAILURE;
    }
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_destroy(FrameLabVulkanSr* sr)
{
    if (sr && sr->phase != Phase::idle) return FRAMELAB_BUSY;
    delete sr;
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_configure(
    FrameLabVulkanSr* sr, const FrameLabVulkanSrOptions* o, FrameLabExtent* extent)
{
    if (!sr || !o || !extent) return FRAMELAB_INVALID_ARGUMENT;
    *extent = {};
    if (!abi(*o)) return FRAMELAB_ABI_MISMATCH;
    if (sr->lost) return FRAMELAB_DEVICE_LOST;
    if (sr->phase != Phase::idle) return FRAMELAB_BUSY;
    if (!valid(o->output_extent) || o->hdr > 1) return FRAMELAB_INVALID_ARGUMENT;
    sl::DLSSOptions options;
    switch (o->mode) {
    case FRAMELAB_SR_PERFORMANCE: options.mode = sl::DLSSMode::eMaxPerformance; break;
    case FRAMELAB_SR_BALANCED: options.mode = sl::DLSSMode::eBalanced; break;
    case FRAMELAB_SR_QUALITY: options.mode = sl::DLSSMode::eMaxQuality; break;
    default: return FRAMELAB_UNSUPPORTED;
    }
    options.outputWidth = o->output_extent.width;
    options.outputHeight = o->output_extent.height;
    options.colorBuffersHDR = flag(o->hdr != 0);
    sl::DLSSOptimalSettings settings;
    if (!sr->ok("slDLSSGetOptimalSettings", sr->optimalSettings(options, settings)))
        return FRAMELAB_UNSUPPORTED;
    FrameLabExtent optimal{settings.optimalRenderWidth, settings.optimalRenderHeight};
    if (!valid(optimal) || optimal.width > options.outputWidth || optimal.height > options.outputHeight)
        return FRAMELAB_UNSUPPORTED;
    if (!sr->ok("slDLSSSetOptions", sr->setOptions(sr->viewport, options))) {
        sr->render = {};
        return FRAMELAB_NATIVE_FAILURE;
    }
    sr->render = optimal;
    sr->output = o->output_extent;
    sr->needReset = true;
    *extent = optimal;
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_record(FrameLabVulkanSr* sr, const FrameLabVulkanSrFrame* f)
{
    if (!sr || !f) return FRAMELAB_INVALID_ARGUMENT;
    if (!abi(*f)) return FRAMELAB_ABI_MISMATCH;
    if (sr->lost) return FRAMELAB_DEVICE_LOST;
    if (sr->phase != Phase::idle) return FRAMELAB_BUSY;
    if (!valid(sr->render)) return FRAMELAB_INVALID_PHASE;
    const bool reset = sr->needReset || f->reset_history ||
        (sr->hasFrame && f->frame_index - sr->lastFrame != 1);
    sr->needReset = true;
    if (!f->commands || f->reset_history > 1) return FRAMELAB_INVALID_ARGUMENT;
    if (sr->hasFrame && f->frame_index <= sr->lastFrame) return FRAMELAB_INVALID_SEQUENCE;
    if (!validCamera(f->camera)) return FRAMELAB_INVALID_CAMERA;
    const FrameLabVulkanSrImage* images[]{&f->color, &f->depth, &f->motion_vectors, &f->output};
    const VkImageAspectFlags aspects[]{VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                                      VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_COLOR_BIT};
    const VkImageLayout layouts[]{VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkFormat formats[]{VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
    for (unsigned i = 0; i < std::size(images); ++i) {
        const auto& img = *images[i];
        if (!img.lease) return FRAMELAB_MISSING_LEASE;
        if (!same(img.extent, i == 3 ? sr->output : sr->render)) return FRAMELAB_INVALID_DIMENSIONS;
        const auto usage = VK_IMAGE_USAGE_SAMPLED_BIT | (i == 3 ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
        if (!img.image || !img.view || img.view_aspects != aspects[i] ||
            img.layout != layouts[i] || img.format != formats[i] || (img.usage & usage) != usage)
            return FRAMELAB_INVALID_RESOURCE_STATE;
        if (img.queue_family != sr->family) return FRAMELAB_INVALID_SYNC;
        for (unsigned j = 0; j < i; ++j)
            if (img.image == images[j]->image || img.view == images[j]->view) return FRAMELAB_INVALID_IDENTITY;
    }
    sl::FrameToken* frame = nullptr;
    if (!sr->ok("slGetNewFrameToken", sr->token(frame, &f->frame_index)) || !frame)
        return FRAMELAB_NATIVE_FAILURE;
    const auto camera = constants(f->camera, reset);
    if (!sr->ok("slSetConstants", sr->setConstants(camera, *frame, sr->viewport)))
        return FRAMELAB_NATIVE_FAILURE;
    for (unsigned i = 0; i < std::size(images); ++i)
        sr->leases[i] = framelab::detail::retainLease(images[i]->lease);
    sl::Resource resources[]{resource(f->color), resource(f->depth), resource(f->motion_vectors), resource(f->output)};
    const sl::ResourceTag tags[]{
        {&resources[0], sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow},
        {&resources[1], sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow},
        {&resources[2], sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow},
        {&resources[3], sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow}
    };
    const sl::BaseStructure* args[]{&sr->viewport, &tags[0], &tags[1], &tags[2], &tags[3]};
    sr->phase = Phase::failed_record;
    sr->allocated = true;
    if (!sr->ok("slEvaluateFeature", sr->evaluate(sl::kFeatureDLSS, *frame, args, uint32_t(std::size(args)),
                                               reinterpret_cast<sl::CommandBuffer*>(f->commands))))
        return FRAMELAB_NATIVE_FAILURE;
    sr->frameIndex = f->frame_index;
    sr->phase = Phase::recorded;
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_completion(const FrameLabVulkanSr* sr, VkSemaphore* out)
{
    if (!sr || !out) return FRAMELAB_INVALID_ARGUMENT;
    *out = VK_NULL_HANDLE;
    if (sr->phase != Phase::recorded) return FRAMELAB_INVALID_PHASE;
    *out = sr->completion;
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_mark_submitted(FrameLabVulkanSr* sr, uint64_t ticket)
{
    if (!sr) return FRAMELAB_INVALID_ARGUMENT;
    if (sr->phase != Phase::recorded) return FRAMELAB_INVALID_PHASE;
    if (ticket <= sr->ticket || ticket == UINT64_MAX) return FRAMELAB_INVALID_SYNC;
    sr->lastFrame = sr->frameIndex;
    sr->hasFrame = true;
    sr->needReset = false;
    sr->ticket = ticket;
    sr->phase = Phase::submitted;
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_cancel(FrameLabVulkanSr* sr)
{
    if (!sr) return FRAMELAB_INVALID_ARGUMENT;
    if (sr->phase != Phase::recorded && sr->phase != Phase::failed_record) return FRAMELAB_INVALID_PHASE;
    sr->needReset = true;
    sr->phase = Phase::idle;
    sr->leases = {};
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_retire(FrameLabVulkanSr* sr)
{
    if (!sr) return FRAMELAB_INVALID_ARGUMENT;
    if (sr->phase != Phase::submitted) return FRAMELAB_INVALID_PHASE;
    uint64_t reached = 0;
    const auto result = vulkanResult(vkGetSemaphoreCounterValue(sr->device, sr->completion, &reached));
    if (result == FRAMELAB_OK && reached < sr->ticket) return FRAMELAB_NOT_READY;
    if (result != FRAMELAB_OK && result != FRAMELAB_DEVICE_LOST) return result;
    sr->lost = result == FRAMELAB_DEVICE_LOST;
    sr->phase = Phase::idle;
    sr->leases = {};
    return result;
}

FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_invalidate(FrameLabVulkanSr* sr)
{
    if (!sr) return FRAMELAB_INVALID_ARGUMENT;
    if (sr->phase != Phase::idle) return FRAMELAB_BUSY;
    sr->needReset = true;
    return FRAMELAB_OK;
}
