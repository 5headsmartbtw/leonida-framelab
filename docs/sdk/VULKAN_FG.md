# Installed Vulkan Frame Generation

`VulkanFG` is the experimental Direct NGX component of FrameLab for Windows x64. It accepts a native Vulkan device, the public FrameInputs descriptor, and native image bindings. It records commands into an application-owned command buffer. The application owns queue submission, swapchain, and ordinary presentation.

## Build and consume

```powershell
cmake -S . -B build -A x64 -DBUILD_SHARED_LIBS=ON -DFRAMELAB_BUILD_VULKAN_FG=ON "-DDLSS_NGX_SDK_ROOT=<external DLSS SDK>"
cmake --build build --config Release
cmake --install build --config Release --prefix "<FrameLab install prefix>"
```

Install the Vulkan SDK and make it discoverable by CMake. Obtain the NVIDIA headers, link library, and runtime separately. The installed client needs the Vulkan SDK and the FrameLab package. It does not compile against NVIDIA headers or link against the NGX import library directly.

```cmake
find_package(FrameLab 0.4.0 EXACT CONFIG REQUIRED COMPONENTS VulkanFG)
target_link_libraries(client PRIVATE FrameLab::FrameLab)
```

Include `<framelab/vulkan_fg.h>`. The same shared package can include [D3D12FG](D3D12_FG.md). The default static and shared core builds work without either GPU component.

## Resource and queue contract

- Create a Vulkan 1.2 or later device with timeline semaphores enabled and supply a graphics/compute queue. Enable the runtime's required extensions before creating the device. The Windows reference sample enables `VK_NVX_binary_import`, `VK_NVX_image_view_handle`, `VK_KHR_push_descriptor`, `VK_KHR_external_memory_win32`, and `VK_KHR_external_semaphore_win32`. Pass existing wide-character runtime and log directories, an application project identity, and a lease on the actual Vulkan device owner. Query NGX initialization and capability through the optional report callback during create. An absent capability or failed native call leaves application presentation in charge. Serialize handle and queue access. The experimental component supports one active Vulkan FG handle per process.
- Supply equal render/output extents and native single-sample, single-mip, single-layer 2D images. Use `R16G16B16A16_SFLOAT` color in shader-read layout, a depth-only `D24_UNORM_S8_UINT` view in depth/stencil attachment layout, and `R16G16_SFLOAT` motion vectors in color-attachment layout. Color must support storage and transfer source usage as well as sampling. Depth and motion require the sampled usage used by the shader-readable NGX views. The host supplies FrameInputs camera transforms, pixel jitter, motion scale, resource and queue identities, monotonic frame indices, and explicit history resets at scene breaks. The reference profile uses Y-down, Z `[0, 1]` projection and Y-up, Z `[0, 1]` reprojection. Keep the actual allocations, views, and synchronization primitives alive via the C leases.
- For same-command-stream input, identify the same recording in the public descriptor and in both command-buffer fields. For split submissions, supply a real producer semaphore and signal operation, lease it, and match the stable semaphore identity in both descriptors. Use a binary semaphore with value zero or a timeline semaphore with the recorded producer value. For different queue families, supply exclusive images with matching producer release barriers after their final producer access. Each release keeps the declared layout, covers mip 0/layer 0 and both depth/stencil aspects for the packed depth image, and transfers ownership to the configured consumer family. FrameLab records the corresponding acquire. The declared layouts are restored after NGX. The consumer family owns the images on return. The host must transfer them back before a later use on a different family. Order producer reuse after all consumer reads. The two-queue reference client returns a timeline signal after its generated and real-frame submissions.
- Set the versioned structs' `abi_version` and `struct_size`. Record feature creation with `framelab_vulkan_fg_record_resize`, then follow the submit/retire sequence below before using it. Before recording a resize or destroying the idle handle, drain every other GPU user of the old output and invalidate host descriptors that reference it. Passing diagnostics enables synchronous readback work for the synthetic verification profile.

## Recording and completion

1. Record FG with `framelab_vulkan_fg_record` outside a render pass. `out_generated == 0` on a successful call means the source frame can still be committed to temporal history, even though there is no generated frame to present. A generated output comes from `framelab_vulkan_fg_output` and is borrowed in shader-read layout. Keep later consumer uses ordered and complete before overwrite, resize or destruction.
2. Get `framelab_vulkan_fg_submission`. Submit the recording on the configured queue, with the provided producer wait if present, and signal its completion timeline at a new strictly increasing positive value below `UINT64_MAX`. When using `vkQueueSubmit`, include matching wait and signal arrays in `VkTimelineSemaphoreSubmitInfo`. Signal only from the GPU submission that covers the recorded work. The SDK owns that semaphore. After the queue accepts the work, call `framelab_vulkan_fg_mark_submitted` with the same value.
3. Call `framelab_vulkan_fg_retire`. It reads the actual device timeline and returns `FRAMELAB_NOT_READY` while the value is pending. There is one outstanding operation per handle. A pending handle returns `FRAMELAB_BUSY` on destruction. An acknowledged device loss ends this component's GPU work. Recreate the device and backend before attempting FG again.
4. Rejected input takes the real-frame path. An input rejected before recording leaves the command buffer usable. The application still handles any already-submitted producer waits and ownership transfers on that path. A native failure after recording starts can leave partial commands. Discard the affected command recording, call `framelab_vulkan_fg_cancel` if an operation is pending, and use separate ordinary presentation commands. Cancel an intentionally abandoned recording only after it cannot be submitted. Invalidate temporal history after an idle-frame discontinuity. Treat native feature errors as a reason to fall back and reinitialize.

## Reference consumer and verification

Build the synthetic sample against the installed package. The sample consumes the Vulkan C ABI and owns the fallback, VSync controls, and present loop. See the [reference sample build](../BUILD.md) for how to supply the independently acquired runtime and sample dependencies.

```powershell
cmake -S sample -B client-build -A x64 -DFRAMELAB_USE_INSTALLED_VULKAN_FG=ON `
  "-DCMAKE_PREFIX_PATH=<FrameLab install prefix>" `
  "-DBASE_DIRECTORY=<parent directory of nvpro_core>" `
  "-DSTREAMLINE_ROOT_DIR=<external Streamline SDK>"
cmake --build client-build --config Release
$exe = "./client-build/bin/Release/vk_streamline.exe"
$baseline = "<local legacy control run>/direct/generated-5.rgba16f"
foreach ($profile in 'same-stream','cross-family','same-family') {
  ./tests/run_gpu_checks.ps1 -Executable $exe -BaselineReadback $baseline -VulkanSdkProfile $profile
  foreach ($scenario in 'off','on','missing-depth') {
    try {
      ./tests/run_gpu_checks.ps1 -Executable $exe -BaselineReadback $baseline -VulkanSdkProfile $profile -Scenario $scenario -SynchronizationValidation
    } catch { Write-Warning $_ }
  }
}
```

Supply a legacy baseline generated on the same runtime profile for byte comparison. Preserve the runner's logs when reporting failure. Missing queue topology is reported as skipped.

The installed client ran on Windows 11 build 26200, RTX 2050 Laptop (SM 8.6, 4 GB), NVIDIA driver 617.14, MSVC 2022, Vulkan SDK/layers 1.4.363.1, Streamline 2.14.1 and DLSS SDK v310.9.1. It reused the separately supplied SM86 0.4.2/Bundled runtime 310.9.16 and Windows layer from the [Phase 3 reference profile](../phase3/VALIDATION.en.md). NGX returned success (`0x1`) from initialization and feature creation, and availability reported `1`.

| Local check | Result |
| --- | --- |
| Same command stream, same-family timeline handoff, and cross-family binary handoff, Immediate and FIFO | Each of six runs evaluated 6 source frames, verified and presented 5 generated images, and logged 0 errors / 0 warnings under standard Vulkan validation. |
| Missing depth with all three topologies | 5 evaluations, 3 verified generated images, 6 real presents, 1 expected rejection, and 0 errors / 0 warnings in each run. Frame 2 bypassed NGX and frame 3 reset history. |
| Readback and GPU ownership | All three topologies matched the legacy control SHA-256 `caa4a0256f6920a8fc596fc414680d9544b72abff5c7f6e3c148a6d6924d96a2`. The final gated submit refused early retirement and destruction. The actual GPU signal preceded the final lease release. |
| Live FIFO resize | Recreated NGX at 1008×601 after starting at 1280×720. Exit 0, 0 errors / 0 warnings and empty stderr. Readback disabled for the live run. |
| Extended synchronization validation | Failed on all three exercised topologies. The same-stream, cross-family and preserved legacy Immediate checks reached the default duplicate-message limit at 10 errors. The same-stream and cross-family FIFO checks also reached 10, and their missing-depth runs reported 9. The same-family rerun used `VK_LAYER_DUPLICATE_MESSAGE_LIMIT=10000` and reported 11 errors in each control and 9 with missing depth. All runs had 0 warnings. The reported hazards after correcting the reference client's reuse barriers were write-after-write in `vkCmdClearColorImage` and `vkCmdFillBuffer` on private `nv.ngx.dlssg.resource` objects. FrameLab did not suppress diagnostics. |
| Regression checks | The rebuilt legacy Vulkan sample passed all three standard GPU cases. Both contract tests and all three installed C/C++ tests passed. The D3D12 client rebuilt against the same DLL, verified 5 generated images and matched the [original D3D12 readback SHA-256](D3D12_FG.md#hardware-evidence). |
| Queue selection | The RTX 2050 exposed two distinct graphics/compute queues in family 0 for the timeline test. The client reserves the producer before Streamline configures its queues. A device without a suitable topology reports a skip. |
| Other outstanding checks | Native cancellation and failed submission fault injection, capability-negative hardware, device loss, independent GPU replication, reusable SR dispatch and combined SR + FG. |

## Two-frame NGX SyncVal reproducer

Build the legacy sample and the installed-package sample from the same revision. Keep the validation layers and externally acquired runtime constant. The trace runs both variants with a single command stream and two frames. It omits diagnostic image clears and readback work and counts the validation callbacks between entry to and return from `NGX_VK_EVALUATE_DLSSG`. The script preserves full logs and fails on a validation message.

```powershell
./tests/trace_ngx_fg.ps1 -InstalledExecutable <installed-consumer-exe> -LegacyExecutable <legacy-exe>
```

On the RTX 2050 reference runtime, both variants returned `0x1` for each native evaluation and emitted 8 errors / 0 warnings. Each callback was inside an evaluation and named a private NGX image cleared by `vkCmdClearColorImage`. There were no callbacks outside the two evaluation calls. The six-frame workload also reports private buffer fills. That larger workload is required for the actual generated-image readback and the private-buffer diagnostics.

Run the normal installed verification with `-VerifyNativeResizeCancellation` to record native feature creation, discard the Vulkan command buffer before submission, cancel the SDK operation, and then recreate and verify FG. The RTX 2050 same-stream Immediate run recovered with 6 evaluations, 5 verified generated images, the original readback hash and 0 errors / 0 warnings. This exercises recovery from abandoned native creation work. Actual Vulkan queue-submit failure injection remains unverified.

To cancel an actual Evaluate recording, start the installed client with:

```powershell
& $exe --verify-direct-fg --frames 6 --validation --no-vsync --sdk-handoff-same --sdk-cancel-evaluate-frame 2
```

The client submits the actual scene producer first, records Evaluate for source frame 2 into a separate consumer command buffer, and discards that buffer before SDK cancellation. The fallback submission still waits for the producer and presents the original raster result. The RTX 2050 logged six native Evaluate recordings, five submitted Evaluate command buffers, six real presents, and three generated images verified and presented. Frame 2 had no FG completion, frame 3 reset history, and standard validation reported 0 errors and 0 warnings.
