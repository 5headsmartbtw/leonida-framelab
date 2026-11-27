/* SPDX-License-Identifier: MIT */
#ifndef FRAMELAB_D3D12_FG_H
#define FRAMELAB_D3D12_FG_H

#include <framelab/framelab.h>
#include <d3d12.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrameLabD3D12Fg FrameLabD3D12Fg;
typedef void (FRAMELAB_CALL *FrameLabD3D12InitReportFn)(void* user, const char* operation, uint32_t ngx_result);

/* FrameLabD3D12FgConfig.flags */
enum {
    /* Color is HDR10: BT.2100 PQ-encoded BT.2020 RGB. Requires FRAMELAB_FORMAT_RGB10A2_UNORM.
     * FrameLab passes the frames through without tone mapping. */
    FRAMELAB_D3D12_FG_HDR10 = 1
};

typedef struct FrameLabD3D12FgConfig {
    uint32_t struct_size, abi_version;
    uint64_t device_id, queue_id;
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    ID3D12Fence* completion_fence;
    FrameLabExtent extent;
    const char* project_id;
    const char* engine_version;
    const wchar_t* ngx_runtime_directory;
    const wchar_t* log_directory;
    FrameLabD3D12InitReportFn report; /* Optional, called synchronously during create. */
    void* report_user;
    /* Appended after v0.4.0 (ABI version unchanged). Depth and motion vectors use this extent;
     * color and the generated image use `extent`. Both dimensions zero select `extent`; one zero
     * dimension or a dimension above `extent` returns FRAMELAB_INVALID_DIMENSIONS. A client built
     * against the v0.4.0 header passes that struct's size (104 bytes on x64) and is accepted
     * without this field being read. Any size other than a published layout's size or at least
     * sizeof returns FRAMELAB_ABI_MISMATCH. A v0.4.0 library rejects the new sizes, so a different
     * render extent needs this library. */
    FrameLabExtent render_extent;
    /* Appended after render_extent (ABI version unchanged). The color and generated images use
     * color_format: 0 or FRAMELAB_FORMAT_RGBA8_UNORM, FRAMELAB_FORMAT_BGRA8_UNORM or
     * FRAMELAB_FORMAT_RGB10A2_UNORM. FRAMELAB_FORMAT_RGBA16_FLOAT returns FRAMELAB_UNSUPPORTED;
     * any other value, an unknown flag, or HDR10 without RGB10A2 returns FRAMELAB_INVALID_ARGUMENT.
     * A format the device cannot load, sample and store as a typed UAV returns FRAMELAB_UNSUPPORTED.
     * A client built against the header that ends at render_extent passes that struct's size
     * (112 bytes on x64) and gets RGBA8 without these fields being read. */
    uint32_t color_format;
    uint32_t flags; /* FRAMELAB_D3D12_FG_* */
} FrameLabD3D12FgConfig;

typedef struct FrameLabD3D12FgImage {
    uint64_t resource_id;
    ID3D12Resource* resource;
    uint32_t state; /* Producer-tracked D3D12_RESOURCE_STATES. */
} FrameLabD3D12FgImage;

typedef struct FrameLabD3D12FgBindings {
    uint32_t struct_size, abi_version;
    uint64_t command_stream_id;
    ID3D12GraphicsCommandList* commands;
    FrameLabD3D12FgImage color, depth, motion_vectors, generated;
} FrameLabD3D12FgBindings;

/* Queries native NGX capability. The host provides live COM interfaces and an
 * increasing completion timeline signaled by this direct queue. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_create(
    const FrameLabD3D12FgConfig* config, FrameLabD3D12Fg** out_fg);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_destroy(FrameLabD3D12Fg* fg);

/* Record NGX feature creation into an open direct list on the configured device.
 * Submit, acknowledge, and retire it before the first frame. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_record_initialize(
    FrameLabD3D12Fg* fg, ID3D12GraphicsCommandList* commands);
/* Validate metadata and native objects before attempting to record NGX Evaluate.
 * Resource or metadata rejection leaves the host command list usable. After
 * FRAMELAB_NATIVE_FAILURE from recording, discard the list and cancel the packet. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_record(
    FrameLabD3D12Fg* fg, const FrameLabFgFrame* frame, const FrameLabD3D12FgBindings* bindings);
/* Acknowledge only after executing the recorded list and queuing Signal(value). */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_mark_submitted(
    FrameLabD3D12Fg* fg, uint64_t value);
/* Cancel only after ensuring the recorded work cannot be submitted. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_cancel(FrameLabD3D12Fg* fg);
/* Check the actual completion fence. FRAMELAB_NOT_READY keeps all references. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_retire(FrameLabD3D12Fg* fg);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_invalidate(FrameLabD3D12Fg* fg);
/* Raw NGX status from the most recent recorded native operation. */
FRAMELAB_API uint32_t FRAMELAB_CALL framelab_d3d12_fg_last_ngx_result(const FrameLabD3D12Fg* fg);

#ifdef __cplusplus
}
#endif
#endif
