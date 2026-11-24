/* SPDX-License-Identifier: MIT */
#include <framelab/vulkan_fg.h>

int main(void)
{
    FrameLabVulkanFgConfig config = {0};
    FrameLabVulkanFg* fg = 0;
    return framelab_abi_version() == FRAMELAB_ABI_VERSION &&
        framelab_vulkan_fg_create(&config, &fg) == FRAMELAB_ABI_MISMATCH &&
        !fg && framelab_vulkan_fg_destroy(fg) == FRAMELAB_OK ? 0 : 1;
}
