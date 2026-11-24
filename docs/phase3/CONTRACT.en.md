# FrameInputs contract

[Русский](CONTRACT.md)

The public [FrameInputs header](../../fg/FrameInputs.hpp) is the C++20 contract. It uses opaque IDs and standard library types. The [Vulkan adapter](../../fg/FrameInputsVulkan.hpp) provides the corresponding native handles and state.

| Field | Meaning |
| --- | --- |
| Identity | Stream, device and generation IDs assigned by the producer. `frameIndex` counts source frames. |
| Images | Color, depth, motion vectors and optional UI/HUD-less inputs. Each image carries an ID, format, extent, frame index, state, queue owner, provenance and allocation lease. |
| Dimensions | Color matches `outputExtent`. Depth and motion vectors match `renderExtent`. The current Vulkan consumer requires equal extents. |
| Camera | Five row-major, unjittered matrices, including projection and inverse and current-to-previous clip and inverse. Explicit camera basis and position, near/far, FOV and aspect, jitter in pixels, motion-vector scale, projection conventions and depth direction. |
| History | Positive finite frame delta, reset reason, resize and device-lost flags. Reasons include initial frame, resize, camera cut, reactivation and discontinuity. |
| Ownership | A shared allocation lease holds the native allocation, views and any necessary allocator. Raw handles and IDs carry identity. |
| Completion | Same command stream, host completion, binary signal or timeline signal. The structure carries queue/stream/signal identity, timeline value and owned synchronization resources where applicable. |

The v1 semantics are scene-linear RGB, post-projection depth in `[0,1]` with explicit direction, and previous-minus-current motion in UV with explicit scale. The producer handles conversions for different depth conventions, vector signs or color spaces. An image format by itself cannot establish meaning.

`validate` checks structural consistency, including dimensions, format, source-frame identity, state, ownership, camera finiteness, leases and the completion description. `Sequence::check` additionally checks frame/generation continuity, first or skipped frame resets, and changes in resource identity or lease owner. The sequence's snapshot holds weak owner identities. `accept` runs after successful recording, and invalid input does not advance the sequence.

## Supported Vulkan profile

The producer and consumer share one device, queue and family, command buffer and command stream token. The accepted formats and states are RGBA16F color in shader-read layout, D24S8 depth in depth/stencil attachment layout with a depth view, and RG16F motion vectors in color attachment layout. The views cover full 2D single-sample inputs. The sample uses Vulkan Y-down projection and D3D-oriented Y-up reprojection. HDR, MSAA, arrays, arbitrary subresources, UI/HUD-less inputs, SR scaling and other camera convention pairs require separate support and evidence.

Before recording image barriers, the adapter checks native identity, format, extent, layout, aspect, aliasing and queue ownership against the supplied bindings. It rejects external host completion, binary and timeline waits, or different queue families. A producer in a distinct submission must preserve its real completion mode.

## Sample handoff and recovery

The sample packages its own GBuffer and camera constants after rasterization. `SampleFrameOwner` keeps the GBuffer and allocator alive. Direct NGX retains the complete packet after successful recording and releases it after `vkQueueWaitIdle` and readback. A separate GBuffer owner remains alive through real presentation. This is a one-frame-in-flight design.

Invalid input falls back to a real Present before NGX calls or new input barriers, invalidates the sequence and resets history on recovery. The consumer rejects new recording while an earlier packet is in flight. NGX failures retain the existing FG disable path.
