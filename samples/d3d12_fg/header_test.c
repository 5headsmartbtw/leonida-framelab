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
    if (framelab_abi_version() != FRAMELAB_ABI_VERSION ||
        framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_ABI_MISMATCH || fg)
        return 1;
    /* A caller built against the v1 layout passes the shorter size: accepted up to argument checks. */
    config.abi_version = FRAMELAB_ABI_VERSION;
    config.struct_size = (uint32_t)offsetof(FrameLabD3D12FgConfig, render_extent);
    if (framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_INVALID_ARGUMENT || fg)
        return 2;
    config.struct_size -= 1;
    if (framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_ABI_MISMATCH || fg)
        return 3;
    config.struct_size = sizeof config;
    if (framelab_d3d12_fg_create(&config, &fg) != FRAMELAB_INVALID_ARGUMENT || fg)
        return 4;
    return framelab_d3d12_fg_destroy(fg) == FRAMELAB_OK ? 0 : 5;
}
