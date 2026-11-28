# D3D12 Frame Generation in FrameLab

The optional D3D12FG component exposes Direct NGX FG through a Windows x64 C ABI. An external project can build against the public Windows SDK header and the installed FrameLab package. The native DLSS SDK is needed when building FrameLab. At runtime, the host supplies the separately acquired NGX runtime and all GPU resources.

## Build and link

```powershell
cmake -S . -B build-sdk -G 'Visual Studio 17 2022' -A x64 `
  -DBUILD_SHARED_LIBS=ON -DFRAMELAB_BUILD_D3D12_FG=ON `
  '-DDLSS_NGX_SDK_ROOT=D:/SDKs/DLSS'
cmake --build build-sdk --config Release
cmake --install build-sdk --config Release --prefix D:/FrameLab

cmake -S samples/d3d12_fg -B build-client -G 'Visual Studio 17 2022' -A x64 `
  '-DCMAKE_PREFIX_PATH=D:/FrameLab'
cmake --build build-client --config Release
ctest --test-dir build-client -C Release --output-on-failure
```

The client uses `find_package(FrameLab 0.4.0 EXACT CONFIG REQUIRED COMPONENTS D3D12FG)` and links `FrameLab::FrameLab`. Include `<framelab/d3d12_fg.h>`. The default portable static/shared packages expose the [core ABI](README.md). The GPU component requires the opt-in shared Windows package.

## Record and retire

1. Fill `FrameLabD3D12FgConfig` including its size and ABI version. Pass the real device, direct command queue and its completion fence. Use an application-owned project ID and directories. The optional initialization callback reports unmodified NGX result codes synchronously. It must not unwind or re-enter FrameLab. FrameLab checks actual NGX availability before it permits feature creation.
2. Record initialization into an open direct list. Execute it on the configured queue, enqueue a fence signal on that queue, and pass the value to `framelab_d3d12_fg_mark_submitted`. Retire after FrameLab observes completion. Reserve `UINT64_MAX` for device removal. Keep fence values monotonically increasing across the entire host queue timeline.
3. For each frame, pass `FrameLabFgFrame` and matching `FrameLabD3D12FgBindings`. The three input descriptors carry real owners and source-frame metadata. The producer tracks and establishes the native states. FrameLab checks COM device identity, formats, extents, resource IDs, command stream, tracked states and input ownership before calling Evaluate. The caller supplies a distinct output texture.
4. Execute and signal the queue, then acknowledge the value. The backend keeps native resource interfaces, the command list and frame leases until the real fence completes. `FRAMELAB_NOT_READY` and `FRAMELAB_BUSY` preserve ownership. The host keeps the allocator, uploads and presentation resources alive as long as its work uses them. FrameLab does not submit commands, signal completion or present.
5. After a metadata or state rejection, the host can continue ordinary rendering. If NGX recording returns `FRAMELAB_NATIVE_FAILURE`, discard the command list and cancel the packet. Only cancel after making sure that work cannot be submitted later. Invalidate history on dropped or discontinuous frames and set the reset flag on recovery. Destroy the idle backend before reclaiming its device.

The tested profile uses output-size RGBA8 color, render-size R32_FLOAT depth and RG16_FLOAT motion in non-pixel shader read state, plus a distinct output-size RGBA8 UAV output. It uses one direct command stream, a zero-jitter Y-up camera and one outstanding packet. Resizing requires completing outstanding work and creating a new backend at the new size. Only one FrameLab D3D12 FG backend may own the NGX D3D12 initialization in a process. While it exists, [D3D12SR](D3D12_SR.md#ngx-session-and-d3d12-fg) handles cannot be created, and FG creation returns `FRAMELAB_BUSY` while an SR handle exists. Serialize calls on it and do not manage the same NGX D3D12 session separately during its lifetime.

## Render extent

`FrameLabD3D12FgConfig.render_extent` sets the depth and motion-vector size. Color and the generated image use `extent`. The render extent is fixed when the backend is created, because NGX takes it at feature creation.

NGX receives the full render-sized depth and motion-vector subrects. Motion vectors use previous-minus-current UV units in this profile. The backend forwards the supplied `motion_vector_scale` to NGX directly. The synthetic client uses a scale of `{1, 1}` at every render extent.

- Both dimensions zero select `extent`. One zero dimension, or a dimension larger than `extent`, returns `FRAMELAB_INVALID_DIMENSIONS` from create.
- Every frame must use the configured render and output extents; anything else returns `FRAMELAB_INVALID_DIMENSIONS`. The native color, depth, motion and output resources must also have those sizes; FrameLab reads their descriptions and returns `FRAMELAB_INVALID_RESOURCE_STATE` on a mismatch.
- To change either extent, retire outstanding work, destroy the backend (it returns `FRAMELAB_BUSY` while work is pending), create a new one and record its initialization. Its first frame must set a history reset; otherwise it returns `FRAMELAB_RESET_REQUIRED`.
- This is not dynamic resolution. Changing the render extent from frame to frame on one backend is not supported.

The field was appended without changing the ABI version. A client built against the v0.4.0 header passes that struct's size (104 bytes on x64). This library reads the original fields and uses `extent` as the render extent. Any `struct_size` between that size and `sizeof(FrameLabD3D12FgConfig)` returns `FRAMELAB_ABI_MISMATCH`. A v0.4.0 library reads the original fields even when given a larger descriptor and uses the output extent for all inputs. A different render extent requires this library.

## DXGI presentation

FrameLab returns the interpolated image in the caller's output texture. It does not create a swapchain, select a backbuffer, schedule frames or call Present. The [external sample](../../samples/d3d12_fg) shows one application-owned arrangement:

- **Architecture.** The application presents Direct NGX output through its own flip-model swapchain (`DXGI_SWAP_EFFECT_FLIP_DISCARD`, two buffers). This is different from Streamline DLSS-G, whose proxy swapchain inserts and schedules generated frames itself inside Present ([Streamline DLSS-G guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md)). Do not combine the two for the same swapchain, or the same interval is generated twice. FrameLab's D3D12 FG path is Direct NGX only.
- **Order.** Source frame N is rendered and evaluated against N-1. The application presents the generated N-1/N image, then real frame N. Each Present copies the chosen texture into the current backbuffer on the same direct queue, transitions it to `PRESENT`, and releases the backbuffer reference before Present and before any `ResizeBuffers`.
- **Pacing and latency.** The midpoint becomes available after frame N has rendered. The sample then delays real frame N by the preceding CPU-side real-to-generated Present interval, capped at 250 ms. This approximates even submission spacing. `--no-pacing` disables that delay. Sync interval 1 requests display synchronization. At sync interval 0 the sample sets `DXGI_PRESENT_ALLOW_TEARING` when the feature query reports support. Queue waits and readback affect sample pacing. Display-event timing and measured end-to-end latency require separate evidence. [DXGI Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)
- **UI.** Interpolate the scene without UI and compose the UI onto both the generated and the real image before Present. The synthetic scene has no UI plane.
- **Lifetime.** The host keeps the allocator, upload and readback buffers, output texture and backbuffer alive until the fence of the work that used them completes. Before resizing, the host drains the queue (including the last Present), destroys the idle FrameLab backend, resizes the swapchain and creates a new backend at the new extent with a history reset. Drain the queue again before releasing the swapchain at shutdown: on the RTX 5090 with the D3D12 debug layer, releasing it with the final Present still queued terminated the process (exit code 2173) after all checks had passed.
- **Fallback.** When FG initialization or an optional FG allocation fails, the sample presents the ordinary source. On recording failure it discards the list, cancels a natively failed packet, re-uploads the real frame and presents it. The next accepted frame carries a history reset. After `framelab_d3d12_fg_invalidate`, a frame without a reset flag is rejected with `FRAMELAB_RESET_REQUIRED`.

## Hardware evidence

The [v0.4.0 regression comparison](RELEASE_NOTES.md#rtx-2050-regression-comparison) repeated the installed 720p FG readback and fence checks on the RTX 2050. Both revisions matched the historical readback hash and retired all three final leases. The separate ordinary-fallback verification passed with D3D12 debug-layer and GPU-based validation. The native FG readback run did not use those validation modes.

The [external sample](../../samples/d3d12_fg) was copied outside the SDK source tree and built against the installed package. The public header smoke test compiled and ran as C. The C++ executable used the public ABI for native NGX dispatch without NVIDIA headers or link inputs.

On the reference RTX 2050 Laptop (Windows 11 build 26200, driver 617.14, external SM86 0.4.2 / bundled NGX FG 310.9.16), six submissions finished and produced five verified interpolations. The test rejected invalid native state and stale depth before recording. It gated the final submission, dropped the caller's leases and resource interfaces, and verified that early retirement and destruction failed. All three final lease callbacks ran after the actual fence completed.

The last output matched the prior synthetic [D3D12 reference](../../experiments/d3d12/FRAME_INPUTS.md) byte for byte. SHA-256 `3251fb1b43c329882cfa8236c2bb2828882fd608a19372feff7423bcb2b79128`. The first attempt failed at NGX initialization with other logging sinks disabled and no callback. The successful repeat kept the normal NGX logging configuration.

This run covered image readback and fence lifetime. It skipped DXGI Present, D3D12 debug-layer validation, device-loss injection, cross-queue operation, stock-runtime compatibility and independent DLSS replication.

Render extents were checked on an RTX 5090 (Windows 11 build 26300, driver 621.24, DLSS SDK v310.9.1 runtime) with [`tests/run_d3d12_fg_extent_checks.ps1`](../../tests/run_d3d12_fg_extent_checks.ps1) at equal extents, 960×540, 640×360, 1280×540 and 960×720. Every case produced five verified interpolations, retired all three final leases and passed the descriptor, rejection and recreation checks. On the middle row the generated rectangle sat within 4 pixels of the halfway position at every extent. The equal-extent readback matched, byte for byte, the unmodified v0.4.0 sample run with both the v0.4.0 library and this one. Compared with it, the render-extent readbacks differed in at most 0.18 % of pixels, by at most 12 of 255 per channel. That run also skipped DXGI Present and D3D12 debug-layer validation.

DXGI presentation was measured on an RTX 5090: Windows 11 build 26300, driver 621.24, DLSS SDK v310.9.1 runtime, 3840×2160 display at 240 Hz. The run used `tests/run_d3d12_fg_present_checks.ps1 -PresentMon` with PresentMon 2.x ETW, not elevated, and all ten cases passed.

- **Present mode.** The client's windowed 1280×720 flip-discard swapchain started in `Composed: Flip`. After about 100 presents (2.6–3.3 s), DWM promoted it to `Hardware Composed: Independent Flip`. In each 1800-frame run, with VSync on or off, that mode carried 3426–3427 of 3527 presents. The short FG cases (10–15 presents) stayed composed. The five-present no-FG case reached independent flip for its last two presents.
- **Tearing.** With sync interval 0, PresentMon reported `AllowsTearing` only after the promotion.
- **Dropped presents.** All dropped presents fell in the composed phase: 2–3 of 3527 per 1800-frame run.
- **Intervals.** Display-change intervals followed the Present intervals:
  - With VSync: p50 29.2 ms, p95 37.5 ms. Present-to-display time p50 was 2.9 ms.
  - Without VSync: p50 27.7 ms, p95 37.7 ms. Present-to-display time p50 was 0.8 ms.

  These intervals reflect the sample's per-Present GPU waits and readback, not production pacing. Physical scanout and end-to-end latency were not measured.
