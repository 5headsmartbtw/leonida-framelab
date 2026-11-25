# Installed Vulkan Super Resolution

`VulkanSR` exposes Streamline DLSS Super Resolution through the FrameLab C ABI on Windows x64. It uses the same Streamline options, camera constants and tagged-image evaluation as the standalone Vulkan SR reference. The application owns Vulkan submission and presentation.

## Build and link

```powershell
cmake -S . -B build -A x64 -DBUILD_SHARED_LIBS=ON -DFRAMELAB_BUILD_VULKAN_SR=ON "-DFRAMELAB_STREAMLINE_SDK_ROOT=<external Streamline SDK>"
cmake --build build --config Release
cmake --install build --config Release --prefix "<FrameLab install prefix>"
```

```cmake
find_package(FrameLab 0.4.0 EXACT CONFIG REQUIRED COMPONENTS VulkanSR)
target_link_libraries(client PRIVATE FrameLab::FrameLab)
```

Include `<framelab/vulkan_sr.h>`. The installed package needs Vulkan headers and loader on this profile. The client does not link against a Streamline import library to call FrameLab. The host initializes its separately acquired Streamline runtime and registers Vulkan using the [NVIDIA initialization guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md). FrameLab acquires a reference to the already-loaded interposer at the supplied absolute path, checks real SR availability and reports native result codes through the optional callback. Give each handle an exclusive Streamline viewport. Device and runtime lifetime must cover the handle. Serialize handle calls and enable Vulkan timeline semaphores when creating the device.

## Record and retire

1. Fill each versioned descriptor's ABI version and size. Configure the output extent and quality mode. Use the returned optimal render extent for inputs. Reconfiguration requires an idle backend and resets temporal history.
2. Supply actual RGBA16F color, D24S8 depth with a depth-only view, RG16F motion vectors, and a distinct RGBA16F storage output at the output extent. Use one mip, one layer and one sample. The producer records on the configured graphics/compute queue and in the same command buffer as SR. Color and motion are in color-attachment layout, depth in depth/stencil attachment layout, and output in shader-read layout. Include actual usage and ownership, normalized previous-minus-current motion, render-pixel jitter, and row-major camera transforms. Keep each image, view and memory allocation alive through the supplied lease.
3. Call `framelab_vulkan_sr_record` outside rendering and submit the same command buffer with the SDK completion timeline at a new increasing GPU signal value. Acknowledge that value after successful submission. `framelab_vulkan_sr_retire` reads the real Vulkan timeline before releasing resources. The app also retains any later users. A pending operation prevents destruction.
4. On rejected metadata, present the raster result and let the next accepted frame reset history. A native recording failure may have emitted commands. Discard the recording before cancel and record fallback into a safe command buffer. Cancel and explicit invalidation reset history.

On `VK_ERROR_DEVICE_LOST`, stop submissions and keep the device and runtime lease. A failed timeline query leaves the packet retained. Once the host has drained the old device and discarded host recordings, call `framelab_vulkan_sr_release_lost_device` and destroy the handle. Then tear down the old runtime and device. Resume with a new device, resources and SR handle, with reset history. The release operation also covers loss observed by the host during submission or a wait. It does not wait on the device or attempt recreation.

## Reference validation

Build the [synthetic scene](../BUILD.md) separately with `FRAMELAB_USE_INSTALLED_VULKAN_SR=ON` and the installed package in `CMAKE_PREFIX_PATH`. Use `--verify-sdk-sr --validation` for the 120-frame case. Add `--frames 6 --sdk-sr-reject-frame 2` for a missing-depth fallback and reset test. Set `VK_VALIDATION_VALIDATE_SYNC=1` for the separate extended run.

On RTX 2050 Laptop, Windows 11 build 26200, NVIDIA 617.14, Vulkan SDK 1.4.363.1 and Streamline 2.14.1, the installed client passed 120 evaluations from 640×360 to 1280×720 with 0 standard validation errors and warnings. The first and final full RGBA16F readbacks held finite, varying pixels. The final SHA-256 was `bdfdab96b1bc238f670e0ec109da9a45fd66c23003cdc0ec84be9e167e588f3a`. The six-frame missing-depth test accepted five frames, used one ordinary raster fallback, and observed two history resets with 0 validation errors and warnings. The separate six-frame control with SyncVal also reported 0 errors and warnings.

A live Windows resize from 1280×720 to 1008×601 reconfigured the input to 504×301, reset history, and produced three verified readbacks over 120 evaluations with 0 validation errors and warnings.

A host Vulkan call that returns `VK_ERROR_DEVICE_LOST` should notify the SR handle with `framelab_vulkan_sr_notify_device_lost`. This freezes new work and keeps outstanding image leases. Once the old device's work is no longer pending and the host has discarded its recordings, call `framelab_vulkan_sr_release_lost_device` before destroying the backend and the device. A fresh backend starts new history.
