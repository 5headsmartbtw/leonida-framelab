# Architecture

```mermaid
flowchart LR
    A[Known sample renderer] --> B[FrameInputs: IDs, semantics, camera, time, owners]
    B --> C{Validate contract and native bindings}
    C -->|valid tested Vulkan profile| D[Direct NVIDIA NGX Evaluate]
    C -->|missing or unsupported| E[Normal real-frame Present]
    D --> F[GPU completion and lease retirement]
    F --> G[Generated plus real Present]
    H[Future Leonida semantic provider] -. not implemented .-> B
```

The portable contract does not resolve GPU handles. Vulkan binding validation is separate and currently supports ordered producer/consumer recording in the same command buffer and queue. External signal descriptions exist in the schema but the current GPU consumer rejects them until wait/ownership support is implemented and tested.

Image leases retain actual GBuffer allocations and their allocator. Resource generation, source-frame continuity and reset history prevent stale temporal reuse. A rejected input does not advance the accepted sequence and must not record NGX work. The next valid frame resets history.

The sample's SR route uses Streamline separately. Combined SR + direct NGX FG is not validated. The normal presenter is a diagnostic reference, not the official Streamline FG pacer. Physical scanout and input latency require separate measurement.
