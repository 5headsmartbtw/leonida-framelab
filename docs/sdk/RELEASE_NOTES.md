# FrameLab SDK v0.4.0 Experimental

This source release contains the portable C ABI v1, static and shared CMake packages, installed C/C++ examples and raw Rust core bindings. It also contains three opt-in Windows x64 components for verified synthetic workloads.

| Component | Tested profile | Verification |
| --- | --- | --- |
| Core SDK | Windows and Linux | Portable tests, external C/C++ consumers for both library variants, and installed shared-library Rust layout and lease tests. |
| [D3D12SR](D3D12_SR.md) | RTX 2050 Laptop 4 GiB, NVIDIA driver 617.14, official DLSS SDK/runtime 310.9.1.0 | 23 verified evaluations, 38 lifecycle checks, 17 lease releases, resize, fallback, shared-session handling and device-removal recovery. Debug and GPU-based validation each reported zero errors and warnings. |
| [D3D12FG](D3D12_FG.md) | Same RTX 2050, experimental SM86 0.4.2 / bundled runtime 310.9.16 | Six submissions, five verified generated images, three final leases released, and matching readbacks across the 720p regression comparison. The installed client makes no Present calls. |
| [VulkanSR](VULKAN_SR.md) | Same RTX 2050, Vulkan SDK and Khronos validation 1.4.363.1, Streamline 2.14.1 | 120 evaluations and two readbacks at 1280×720 with standard validation. A missing-depth run evaluated five frames and used ordinary rendering once. A separate six-frame synchronization-validation run reported zero errors and warnings. |

[D3D12 verification](https://github.com/5headsmartbtw/leonida-framelab/pull/36#pullrequestreview-6084624039) used `73f9daed9d4e1e7d3a1ccc968bdd985031c3c8be`. The exact tree also built the three opt-in components in one installed package for the Vulkan SR run. The Windows runs used build 26200 and MSVC 19.44. JoeY54 separately verified official-runtime D3D12 SR on an RTX 5090. The RTX 4070 Ti SUPER and AMD records cover the standalone Vulkan handoff fixture. [Independent SR results](D3D12_SR.md) · [Vulkan handoff reports](../evidence/independent-vulkan-handoff.md)

### RTX 2050 regression comparison

Baseline `2d25525db8418aa111a30614f07b9f49146a2313` versus candidate `73f9daed9d4e1e7d3a1ccc968bdd985031c3c8be`, at 1280×720. Each mode used three calibration runs followed by three interleaved runs per revision, each with 60 warm-up and 240 measured source frames. The same headless synthetic D3D12 scene and direct queue ran on both revisions. Numerical tolerances used twice the baseline range with a floor of two reporting units. The fallback path rejected a depth binding before native FG evaluation and copied the ordinary source image. A separate verification ran six fallback frame readbacks per revision with zero debug and GPU-based validation diagnostics.

| Metric | FG baseline | FG candidate | FG tolerance | Fallback baseline | Fallback candidate | Fallback tolerance |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| GPU p50 (ms) | 6.1258 | 6.1309 | 0.0602 | 0.3976 | 0.3803 | 0.0148 |
| GPU p95 (ms) | 6.2591 | 6.2504 | 0.0496 | 0.9337 | 0.8709 | 0.1560 |
| GPU p99 (ms) | 6.6681 | 6.8152 | 0.3470 | 1.4029 | 1.2055 | 0.0338 |
| Source interval p50 (ms) | 17.9980 | 17.8405 | 1.5294 | 15.3568 | 15.3153 | 2.0076 |
| Source interval p95 (ms) | 20.8796 | 20.2061 | 1.3922 | 20.5352 | 19.0295 | 4.9176 |
| Source FPS equivalent (1000 / median interval) | 55.56 | 56.05 | 4.70 | 65.12 | 65.29 | 8.46 |
| Steady / sampled peak local VRAM (MiB) | 179.20 / 179.20 | 179.20 / 179.20 | 0.02 | 164.26 / 164.26 | 164.26 / 164.26 | 0.02 |
| Minimum local budget (MiB) | 3367.70 | 3367.70 | 0.02 | 3367.70 | 3367.70 | 0.02 |

The three-run medians showed no regression beyond tolerance. One interleaved baseline fallback run had elevated source and GPU intervals. Both fallback medians for the candidate also passed against the original calibration. Both FG outputs hashed to `3251fb1b43c329882cfa8236c2bb2828882fd608a19372feff7423bcb2b79128`. Headless source intervals, generated-image readbacks and display measurements are separate.

D3D12 SR is a new mode. The 600-evaluation Quality run at 853×480 → 1280×720 measured 2.9676 ms GPU p50, 2.9768 ms p95, 3.3783 ms CPU interval p50, 92.5 MiB steady and peak local usage, and a 3367.7 MiB budget. A deliberate host allocation fault during NGX session setup returned `FRAMELAB_OUT_OF_MEMORY` and allowed retry, sharing, and final cleanup. Physical VRAM exhaustion was skipped.

## Build from source

```sh
cmake -S . -B build -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix ./install
```

Set `BUILD_SHARED_LIBS=ON` in a separate build directory for a shared library. Configure external consumers with the install prefix in `CMAKE_PREFIX_PATH`.

```cmake
find_package(FrameLab 0.4.0 EXACT CONFIG REQUIRED)
target_link_libraries(client PRIVATE FrameLab::FrameLab)
```

The core package needs a C++20 toolchain and CMake 3.20+. Each Windows x64 component must be enabled explicitly in a shared build. Its guide lists the external SDK, runtime and exact `find_package` component name. Use the [installed C/C++ examples](../../tests/external-consumer/) and [Rust ABI instructions](../../bindings/rust/README.md).

## Open gates

- [#24](https://github.com/5headsmartbtw/leonida-framelab/issues/24) holds installed Vulkan FG and combined Vulkan SR → FG. Both legacy and installed FG traces reproduced eight extended SyncVal write-after-write diagnostics with zero standard validation errors and warnings. The official RTX 5090 report also records separate strict-runner libpng warnings. The cause is open. The [NGX-free control](../../tests/vulkan-syncval-control/README.md) is available for triage. The included legacy Vulkan sample remains a research fixture.
- D3D12 SR and FG take mutually exclusive NGX session ownership. [FL-15](https://github.com/5headsmartbtw/leonida-framelab/issues/30) tracks installed-client DXGI presentation. [FL-16](https://github.com/5headsmartbtw/leonida-framelab/issues/31) tracks combined operation.
- The hardware checks cover the exact synthetic profiles above. They did not measure physical scanout, latency or game behavior. The RTX 2050 experimental FG result does not establish official support for that GPU.
- [VIctory](../integrations/victory/README.md), Leonida and Cauldron are downstream research tracks. Production use and game compatibility remain unverified.

The release ships source with [MIT and upstream notices](../../NOTICE.md). Acquire vendor SDKs and runtimes separately. Thanks to JoeY54 for FL-14 and SyncVal research, and to akimfomin and the independent hardware testers for validation and review. Codex assisted with release preparation, review and validation.
