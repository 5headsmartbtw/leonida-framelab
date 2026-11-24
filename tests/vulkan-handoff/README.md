# Synthetic Vulkan queue handoff

This standalone test verifies real GPU ordering and image ownership apart from DLSS. One queue clears an exclusive 8×8 image. A distinct queue waits on the producer's binary or timeline semaphore and copies the image to a host-visible buffer. Different queue families use matching release/acquire barriers and a layout transition. The test compares all 64 pixels after the consumer fence and runs the Khronos synchronization validation layer.

```sh
cmake -S tests/vulkan-handoff -B build-handoff
cmake --build build-handoff --config Release
ctest --test-dir build-handoff -C Release -V
```

A Vulkan 1.2 device and the validation layer are required. The test reports unavailable queue configurations explicitly. A run with no two-queue configuration is skipped. SDK/validation discovery uses the standard Vulkan loader settings.

On the reference RTX 2050 Laptop, NVIDIA driver 617.14 and Vulkan SDK 1.4.363.1, all four cases passed with zero validation errors or warnings. These were distinct queues in family 0 and queues across families 0 → 1, each with binary and timeline waits. An earlier run with an NVIDIA-only loader filter failed the strict warning check because the loader warned it had skipped the AMD driver. With the filter removed the selected device was still the RTX 2050 and the run was clean.

The Direct NGX consumer still rejects external producer completion. Integration requires typed primitive ownership, matching frame-resource layout transitions, retirement for every GPU user, and failure/reuse/resize coverage before enabling a multi-queue FG profile. Track this in [VK-01 / #2](https://github.com/5headsmartbtw/leonida-framelab/issues/2).
