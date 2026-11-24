# Leonida Vulkan pipeline audit

[Русский и ссылки на строки исходников](LEONIDA_AUDIT.md)

This read-only source audit applies to Leonida revision `8575d36edc479218972497a7bded8651a42856c0`. It used source code rather than game images, captures or resource payloads. The [source manifest](AUDIT_SOURCE_MANIFEST.json) records the files and hashes.

| Input or responsibility | Existing code | Missing evidence or implementation for FG |
| --- | --- | --- |
| Color | `VulkanPresenter::present` resolves the supplied color resource. `present_planes` handles up to two planes, rectangles, HDR and composition. | Mapping from the presented color to matching depth, vectors and camera. A plane index does not establish its HUD semantics. |
| Depth | Depth/stencil descriptors, native image format/aspect metadata, recorded attachments. | Which depth image belongs to the relevant color frame and remains available when needed. |
| Motion vectors | General RG textures, render targets, descriptor binding and shader replay. | Explicit velocity binding, units, sign, jitter policy and camera-motion inclusion. |
| Camera | Recorded root constants and CBV/descriptor-table addresses. | Verified matrices, jitter, depth direction and camera planes/FOV for the presented color. The camera-view enum controls view mode. |
| Layout | Per-resource/subresource tracking, D3D12-to-Vulkan state mapping, transitions and rollback after failed submission. | FG needs to participate in that same tracking and rollback protocol. |
| Lifetime | Real COM resource owner, cached views, deferred deletion and timeline-based retirement. | `ResourceBinding` carries metadata. FG needs an owned lease covering the exact generation, views, device, history and all presentation consumers. |
| Synchronization | Multiple queues, submit lock, progress timelines, held batches, presenter semaphores and frame fences. | An exported completion chain for the specific inputs, waits in the consumer, and ownership transfers whenever queue families differ. |
| Present and pacing | WindowPresenter routes into VulkanPresenter. XboxPresentation has plain and plane presents, suspension and frame tokens. | A shared frame packet and policy for real/generated presentation, history resets and tokens. |

The audited device setup can use dedicated compute and copy families. General producer work cannot be assumed to run in the present queue. The likely integration seam is `VulkanPresenter::submit_now` before blit or composition, after resource resolution. Preserve resource → tracker → submit lock ordering and existing rollback. A producer wait must not hold a lock needed to signal completion. A deferred present must bind at actual submission or explicitly pin the exact resource generation and matching semantic inputs.

FG in Leonida remains gated on a verified semantic provider and completion/lifetime support. The synthetic sample proves its own producer inputs only.
