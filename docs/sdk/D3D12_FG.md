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

The tested profile uses output-size color in the [configured format](#color-formats-and-hdr10) (RGBA8 by default), render-size R32_FLOAT depth and RG16_FLOAT motion in non-pixel shader read state, plus a distinct output-size UAV output in the same color format. It uses one direct command stream, a zero-jitter Y-up camera and one outstanding packet. Resizing requires completing outstanding work and creating a new backend at the new size. Only one FrameLab D3D12 FG backend may own the NGX D3D12 initialization in a process. While it exists, [D3D12SR](D3D12_SR.md#ngx-session-and-d3d12-fg) handles cannot be created, and FG creation returns `FRAMELAB_BUSY` while an SR handle exists. Serialize calls on it and do not manage the same NGX D3D12 session separately during its lifetime.

## Render extent

`FrameLabD3D12FgConfig.render_extent` sets the depth and motion-vector size. Color and the generated image use `extent`. The render extent is fixed when the backend is created, because NGX takes it at feature creation.

- Both dimensions zero select `extent`. One zero dimension, or a dimension larger than `extent`, returns `FRAMELAB_INVALID_DIMENSIONS` from create.
- Every frame must use the configured render and output extents; anything else returns `FRAMELAB_INVALID_DIMENSIONS`. The native color, depth, motion and output resources must also have those sizes; FrameLab reads their descriptions and returns `FRAMELAB_INVALID_RESOURCE_STATE` on a mismatch.
- To change either extent, retire outstanding work, destroy the backend (it returns `FRAMELAB_BUSY` while work is pending), create a new one and record its initialization. Its first frame must set a history reset; otherwise it returns `FRAMELAB_RESET_REQUIRED`.
- This is not dynamic resolution. Changing the render extent from frame to frame on one backend is not supported.

The field was appended without changing the ABI version. Backward compatibility covers old clients with this library: a client built against the v0.4.0 header passes that struct's size (104 bytes on x64), and this library accepts it without reading past it and uses `extent` as the render extent. It does not cover new clients with an older library: a v0.4.0 library rejects the new size with `FRAMELAB_ABI_MISMATCH`, so a render extent different from `extent` needs this library. [Accepted sizes](#accepted-config-sizes) lists every layout.

## Color formats and HDR10

`FrameLabD3D12FgConfig.color_format` and `flags` set the format of the color input and of the generated image. NGX takes the backbuffer format at feature creation, so it is fixed for the backend's lifetime, like the extents.

| `color_format` | `flags` | Color and generated image |
|---|---|---|
| `0` or `FRAMELAB_FORMAT_RGBA8_UNORM` | `0` | `DXGI_FORMAT_R8G8B8A8_UNORM`, SDR. Same as v0.4.0. |
| `FRAMELAB_FORMAT_BGRA8_UNORM` | `0` | `DXGI_FORMAT_B8G8R8A8_UNORM`, SDR. |
| `FRAMELAB_FORMAT_RGB10A2_UNORM` (9) | `0` | `DXGI_FORMAT_R10G10B10A2_UNORM`, SDR content in a 10-bit texture. |
| `FRAMELAB_FORMAT_RGB10A2_UNORM` (9) | `FRAMELAB_D3D12_FG_HDR10` | `DXGI_FORMAT_R10G10B10A2_UNORM`, HDR10. |

- `FRAMELAB_D3D12_FG_HDR10` declares that the color is HDR10: BT.2020 primaries, PQ-encoded as in BT.2100 / SMPTE ST 2084. FrameLab sets NGX's HDR color flag on every evaluation and passes the frames through as they are. It does no tone mapping or color conversion, and a 10-bit texture alone does not make content HDR10.
- `FRAMELAB_FORMAT_RGBA16_FLOAT` (scRGB/FP16) returns `FRAMELAB_UNSUPPORTED` from create. This FG profile has no FP16 path. D3D12 SR keeps its FP16 HDR support.
- These return `FRAMELAB_INVALID_ARGUMENT` from create: a `color_format` that is not one of the color formats above (the depth and motion formats, or an unknown value), an unknown `flags` bit, and `FRAMELAB_D3D12_FG_HDR10` with any format other than RGB10A2.
- Create queries the device's format support. The color format must be loadable and sampleable as a 2D texture and support typed UAV stores; otherwise create returns `FRAMELAB_UNSUPPORTED` before NGX is initialized. This matters most for BGRA8, whose typed UAV store support depends on the adapter and driver. A host that gets `FRAMELAB_UNSUPPORTED` can create the backend with RGBA8 instead and convert, or present without frame generation.
- Every frame's color image must declare the configured format; any other format, including FP16, returns `FRAMELAB_INVALID_IMAGE_DESCRIPTION` from record. The native color and generated resources must have the configured DXGI format, and the color, depth and motion resources must allow shader resource views; otherwise record returns `FRAMELAB_INVALID_RESOURCE_STATE`. Depth and motion formats are unchanged.

The fields were appended after `render_extent` without changing the ABI version. A client built against an older header gets RGBA8 SDR, and the bytes after its struct are not read. A new client that sets a format other than RGBA8 needs this library: older libraries reject its size.

### Accepted config sizes

| `struct_size` on x64 | Layout | Fields read |
|---|---|---|
| 104 | v0.4.0 | up to `report_user`; render extent = `extent`, RGBA8 SDR |
| 112 | with `render_extent` | up to `render_extent`; RGBA8 SDR |
| 120 or more | current | all fields |
| anything else | | `FRAMELAB_ABI_MISMATCH` |

Each older size is that layout's `sizeof`: its last field's end rounded up to the struct alignment.

## Hardware evidence

The [v0.4.0 regression comparison](RELEASE_NOTES.md#rtx-2050-regression-comparison) repeated the installed 720p FG readback and fence checks on the RTX 2050. Both revisions matched the historical readback hash and retired all three final leases. The separate ordinary-fallback verification passed with D3D12 debug-layer and GPU-based validation. The native FG readback run did not use those validation modes.

The [external sample](../../samples/d3d12_fg) was copied outside the SDK source tree and built against the installed package. The public header smoke test compiled and ran as C. The C++ executable used the public ABI for native NGX dispatch without NVIDIA headers or link inputs.

On the reference RTX 2050 Laptop (Windows 11 build 26200, driver 617.14, external SM86 0.4.2 / bundled NGX FG 310.9.16), six submissions finished and produced five verified interpolations. The test rejected invalid native state and stale depth before recording. It gated the final submission, dropped the caller's leases and resource interfaces, and verified that early retirement and destruction failed. All three final lease callbacks ran after the actual fence completed.

The last output matched the prior synthetic [D3D12 reference](../../experiments/d3d12/FRAME_INPUTS.md) byte for byte. SHA-256 `3251fb1b43c329882cfa8236c2bb2828882fd608a19372feff7423bcb2b79128`. The first attempt failed at NGX initialization with other logging sinks disabled and no callback. The successful repeat kept the normal NGX logging configuration.

This run covered image readback and fence lifetime. It skipped DXGI Present, D3D12 debug-layer validation, device-loss injection, cross-queue operation, stock-runtime compatibility and independent DLSS replication.

Render extents were checked on an RTX 5090 (Windows 11 build 26300, driver 621.24, DLSS SDK v310.9.1 runtime) with [`tests/run_d3d12_fg_extent_checks.ps1`](../../tests/run_d3d12_fg_extent_checks.ps1) at equal extents, 960×540, 640×360, 1280×540 and 960×720. Every case produced five verified interpolations, retired all three final leases and passed the descriptor, rejection and recreation checks. On the middle row the generated rectangle sat within 4 pixels of the halfway position at every extent. The equal-extent readback matched, byte for byte, the unmodified v0.4.0 sample run with both the v0.4.0 library and this one. Compared with it, the render-extent readbacks differed in at most 0.18 % of pixels, by at most 12 of 255 per channel. That run also skipped DXGI Present and D3D12 debug-layer validation.

Color formats were checked on the same RTX 5090, driver and runtime with the runner's RGBA8, BGRA8, RGB10A2 and HDR10 modes, each at equal extents, 960×540 and 640×360. All twelve cases produced five verified interpolations, retired all three final leases and passed the descriptor, color-configuration, rejection and recreation checks. The generated rectangle sat within 4 pixels of the halfway position (2 pixels for HDR10). The device reported the required format support for all three formats. The RGBA8 readbacks matched the earlier render-extent run byte for byte at each extent. The BGRA8 readbacks equalled the RGBA8 ones with red and blue swapped, with no other difference. In HDR10 mode the generated image kept the scene's PQ code values (for example 543/455/301 for the rectangle, about 125 cd/m² red in BT.2020), so no tone mapping was applied. The unmodified v0.4.0 sample (104-byte config) and the render-extent sample (112-byte config) also passed against this library, and the v0.4.0 readback was unchanged.

The same twelve cases were repeated with the runner's `-DebugLayer` option, which enables the D3D12 debug layer, GPU-based validation and synchronized command-queue validation before the device is created. Every case passed with no corruption, error or warning message and none discarded; the only message was the layer's notice that GPU-based validation was enabled. The readbacks were byte-identical to the runs without validation. This is the first native FG readback run under these validation modes. Messages from FrameLab and NGX share the device and are not told apart; with none reported, that did not matter here.

These runs skipped DXGI Present, an HDR display and other adapters.
