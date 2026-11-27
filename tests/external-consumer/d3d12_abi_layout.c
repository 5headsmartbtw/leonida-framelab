/* SPDX-License-Identifier: MIT */
/* Native layout, constant and signature probe for the Rust D3D12 SR/FG bindings (Windows x64).
 * Prints whitespace-separated key=value pairs that bindings/rust/tests/d3d12_abi.rs compares.
 * It references no FrameLab function, so it runs without the D3D12 components or a GPU. */
#include <framelab/d3d12_fg.h>
#include <framelab/d3d12_sr.h>

#include <stddef.h>
#include <stdio.h>

#define TYPE(t) printf(#t ".size=%zu " #t ".align=%zu\n", sizeof(t), _Alignof(t))
#define FIELD(t, f) printf(#t "." #f "=%zu\n", offsetof(t, f))
#define CONSTANT(n) printf(#n "=%u\n", (unsigned)(n))
/* Each pointer type below mirrors the corresponding Rust `pfn` type. _Generic does not evaluate
 * its operand, so the probe needs no export. */
#define SIGNATURE(f, ret, ...) \
    typedef ret (FRAMELAB_CALL* f##_mirror)(__VA_ARGS__); \
    _Static_assert(_Generic(&f, f##_mirror: 1, default: 0), #f " signature differs from the Rust binding")

SIGNATURE(framelab_d3d12_sr_create, FrameLabStatus, const FrameLabD3D12SrConfig*, FrameLabD3D12Sr**);
SIGNATURE(framelab_d3d12_sr_destroy, FrameLabStatus, FrameLabD3D12Sr*);
SIGNATURE(framelab_d3d12_sr_query_settings, FrameLabStatus, FrameLabD3D12Sr*, const FrameLabD3D12SrOptions*,
    FrameLabD3D12SrSettings*);
SIGNATURE(framelab_d3d12_sr_record_configure, FrameLabStatus, FrameLabD3D12Sr*, ID3D12GraphicsCommandList*,
    const FrameLabD3D12SrOptions*, FrameLabD3D12SrSettings*);
SIGNATURE(framelab_d3d12_sr_record, FrameLabStatus, FrameLabD3D12Sr*, const FrameLabD3D12SrFrame*);
SIGNATURE(framelab_d3d12_sr_mark_submitted, FrameLabStatus, FrameLabD3D12Sr*, uint64_t);
SIGNATURE(framelab_d3d12_sr_cancel, FrameLabStatus, FrameLabD3D12Sr*);
SIGNATURE(framelab_d3d12_sr_retire, FrameLabStatus, FrameLabD3D12Sr*);
SIGNATURE(framelab_d3d12_sr_invalidate, FrameLabStatus, FrameLabD3D12Sr*);
SIGNATURE(framelab_d3d12_sr_last_ngx_result, uint32_t, const FrameLabD3D12Sr*);
SIGNATURE(framelab_d3d12_fg_create, FrameLabStatus, const FrameLabD3D12FgConfig*, FrameLabD3D12Fg**);
SIGNATURE(framelab_d3d12_fg_destroy, FrameLabStatus, FrameLabD3D12Fg*);
SIGNATURE(framelab_d3d12_fg_record_initialize, FrameLabStatus, FrameLabD3D12Fg*, ID3D12GraphicsCommandList*);
SIGNATURE(framelab_d3d12_fg_record, FrameLabStatus, FrameLabD3D12Fg*, const FrameLabFgFrame*,
    const FrameLabD3D12FgBindings*);
SIGNATURE(framelab_d3d12_fg_mark_submitted, FrameLabStatus, FrameLabD3D12Fg*, uint64_t);
SIGNATURE(framelab_d3d12_fg_cancel, FrameLabStatus, FrameLabD3D12Fg*);
SIGNATURE(framelab_d3d12_fg_retire, FrameLabStatus, FrameLabD3D12Fg*);
SIGNATURE(framelab_d3d12_fg_invalidate, FrameLabStatus, FrameLabD3D12Fg*);
SIGNATURE(framelab_d3d12_fg_last_ngx_result, uint32_t, const FrameLabD3D12Fg*);

typedef void (FRAMELAB_CALL* report_mirror)(void*, const char*, uint32_t);
_Static_assert(_Generic((FrameLabD3D12SrReportFn)0, report_mirror: 1, default: 0), "SR report signature");
_Static_assert(_Generic((FrameLabD3D12InitReportFn)0, report_mirror: 1, default: 0), "FG report signature");

int main(void)
{
    TYPE(wchar_t);

    TYPE(FrameLabD3D12SrConfig);
    FIELD(FrameLabD3D12SrConfig, struct_size); FIELD(FrameLabD3D12SrConfig, abi_version);
    FIELD(FrameLabD3D12SrConfig, device); FIELD(FrameLabD3D12SrConfig, queue);
    FIELD(FrameLabD3D12SrConfig, completion_fence); FIELD(FrameLabD3D12SrConfig, project_id);
    FIELD(FrameLabD3D12SrConfig, engine_version); FIELD(FrameLabD3D12SrConfig, ngx_runtime_directory);
    FIELD(FrameLabD3D12SrConfig, log_directory); FIELD(FrameLabD3D12SrConfig, report);
    FIELD(FrameLabD3D12SrConfig, report_user);
    TYPE(FrameLabD3D12SrOptions);
    FIELD(FrameLabD3D12SrOptions, struct_size); FIELD(FrameLabD3D12SrOptions, abi_version);
    FIELD(FrameLabD3D12SrOptions, output_extent); FIELD(FrameLabD3D12SrOptions, mode);
    FIELD(FrameLabD3D12SrOptions, flags);
    TYPE(FrameLabD3D12SrSettings);
    FIELD(FrameLabD3D12SrSettings, struct_size); FIELD(FrameLabD3D12SrSettings, abi_version);
    FIELD(FrameLabD3D12SrSettings, optimal_render_extent); FIELD(FrameLabD3D12SrSettings, min_render_extent);
    FIELD(FrameLabD3D12SrSettings, max_render_extent);
    TYPE(FrameLabD3D12SrImage);
    FIELD(FrameLabD3D12SrImage, resource_id); FIELD(FrameLabD3D12SrImage, resource);
    FIELD(FrameLabD3D12SrImage, state); FIELD(FrameLabD3D12SrImage, lease);
    TYPE(FrameLabD3D12SrFrame);
    FIELD(FrameLabD3D12SrFrame, struct_size); FIELD(FrameLabD3D12SrFrame, abi_version);
    FIELD(FrameLabD3D12SrFrame, command_stream_id); FIELD(FrameLabD3D12SrFrame, commands);
    FIELD(FrameLabD3D12SrFrame, frame_index); FIELD(FrameLabD3D12SrFrame, render_extent);
    FIELD(FrameLabD3D12SrFrame, reset_history); FIELD(FrameLabD3D12SrFrame, pre_exposure);
    FIELD(FrameLabD3D12SrFrame, exposure_scale); FIELD(FrameLabD3D12SrFrame, color);
    FIELD(FrameLabD3D12SrFrame, depth); FIELD(FrameLabD3D12SrFrame, motion_vectors);
    FIELD(FrameLabD3D12SrFrame, output); FIELD(FrameLabD3D12SrFrame, camera);

    TYPE(FrameLabD3D12FgConfig);
    FIELD(FrameLabD3D12FgConfig, struct_size); FIELD(FrameLabD3D12FgConfig, abi_version);
    FIELD(FrameLabD3D12FgConfig, device_id); FIELD(FrameLabD3D12FgConfig, queue_id);
    FIELD(FrameLabD3D12FgConfig, device); FIELD(FrameLabD3D12FgConfig, queue);
    FIELD(FrameLabD3D12FgConfig, completion_fence); FIELD(FrameLabD3D12FgConfig, extent);
    FIELD(FrameLabD3D12FgConfig, project_id); FIELD(FrameLabD3D12FgConfig, engine_version);
    FIELD(FrameLabD3D12FgConfig, ngx_runtime_directory); FIELD(FrameLabD3D12FgConfig, log_directory);
    FIELD(FrameLabD3D12FgConfig, report); FIELD(FrameLabD3D12FgConfig, report_user);
    TYPE(FrameLabD3D12FgImage);
    FIELD(FrameLabD3D12FgImage, resource_id); FIELD(FrameLabD3D12FgImage, resource);
    FIELD(FrameLabD3D12FgImage, state);
    TYPE(FrameLabD3D12FgBindings);
    FIELD(FrameLabD3D12FgBindings, struct_size); FIELD(FrameLabD3D12FgBindings, abi_version);
    FIELD(FrameLabD3D12FgBindings, command_stream_id); FIELD(FrameLabD3D12FgBindings, commands);
    FIELD(FrameLabD3D12FgBindings, color); FIELD(FrameLabD3D12FgBindings, depth);
    FIELD(FrameLabD3D12FgBindings, motion_vectors); FIELD(FrameLabD3D12FgBindings, generated);

    CONSTANT(FRAMELAB_SR_PERFORMANCE); CONSTANT(FRAMELAB_SR_BALANCED); CONSTANT(FRAMELAB_SR_QUALITY);
    CONSTANT(FRAMELAB_D3D12_SR_HDR); CONSTANT(FRAMELAB_D3D12_SR_DEPTH_INVERTED);
    CONSTANT(FRAMELAB_D3D12_SR_AUTO_EXPOSURE);
    return 0;
}
