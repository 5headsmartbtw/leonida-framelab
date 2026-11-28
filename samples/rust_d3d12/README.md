# Native Rust D3D12 consumers

Two Rust programs drive the installed FrameLab D3D12 components on a GPU through [`framelab-sys`](../../bindings/rust): `framelab-rust-sr` (DLSS Super Resolution) and `framelab-rust-fg` (DLSS Frame Generation). Each uses one handle in its own process, because the shipped components do not allow an SR and an FG handle at the same time. Both load `FrameLab.dll` at run time from an absolute path (`framelab-sys` features `runtime`, `d3d12-sr`, `d3d12-fg`, without `link`), so neither executable imports FrameLab. The D3D12 host code uses [windows-rs](https://github.com/microsoft/windows-rs) 0.62.2 and is adapted from the maintainer's standalone Rust SR prototype shared in #43.

This is a test consumer, not a safe Rust API: every FrameLab call is a raw FFI call with the rules of the [SR](../../docs/sdk/D3D12_SR.md) and [FG](../../docs/sdk/D3D12_FG.md) guides.

## Build

1. Build and install a shared SDK with both components, as in the [D3D12 FG guide](../../docs/sdk/D3D12_FG.md): `-DBUILD_SHARED_LIBS=ON -DFRAMELAB_BUILD_D3D12_SR=ON -DFRAMELAB_BUILD_D3D12_FG=ON -DDLSS_NGX_SDK_ROOT=<DLSS SDK>`.
2. Build this project separately. It needs Rust 1.82 (windows-rs) and the crates in `Cargo.lock`, from crates.io or a local registry cache. `framelab-sys` keeps its own MSRV of 1.77 and no dependencies.

```powershell
cargo build --release --manifest-path samples/rust_d3d12/Cargo.toml
```

No `FRAMELAB_LIB_DIR` is needed: nothing links FrameLab.

## Run

```powershell
& .\samples\rust_d3d12\target\release\framelab-rust-sr.exe $sdk\bin\FrameLab.dll $runtimeDirectory $evidenceDirectory
& .\samples\rust_d3d12\target\release\framelab-rust-fg.exe $sdk\bin\FrameLab.dll $runtimeDirectory $evidenceDirectory --debug-layer
```

All paths are absolute. `$runtimeDirectory` holds the separately acquired NGX runtime. `--debug-layer` enables the D3D12 debug layer, GPU-based validation and synchronized queue validation before the device is created (this needs the Windows Graphics Tools feature), writes every message to `d3d12-debug.log` and fails on any corruption, error or warning message.

Exit code 0 means every check passed and 1 that one failed. Exit code 3 means FrameLab could not be used and the program rendered without it: the library is missing or has another ABI (`Library::open` failed), the component is absent from the build, or create returned `FRAMELAB_UNSUPPORTED`. The program then prints `FRAMELAB_UNAVAILABLE` with the reason, uploads and reads back one ordinary frame, and checks it.

[`tests/run_rust_d3d12_checks.ps1`](../../tests/run_rust_d3d12_checks.ps1) runs both programs with and without the debug layer, with a missing library and, with `-NoComponentLibrary <FrameLab.dll built without the D3D12 components>`, with a library lacking the components. It writes `rust-d3d12-results.json` with the FG readback hash:

```powershell
& .\tests\run_rust_d3d12_checks.ps1 -TargetDirectory .\samples\rust_d3d12\target\release -Library $sdk\bin\FrameLab.dll `
  -RuntimeDirectory $runtimeDirectory -NoComponentLibrary $plainSdk\bin\FrameLab.dll
```

## What each program checks

`framelab-rust-sr` (scene and checks from the prototype):

- Records feature configuration at 1280×720 (Quality) and retires it on the real fence.
- Evaluates frames 0, 1 and 3 and reads back each output. The upscaled rectangle must cover the expected area and sit within 3.5 % of its position. The output must contain no texel of the clear value.
- Frame 2 has no depth. Record returns `FRAMELAB_INVALID_IMAGE_SEMANTICS`, and the same list renders the frame without SR. The readback must equal the ordinary image.
- Frame 4 is recorded, the list discarded and the packet cancelled. All four leases are released, and the fence does not advance.
- Reconfigures at 960×540 and evaluates frame 5 behind a queue gate, after the host dropped all its leases and textures. Early retirement returns `FRAMELAB_NOT_READY` while all four resources stay alive. After the real fence completes, retirement releases all four.

`framelab-rust-fg` (scene, metadata and checks of the C++ [`samples/d3d12_fg`](../d3d12_fg) client):

- Records NGX feature creation and retires it, then six 1280×720 frames of a rectangle moving 32 pixels per frame.
- Frame 0 checks that a wrong native state returns `FRAMELAB_INVALID_RESOURCE_STATE`. Frame 2 checks that stale depth returns `FRAMELAB_INVALID_SEQUENCE`. Each valid frame is then recorded on the same list.
- The last submission is gated, with every host lease and texture reference dropped. Early retirement returns `FRAMELAB_NOT_READY` with all three leased inputs alive and none released. Destroy returns `FRAMELAB_BUSY`. After the fence completes, retirement releases all three.
- Every generated frame must be fully written and differ from both real frames. Its rectangle, measured on the middle row, must be closer to the halfway position than to either real frame. The sixth readback is written to `generated-5.rgba`.

## Verified

On an RTX 5090 (Windows 11 build 26300, driver 621.24 / 32.0.16.2124), with an SDK built from this tree with both components and the DLSS SDK v310.9.1 runtime (`nvngx_dlssg.dll` 310.9.1.0):

- **SR:** every check passed. The optimal render extents were 853×480 for 1280×720 and 640×360 for 960×540. The rectangle centre was within 0.001 of its expected position in all four readbacks.
- **FG:** every check passed, with 5/5 interpolations and 3/3 lease releases. The largest edge distance from the halfway position was 4 pixels. The sixth readback was byte-identical to the C++ client's run against the same library: SHA-256 `8f97bacec2deed0d213711cbfea43336c89876d74df14cd9b7a3c625b83fbd49`.
- **Debug layer:** both programs passed with no corruption, error or warning message. The only message was the layer's notice that GPU-based validation is enabled.
- **Fallback:** a missing library (Windows error 126) and a FrameLab build without D3D12 components (`ComponentAbsent`) each led to a verified ordinary frame and exit code 3.

Not covered: the RTX 2050 reference checks, other adapters and independent hardware validation; combined SR+FG; DXGI presentation; physical display timing; renderer integration.
