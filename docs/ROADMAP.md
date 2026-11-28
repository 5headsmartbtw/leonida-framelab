# Roadmap

Use the [coordination issue](https://github.com/5headsmartbtw/leonida-framelab/issues/4) to claim tasks and post progress. See the [contribution rules](../CONTRIBUTING.md) before submitting changes.

## Independent Vulkan handoff coverage

QA-02 reproduced all four standalone Vulkan queue-handoff cases on the Radeon 860M and the Ryzen 9 9950X3D integrated GPU. Both AMD reports verified exact 64-pixel readback, consumer fence completion and zero diagnostics with synchronization validation enabled. The AMD results extend the standalone RTX 2050 and independent RTX 4070 Ti SUPER evidence. [Device results and original reports](evidence/independent-vulkan-handoff.md#cross-vendor-validation)

## SDK track

The [experimental v0.4.0 scope](sdk/RELEASE_NOTES.md) includes the core SDK and independently verified D3D12FG, D3D12SR and VulkanSR. FL-17 and #24 hold Vulkan FG and the combined Vulkan path.

| Workstream | Priority | Implementation boundary |
| --- | --- | --- |
| SDK-01 | P0 | The public ABI and [D3D12 FG dispatch](sdk/D3D12_FG.md) are verified. Public Vulkan SR merged in [#22](https://github.com/5headsmartbtw/leonida-framelab/pull/22). Vulkan FG runs through the installed SDK in draft [#20](https://github.com/5headsmartbtw/leonida-framelab/pull/20). |
| SDK-02 | P0 | Static/shared CMake package and installed C/C++ consumers pass on Windows and Linux. |
| SR-01 | P0 | The installed [Vulkan SR component](sdk/VULKAN_SR.md) passed the 120-evaluation RTX 2050 profile, readback, resize and history reset. Independent official-runtime validation is tracked in [#24](https://github.com/5headsmartbtw/leonida-framelab/issues/24). |
| D3D-01 | P0 | The installed [public D3D12 FG component](sdk/D3D12_FG.md) passed the external consumer, real fence/lifetime checks and matched the old readback. |
| FL-14 | P0 | The opt-in [D3D12 SR component](sdk/D3D12_SR.md) and its external C/C++ consumer passed native readback, D3D12 debug-layer and GPU-based validation, timing and device removal on an RTX 5090 with the official runtime, and passed the maintainer's RTX 2050 (4 GB) merge gate. [Issue #29](https://github.com/5headsmartbtw/leonida-framelab/issues/29) |
| VK-01 | P1 | The [standalone image handoff](../tests/vulkan-handoff/README.md) verified binary and timeline waits on distinct queues. Installed FG and the combined sample cover both topologies in [#20](https://github.com/5headsmartbtw/leonida-framelab/pull/20) and [#27](https://github.com/5headsmartbtw/leonida-framelab/pull/27). Follow [#26](https://github.com/5headsmartbtw/leonida-framelab/pull/26) for loss and cancellation coverage. [Issue #2](https://github.com/5headsmartbtw/leonida-framelab/issues/2) |
| FFI-01 | P1 | [Raw Rust FFI](../bindings/rust/README.md) passes C layout and shared-library lifecycle tests on Windows and Linux. |
| INT-01 / INT-02 | P1 | Read-only [Leonida](integrations/leonida/README.md) and [VIctory](integrations/victory/README.md) audits identify consumer seams and missing evidence. |
| COMBO-01 | P1 | Combined Vulkan readback, resize and fallback are verified on the reference profile in [#23](https://github.com/5headsmartbtw/leonida-framelab/pull/23) and [#27](https://github.com/5headsmartbtw/leonida-framelab/pull/27). SyncVal remains an open gate in [#24](https://github.com/5headsmartbtw/leonida-framelab/issues/24). |
| QA-01 | P1 | The [standalone Vulkan handoff replicated on an RTX 4070 Ti SUPER](evidence/independent-vulkan-handoff.md). An official-runtime RTX 5090 report in [#24](https://github.com/5headsmartbtw/leonida-framelab/issues/24) reproduced generated output and the open SyncVal diagnostics. |

Use the [experimental release checklist](sdk/RELEASE_CHECKLIST.md) when deciding whether a profile is ready to publish.

## Completed locally

- [x] Independent NVIDIA NGX FG image on RTX 2050 / D3D12.
- [x] Native Windows Vulkan FG with a source-port layer and external SM86 dependency.
- [x] Frame generation in the sample's actual animated 3D scene.
- [x] VSync On/Off, live resize and normal-Present fallback.
- [x] API-neutral FrameInputs, typed bindings, real allocation leases and continuity checks.
- [x] Read-only Leonida pipeline audit; no backend modifications or invented game inputs.
- [x] Phase 3 image comparison, metadata rejection/recovery and validation evidence.
- [x] Installed D3D12 FG presentation with VSync, resize, fallback and fence retirement in [FL-15](https://github.com/5headsmartbtw/leonida-framelab/issues/30).

## Next milestones

1. **Independent reproduction:** rerun the configurable sample from a clean checkout on another developer's machine. Record the GPU, driver, SDK and runtime, actual present modes and any unsupported cases. See the [GPU test procedure](BUILD.md#run-the-gpu-checks).
2. **Vulkan stabilization.** Close the SyncVal gate in [#24](https://github.com/5headsmartbtw/leonida-framelab/issues/24), verify native device loss where feasible, and preserve the timeline and binary evidence in [#27](https://github.com/5headsmartbtw/leonida-framelab/pull/27).
3. **Semantic provider:** identify final-compatible color/depth/MV/camera/jitter for a real frame with evidence. Unknown bindings remain unavailable.
4. **Resource leases in Leonida:** pin actual resource/device/view owners, exact generations and completion tickets through all FG/presentation users.
5. **Shared present seam:** integrate only after previous gates, preserve tracker rollback, lock order, deferred frames and Xbox/DXGI frame-token policy.
6. **Synthetic-scene stability and pacing.** Track same-display benchmarks and stepped SR, FG, and SR → FG runs in [#25](https://github.com/5headsmartbtw/leonida-framelab/pull/25). Record GPU completion, periodic readback, resize, memory and fallback separately from throughput.
7. **Image quality and pacing:** camera cuts, occlusion, rapid motion, resize/memory pressure, scanout and latency measurement.

See the detailed [integration plan](phase3/INTEGRATION_PLAN.en.md) for acceptance conditions.

Tracking issues: [runtime paths](https://github.com/5headsmartbtw/leonida-framelab/issues/1), [multi-queue handoff](https://github.com/5headsmartbtw/leonida-framelab/issues/2), [semantic provider](https://github.com/5headsmartbtw/leonida-framelab/issues/3).

## Next SDK development stage

1. **D3D12 SR → FG.** [FL-16](https://github.com/5headsmartbtw/leonida-framelab/issues/31) follows FL-14 and FL-15. Use the real SR output as FG color. Pin its lifetime through FG completion. Validate depth and motion-vector scaling, camera/jitter conventions, command-list ordering, fences, resize and fallback. Keep each component independently usable.
2. **FidelityFX Cauldron consumer.** Use the [AMD FidelityFX SDK v1 Cauldron framework](https://gpuopen.com/fidelityfx-cauldron-framework/) as an independent D3D12 host. Add an optional render module after scene motion/depth production and before presentation, using the host's resources and completion fences. Pin and review the chosen upstream revision and licenses before adding dependencies. Keep Cauldron and NVIDIA runtime downloads outside the portable FrameLab build.
