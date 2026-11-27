/* SPDX-License-Identifier: MIT */
#include <framelab/d3d12_fg.h>

#include <stddef.h>

#ifdef _WIN64
/* render_extent, then color_format and flags, were appended; the prefixes that older callers pass
 * must not move. */
_Static_assert(offsetof(FrameLabD3D12FgConfig, report_user) == 96, "FrameLabD3D12FgConfig v1 prefix");
_Static_assert(offsetof(FrameLabD3D12FgConfig, render_extent) == 104, "FrameLabD3D12FgConfig.render_extent");
_Static_assert(offsetof(FrameLabD3D12FgConfig, color_format) == 112, "FrameLabD3D12FgConfig.color_format");
_Static_assert(offsetof(FrameLabD3D12FgConfig, flags) == 116, "FrameLabD3D12FgConfig.flags");
_Static_assert(sizeof(FrameLabD3D12FgConfig) == 120, "FrameLabD3D12FgConfig size");
#endif
_Static_assert(FRAMELAB_FORMAT_RGB10A2_UNORM == 9 && FRAMELAB_D3D12_FG_HDR10 == 1, "color constants");

/* sizeof an older layout: its fields rounded up to the struct alignment. */
static uint32_t layout_size(size_t fields)
{
    const size_t align = _Alignof(FrameLabD3D12FgConfig);
    return (uint32_t)((fields + align - 1) / align * align);
}

int main(void)
{
    FrameLabD3D12FgConfig config = {0};
    FrameLabD3D12Fg* fg = 0;
    const uint32_t v1 = layout_size(offsetof(FrameLabD3D12FgConfig, render_extent));
    const uint32_t extent = layout_size(offsetof(FrameLabD3D12FgConfig, color_format));
    uint32_t size;
    if (framelab_abi_version() != FRAMELAB_ABI_VERSION ||
        framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_ABI_MISMATCH || fg)
        return 1;
    config.abi_version = FRAMELAB_ABI_VERSION;
    /* The v0.4.0 size, the render_extent size and the full size pass the size check and stop at
     * the missing device. */
    config.struct_size = v1;
    if (framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_INVALID_ARGUMENT || fg)
        return 2;
    config.struct_size = extent;
    if (framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_INVALID_ARGUMENT || fg)
        return 3;
    config.struct_size = sizeof config;
    if (framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_INVALID_ARGUMENT || fg)
        return 4;
    /* Shorter than v1, or between the published sizes: rejected. */
    config.struct_size = v1 - 1;
    if (framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_ABI_MISMATCH || fg)
        return 5;
    for (size = v1 + 1; size < sizeof config; ++size) {
        if (size == extent) continue;
        config.struct_size = size;
        if (framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_ABI_MISMATCH || fg)
            return 6;
    }
    return framelab_d3d12_fg_destroy(fg) == FRAMELAB_OK ? 0 : 7;
}
