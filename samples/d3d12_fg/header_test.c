/* SPDX-License-Identifier: MIT */
#include <framelab/d3d12_fg.h>

int main(void)
{
    FrameLabD3D12FgConfig config = {0};
    FrameLabD3D12Fg* fg = 0;
    return framelab_abi_version() == FRAMELAB_ABI_VERSION &&
        framelab_d3d12_fg_create(&config, &fg) == FRAMELAB_ABI_MISMATCH &&
        !fg && framelab_d3d12_fg_destroy(fg) == FRAMELAB_OK ? 0 : 1;
}
