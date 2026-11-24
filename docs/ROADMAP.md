# Roadmap

Use the [coordination issue](https://github.com/5headsmartbtw/leonida-framelab/issues/4) to claim tasks and post progress. See the [contribution rules](../CONTRIBUTING.md) before submitting changes.

## Completed locally

- [x] Independent NVIDIA NGX FG image on RTX 2050 / D3D12.
- [x] Native Windows Vulkan FG with a source-port layer and external SM86 dependency.
- [x] Frame generation in the sample's actual animated 3D scene.
- [x] VSync On/Off, live resize and normal-Present fallback.
- [x] API-neutral FrameInputs, typed bindings, real allocation leases and continuity checks.
- [x] Read-only Leonida pipeline audit; no backend modifications or invented game inputs.
- [x] Phase 3 image comparison, metadata rejection/recovery and validation evidence.

## Next milestones

1. **Independent reproduction:** rerun the configurable sample from a clean checkout on another developer's machine. Record the GPU, driver, SDK and runtime, actual present modes and any unsupported cases. See the [GPU test procedure](BUILD.md#run-the-gpu-checks).
2. **Multi-queue completion:** implement real timeline/binary waits and queue-family ownership transfers in an independent harness. Never label a different producer submission as the same command stream.
3. **Semantic provider:** identify final-compatible color/depth/MV/camera/jitter for a real frame with evidence. Unknown bindings remain unavailable.
4. **Resource leases in Leonida:** pin actual resource/device/view owners, exact generations and completion tickets through all FG/presentation users.
5. **Shared present seam:** integrate only after previous gates, preserve tracker rollback, lock order, deferred frames and Xbox/DXGI frame-token policy.
6. **SR + FG on the synthetic scene:** first fix the [SR-only Vulkan validation findings](phase3/VALIDATION.en.md#configurable-runtime-rerun). Then verify SR-only, FG-only, and combined SR → FG separately. Define the color resolution, depth and MV resolution, jitter, motion-vector scaling, and reset rules at the boundary. Require generated-frame readback, validation and ordinary Present fallback. The current direct Vulkan consumer requires equal render and output extents.
7. **Image quality and pacing:** camera cuts, occlusion, rapid motion, resize/memory pressure, scanout and latency measurement.

See the detailed [integration plan](phase3/INTEGRATION_PLAN.en.md) for acceptance conditions.

Tracking issues: [runtime paths](https://github.com/5headsmartbtw/leonida-framelab/issues/1), [multi-queue handoff](https://github.com/5headsmartbtw/leonida-framelab/issues/2), [semantic provider](https://github.com/5headsmartbtw/leonida-framelab/issues/3).
