# Synthetic Vulkan SR → FG

The combined client links the optional VulkanSR and VulkanFG components from one installed FrameLab package. It dispatches SR on the scene's actual low-resolution color, depth, and motion vectors, then supplies the SR output as FG color on the same Vulkan command stream.

## Frame contract

- SR performance mode renders 640×360 for a 1280×720 output on the reference profile. FG receives 1280×720 RGBA16F from SR. The client reads back SR output and FG output independently.
- D24 depth and RG16F motion vectors are mapped from the same source frame to the FG extent using nearest Vulkan blits. The depth aspect is copied without stencil. The device must advertise the source and destination blit format features. This preserves sampled depth values and normalized previous-minus-current UV motion. Motion scale and unjittered view and reprojection matrices stay the same. The combined profile uses zero jitter.
- The client records SR, the depth/MV blits, FG, and post-processing in order. Blits use matching transition barriers, and FG receives a lease covering the real SR output and both mapped images. The host signals both SDK timelines from the submit, and both components retire only after observing GPU completion.
- On missing SR depth, the client presents the original raster result, skips FG, and resets both histories before recovery. When only FG is unavailable, SR can still present. Resizing drains previous work, recalculates the SR extent, recreates FG and resets both histories.

## Reproduce

Build FrameLab with both `FRAMELAB_BUILD_VULKAN_SR=ON` and `FRAMELAB_BUILD_VULKAN_FG=ON`, using the optional external SDKs in the [SR guide](VULKAN_SR.md) and [FG guide](VULKAN_FG.md). Install it and build the separately configured synthetic sample with both `FRAMELAB_USE_INSTALLED_VULKAN_SR=ON` and `FRAMELAB_USE_INSTALLED_VULKAN_FG=ON`. Point `CMAKE_PREFIX_PATH` to the installed package. Obtain the runtime independently.

```powershell
foreach ($scenario in 'combined','fifo','fallback','sr-only','fg-only') {
  ./tests/run_combined_gpu_checks.ps1 -Executable $exe -Scenario $scenario -LogRoot "./logs/$scenario"
}
./tests/run_combined_gpu_checks.ps1 -Executable $exe -Scenario combined -LogRoot './logs/syncval' -SynchronizationValidation
```

For a live resize, run `--verify-sdk-combined --validation --vsync --frames 24` and change the window size after the first SR readback.

## Reference GPU

RTX 2050 Laptop, Windows 11 build 26200, NVIDIA driver 617.14, Vulkan SDK/layers 1.4.363.1, Streamline 2.14.1, and the separately supplied reference FG runtime. This is one synthetic, single-command-stream validation profile. Native failure injection, loss of device, alternate GPUs, cross-queue combined work and production image quality remain unverified.

| Standard validation | Actual result |
| --- | --- |
| SR only | Six evaluations and two verified SR readbacks. |
| FG only | Six evaluations and five generated images presented and verified. The saved control image matched SHA-256 `caa4a0256f6920a8fc596fc414680d9544b72abff5c7f6e3c148a6d6924d96a2`. |
| Combined Immediate and FIFO | Six SR and six FG evaluations in each run. Two SR readbacks and five generated images verified per run, with six real presents and five generated presents. The Immediate combined control image had SHA-256 `1919e57fad2e11da6d24e9d83e78e98f6b1e1513d67c6e710cb3ef88801e2582`. |
| Combined missing SR depth | Six real presents, one raster fallback, five SR evaluations, five FG evaluations, and three verified generated presents. Both components reset history before recovery. |
| Combined live FIFO resize | Reconfigured from 1280×720 to 1008×601. Ran 24 SR and 24 FG evaluations, verified 3 SR readbacks and 21 generated images, and observed a second SR history reset. |

Every row reported zero standard validation errors and warnings. Extended SyncVal completed the combined six-frame GPU workload and returned failure with 11 write-after-write diagnostics, all on private NGX objects. FrameLab left the diagnostics enabled. See the [FG tracing procedure](VULKAN_FG.md#two-frame-ngx-syncval-reproducer) for the matching legacy and installed SDK recording-boundary evidence.
