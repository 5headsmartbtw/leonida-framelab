# Build and reproduce

## CPU contract tests — portable default

Requires CMake 3.20+ and C++20. No game data or GPU dependencies:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Pass `-DFRAMEINPUTS_VULKAN_INCLUDE=/path/to/Vulkan/Include` to build the additional CPU-only Vulkan-binding tests. Without headers, only the neutral test target is built. The validated Windows environment ran both targets (59 checks).

## Windows GPU reference — opt-in

Read [sample/README.md](../sample/README.md) first. Runtime paths are still bound to the original E:/Dev/GTA6 experiment. This is explicitly not yet a general-purpose installation package. Do not install the sample proxy in system directories.

Acquire separately under their own licenses: MSVC 2022, Vulkan SDK 1.4.363.1, nvpro_core at bc19d6ac3ef62938d0ea0e099735878457ce1b6e, Streamline SDK 2.14.1, NVIDIA/DLSS v310.9.1, and the tested local SM86 dependency with Bundled runtime 310.9.16. No script in this repository downloads these binaries or alters drivers/security settings.

Configure with FRAMELAB_BUILD_SAMPLE=ON, BASE_DIRECTORY pointing to the directory that contains nvpro_core, STREAMLINE_ROOT_DIR and DLSS_NGX_SDK_ROOT. Set VULKAN_SDK as required by the vendor framework. New CMake placement adaptations have not been GPU-revalidated outside the original directory layout; see the published evidence for the exact tested checkout.

The original GPU runner `tests/run_gpu_checks.ps1 -TaskRoot E:\Dev\GTA6` invokes the validated installation and requires its preserved baseline readback. The script's existence is not a promise that an unconfigured fresh clone can run proprietary GPU components.

## Components not built by default

`FRAMELAB_BUILD_LAYER=ON` builds only the Windows layer from source against Vulkan headers; it does not provide the external SM86 host/proxy route or an SDK runtime. Legacy probe .cmd files remain provenance/reproduction references for their original paths. No prebuilt GPU executable, SDK DLL or kernel payload is published here.
