# Roadmap

## Completed locally

- [x] Independent NVIDIA NGX FG image on RTX 2050 / D3D12.
- [x] Native Windows Vulkan FG with a source-port layer and external SM86 dependency.
- [x] Frame generation in the sample's actual animated 3D scene.
- [x] VSync On/Off, live resize and normal-Present fallback.
- [x] API-neutral FrameInputs, typed bindings, real allocation leases and continuity checks.
- [x] Read-only Leonida pipeline audit; no backend modifications or invented game inputs.
- [x] Phase 3 image comparison, metadata rejection/recovery and validation evidence.

## Next milestones

1. **Portable runtime configuration:** remove validated-machine absolute paths without changing the preserved baseline; test a fresh checkout/dependency layout.
2. **Multi-queue completion:** implement real timeline/binary waits and queue-family ownership transfers in an independent harness. Never label a different producer submission as the same command stream.
3. **Semantic provider:** identify final-compatible color/depth/MV/camera/jitter for a real frame with evidence. Unknown bindings remain unavailable.
4. **Resource leases in Leonida:** pin actual resource/device/view owners, exact generations and completion tickets through all FG/presentation users.
5. **Shared present seam:** integrate only after previous gates, preserve tracker rollback, lock order, deferred frames and Xbox/DXGI frame-token policy.
6. **Image quality and pacing:** camera cuts, occlusion, rapid motion, resize/memory pressure, scanout and latency measurement; later HDR/SR combinations and broader compatibility.

See the detailed [integration plan](phase3/INTEGRATION_PLAN.md) for acceptance conditions. These items describe future work, not already implemented capabilities.
