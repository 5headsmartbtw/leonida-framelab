/* SPDX-License-Identifier: MIT */
#ifndef FRAMELAB_VULKAN_FG_H
#define FRAMELAB_VULKAN_FG_H

#include <framelab/framelab.h>
#include <stddef.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrameLabVulkanFg FrameLabVulkanFg;
typedef void (FRAMELAB_CALL *FrameLabVulkanFgInitReportFn)(void*, const char*, uint32_t);

typedef struct FrameLabVulkanFgConfig {
    uint32_t struct_size, abi_version;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    uint64_t device_id, queue_id;
    const FrameLabLease* device_lease;
    const char* project_id;
    const char* engine_version;
    const wchar_t* ngx_runtime_directory;
    const wchar_t* log_directory;
    FrameLabVulkanFgInitReportFn report;
    void* report_user;
} FrameLabVulkanFgConfig;

typedef struct FrameLabVulkanFgImage {
    uint64_t resource_id;
    VkImage image;
    VkImageView view;
    FrameLabExtent extent;
    VkFormat format;
    VkImageLayout layout;
    VkImageAspectFlags view_aspects;
    uint32_t queue_family;
} FrameLabVulkanFgImage;

typedef struct FrameLabVulkanFgBindings {
    uint32_t struct_size, abi_version;
    VkCommandBuffer commands;
    VkCommandBuffer producer_commands; /* Same-command-stream mode only. */
    uint64_t command_stream_id;       /* Consumer stream identity. */
    uint32_t producer_queue_family;
    VkSemaphore producer_semaphore;   /* Binary or timeline mode only. */
    uint64_t producer_semaphore_id;
    FrameLabVulkanFgImage color, depth, motion_vectors;
} FrameLabVulkanFgBindings;

typedef struct FrameLabVulkanFgSubmission {
    uint32_t struct_size, abi_version;
    VkSemaphore wait_semaphore;
    uint64_t wait_value; /* Zero for binary. */
    VkPipelineStageFlags wait_stage;
    VkSemaphore completion_timeline;
} FrameLabVulkanFgSubmission;

typedef struct FrameLabVulkanFgOutput {
    VkImage image;
    VkImageView view;
    VkFormat format;
    VkImageLayout layout;
    FrameLabExtent extent;
} FrameLabVulkanFgOutput;

typedef struct FrameLabVulkanFgStats {
    uint32_t evaluated, verified, native_errors;
    uint32_t last_ngx_result;
} FrameLabVulkanFgStats;

FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_create(
    const FrameLabVulkanFgConfig* config, FrameLabVulkanFg** out_fg);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_destroy(FrameLabVulkanFg* fg);
/* The host drains all old-output users and invalidates dependent descriptors before resize. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_record_resize(
    FrameLabVulkanFg* fg, VkCommandBuffer commands, FrameLabExtent extent, uint32_t diagnostics);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_record(
    FrameLabVulkanFg* fg, const FrameLabFgFrame* frame, const FrameLabVulkanFgBindings* bindings,
    uint32_t* out_generated);
/* Submit the recorded work with this wait and a host-chosen completion timeline signal. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_submission(
    const FrameLabVulkanFg* fg, FrameLabVulkanFgSubmission* out_submission);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_mark_submitted(
    FrameLabVulkanFg* fg, uint64_t completion_value);
/* Discard the recording first. Native errors can leave partial commands. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_cancel(FrameLabVulkanFg* fg);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_retire(FrameLabVulkanFg* fg);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_invalidate(FrameLabVulkanFg* fg);
/* Report a lost device without dropping GPU leases or waiting for work. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_notify_device_lost(FrameLabVulkanFg* fg);
/* After stopping submissions, draining the device and discarding host recordings,
 * release retained packets. Destroy this handle and recreate the device and resources. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_release_lost_device(FrameLabVulkanFg* fg);
/* Borrowed until the next resize/destroy. The host keeps any later GPU users alive. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_output(
    const FrameLabVulkanFg* fg, FrameLabVulkanFgOutput* out_output);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_fg_stats(
    const FrameLabVulkanFg* fg, FrameLabVulkanFgStats* out_stats);

#ifdef __cplusplus
}
#endif
#endif
