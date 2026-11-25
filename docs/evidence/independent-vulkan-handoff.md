# Independent Vulkan handoff validation

**Contributor:** [@akimfomin](https://github.com/akimfomin) — [PortBridge](https://github.com/akimfomin/PortBridge)

**Date:** 2026-11-24

**Tested revision:** `82f069e82197e61b97f60bbcbcb7a3af7d3f8e77`

| Parameter | Configuration |
|---|---|
| GPU | NVIDIA RTX 4070 Ti SUPER |
| OS | Windows 11 build 28000 |
| Driver | 617.14 |
| Vulkan loader | 1.4.341 |
| Vulkan device API | 1.4.351 |
| Validation layer | 1.3.285 |
| Compiler | Clang 23.1.6 |

## Results

| Test | Result |
|---|---|
| Same-family binary semaphore | PASS |
| Same-family timeline semaphore | PASS |
| Cross-family binary semaphore | PASS |
| Cross-family timeline semaphore | PASS |

- CTest: 1/1 passed.
- Image readback: 64/64 pixels matched in each case.
- Consumer fence completion: successful in all four cases.
- Validation diagnostics: 0 in the successful run.
- Initial run required resolving a missing validation layer.

This independently reproduces the standalone Vulkan handoff mechanism on a second NVIDIA hardware configuration. It does not establish DLSS FG or SR compatibility on this GPU.

Special thanks to @akimfomin and PortBridge for FrameLab's first independent hardware validation.

[Original test report](https://github.com/5headsmartbtw/leonida-framelab/pull/12#issuecomment-7142089405)

[PortBridge continuity pilot](https://github.com/akimfomin/PortBridge/issues/41)

## Cross-vendor validation

[@JoeY54](https://github.com/JoeY54) independently ran the same pinned fixture on two AMD integrated GPUs in [QA-02 / #15](https://github.com/5headsmartbtw/leonida-framelab/issues/15), coordinated by @akimfomin through [PortBridge Test Lab](https://akimfomin.github.io/PortBridge/#/lab?task=framelab-12-vulkan-handoff). FrameLab reviewed the contributor reports against the [pinned harness](https://github.com/5headsmartbtw/leonida-framelab/blob/82f069e82197e61b97f60bbcbcb7a3af7d3f8e77/tests/vulkan-handoff/handoff.cpp) and accepted both. The hardware results are from JoeY54's runs.

| Setup | Radeon 860M | Ryzen 9 9950X3D integrated Radeon Graphics |
| --- | --- | --- |
| Vulkan PCI ID | `1002:1114` | `1002:13C0`, subsystem `88771043`, rev C9 |
| OS | Windows 11 25H2 x64, build 26200.9711 | Windows 11 Pro 26H2, build 26300.9974 |
| AMD driver | 26.11.1 (LLPC), Windows 32.0.32015.16001 | 26.11.2, Windows 32.0.21045.29003 |
| Vulkan device API | 1.4.359 | 1.4.315 |
| SDK, loader and Khronos layer API | 1.4.363.1 / 1.4.363 / 1.4.363 | 1.4.363.1 / 1.4.363 / 1.4.363 |
| Release toolchain | MinGW GCC 13.1.0, CMake 4.2.2, Ninja | MSVC 19.52.37029.0 x64, CMake 4.3.1-msvc1, Windows SDK 10.0.26100.0 |
| Report and commands | [Radeon 860M](https://github.com/5headsmartbtw/leonida-framelab/issues/15#issuecomment-7147944059) | [Ryzen 9 9950X3D iGPU](https://github.com/5headsmartbtw/leonida-framelab/issues/15#issuecomment-7153817545) |

| Scenario | Radeon 860M | Ryzen 9 9950X3D iGPU |
| --- | --- | --- |
| Binary, distinct queues in family 0 | PASS | PASS |
| Timeline, distinct queues in family 0 | PASS | PASS |
| Binary, family 0 → 1 | PASS | PASS |
| Timeline, family 0 → 1 | PASS | PASS |

Every case reported a completed consumer fence and 64/64 exact pixels from the synthetic 8×8 `R32_UINT` image. Both final runs reported CTest 1/1, zero skips, and zero validation errors or warnings. The harness requests `VK_LAYER_KHRONOS_validation` with synchronization validation, and its warning/error counter is part of the exit verdict.

On the dual-GPU Ryzen host, the first run failed to load the AMD ICD with a forward-slash manifest path. Using native path separators fixed discovery, but the next run still failed the diagnostic gate on loader warnings while using the implicit-layer disable filter. The final run selected the installed AMD ICD, pointed explicit layer discovery at the Vulkan SDK, used an empty implicit-layer discovery directory and cleared the disable filter. These process-local settings kept Khronos synchronization validation and its diagnostic callback active. See the [driver and layer setup in the report](https://github.com/5headsmartbtw/leonida-framelab/issues/15#issuecomment-7153817545) and the [Khronos loader documentation](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md#layer-discovery).

The AMD results cover the standalone Vulkan image handoff fixture. DLSS FG and SR were outside this test scope. The [native NGX SyncVal gate](https://github.com/5headsmartbtw/leonida-framelab/issues/24) remains open.
