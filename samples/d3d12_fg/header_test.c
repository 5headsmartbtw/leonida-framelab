/* SPDX-License-Identifier: MIT */
#include <framelab/d3d12_fg.h>

#include <stddef.h>

#ifdef _WIN64
/* render_extent was appended; the v1 prefix that older callers pass must not move. */
_Static_assert(offsetof(FrameLabD3D12FgConfig, report_user) == 96, "FrameLabD3D12FgConfig v1 prefix");
_Static_assert(offsetof(FrameLabD3D12FgConfig, render_extent) == 104, "FrameLabD3D12FgConfig.render_extent");
_Static_assert(sizeof(FrameLabD3D12FgConfig) == 112, "FrameLabD3D12FgConfig size");
#endif

int main(void)
{
    FrameLabD3D12FgConfig config = {0};
    FrameLabD3D12Fg* fg = 0;
    /* sizeof the v0.4.0 struct: the v1 fields rounded up to the struct alignment. */
    const uint32_t align = (uint32_t)_Alignof(FrameLabD3D12FgConfig);
    const uint32_t v1 = ((uint32_t)offsetof(FrameLabD3D12FgConfig, render_extent) + align - 1) / align * align;
    uint32_t size;
    if (framelab_abi_version() != FRAMELAB_ABI_VERSION ||
        framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_ABI_MISMATCH || fg)
        return 1;
    config.abi_version = FRAMELAB_ABI_VERSION;
    /* The v1 size and the full size pass the size check and stop at the missing device. */
    config.struct_size = v1;
    if (framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_INVALID_ARGUMENT || fg)
        return 2;
    config.struct_size = sizeof config;
    if (framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_INVALID_ARGUMENT || fg)
        return 3;
    /* Shorter than v1, or between v1 and the full size: rejected. */
    for (size = 0; size < sizeof config; ++size) {
        if (size == v1) continue;
        config.struct_size = size;
        if (framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_ABI_MISMATCH || fg)
            return 4;
    }
    return framelab_d3d12_fg_destroy(fg) == FRAMELAB_OK ? 0 : 5;
}
