/* SPDX-License-Identifier: MIT */
#include <framelab/vulkan_sr.h>

int main(void)
{
    FrameLabVulkanSrConfig config = {0};
    FrameLabVulkanSr* sr = 0;
    if (framelab_vulkan_sr_create(&config, &sr) != FRAMELAB_ABI_MISMATCH || sr)
        return 1;
    config.struct_size = sizeof config;
    config.abi_version = FRAMELAB_ABI_VERSION;
    if (framelab_vulkan_sr_create(&config, &sr) != FRAMELAB_INVALID_ARGUMENT || sr)
        return 2;
    if (framelab_vulkan_sr_release_lost_device(sr) != FRAMELAB_INVALID_ARGUMENT)
        return 3;
    return framelab_vulkan_sr_destroy(sr) == FRAMELAB_OK ? 0 : 4;
}
