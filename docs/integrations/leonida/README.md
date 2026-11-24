# Leonida as a FrameLab consumer

This integration design uses the [read-only Vulkan audit](../../phase3/LEONIDA_AUDIT.en.md) of Leonida `8575d36edc479218972497a7bded8651a42856c0`. That audit found the Present color and generic depth resources, views, queue timelines, layout tracking and deferred deletion. It did not verify which depth, motion or camera data corresponds to a particular presented frame.

Use a small host adapter that packages verified inputs and clones the real resource/view owner before invoking FrameLab. Put FrameLab calls at the candidate `VulkanPresenter::submit_now` seam after resolving the live bindings, before the compositor blit. Pass the real producer queue family and completion ticket. The same-command-stream path is valid only when the existing producer and FrameLab record into that stream. External timelines or transfers need the standalone Vulkan handoff tests in [#2](https://github.com/5headsmartbtw/leonida-framelab/issues/2).

The adapter must maintain the existing resource → tracker → submit lock order and rollback. Resolve queued work on actual submission or pin the exact allocation and generation in advance. Drop an SDK lease only after the last user retires, including generated and real presentation. Unknown inputs and failed native recording should return to the host's real Present and reset temporal history. Additional presentation must preserve the host's simulation frame and Xbox/DXGI token policy.

Progress this proposal against [verified semantic inputs and ownership](https://github.com/5headsmartbtw/leonida-framelab/issues/3) and the [integration plan](../../phase3/INTEGRATION_PLAN.en.md). No external repository changes are part of this audit.
