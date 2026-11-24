# Leonida FG integration plan

[Русский](INTEGRATION_PLAN.md)

The Phase 3 audit and standalone tests prepared this sequence. Integration work begins after the synthetic handoff and source semantics are demonstrated.

1. **Prove external completion in a standalone Vulkan harness.** Test a producer and consumer on distinct submissions, real timeline waits and binary semaphore lifetime, and queue-family release/acquire barriers for exclusive resources. Cover failed submissions, repeated resize, cancelled work, resource reuse and retirement before/after GPU completion. Record the queue configurations that actually ran. [Task #2](https://github.com/5headsmartbtw/leonida-framelab/issues/2)
2. **Verify the semantic producer.** For a single source frame, establish matching final color, depth, vectors and scale, unjittered matrices, jitter, near/far/FOV and depth direction. Use controlled static, motion and cut cases. Keep inputs unavailable wherever evidence is missing. [Task #3](https://github.com/5headsmartbtw/leonida-framelab/issues/3)
3. **Bind actual owners and epochs.** Pin allocations, views, device and exact resource generation through FG, presentation and history. Retire with real fence/timeline tickets. Test deferred presents and recycled resource IDs.
4. **Attach at the shared Present boundary.** Audit `VulkanPresenter::submit_now` after resource resolution. Preserve layout rollback and resource → tracker → submit ordering. Keep real Present usable after failed generation. Blank, minimized, stale and surface-lost frames should bypass FG and invalidate history.
5. **Reconcile Xbox/DXGI present and pacing.** Respect source/destination rectangles, composition, HDR/SDR, suspension and the source frame token. Separately measure render cadence, API present counts, display cadence and latency where available.
6. **Start with a narrow validated profile.** Native SDR, 720p, 2X, single sample and one in-flight frame are the starting limits. Test allocation budgets, resize, release/reset, device loss and failure handling before widening scope.

## SR + FG on the synthetic scene

Verify SR alone, FG alone and their combination on the same input sequence. Choose whether FG consumes the post-SR color, and specify how depth and motion vectors map from render resolution to output resolution. Test jitter and reprojection conventions, real versus generated presentation, and resets on cuts, resize and SR mode changes. Require readback and validation on the combined path. The current direct FG adapter accepts equal render/output extents, so a combined path may need a new binding profile.

## Acceptance boundary

Backend integration requires a verified producer, real resource leases, confirmed native state and dimensions, actual completion supported by the consumer, generated image verification and presentation that preserves lifetimes and frame tokens. The original Streamline capability result remains independent of the Direct NGX result. See the [contract](CONTRACT.en.md) and [audit](LEONIDA_AUDIT.en.md).
