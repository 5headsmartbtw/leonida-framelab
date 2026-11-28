# FrameLab raw Rust bindings

The `framelab-sys` crate binds FrameLab C ABI v1: the portable core and, on Windows x86_64, the optional D3D12 SR and FG components. It needs Rust 1.77 or newer and has no dependencies. These are raw FFI functions: the crate does not install a driver, acquire an NVIDIA SDK or dispatch a GPU feature.

## Features

Every feature is additive, and any combination builds on every target.

| Feature | Default | Effect |
|---|---|---|
| `link` | yes | Links the shared FrameLab library at build time and declares its exports as `extern "C"` functions. |
| `runtime` | no | Adds `runtime::Library`, which loads FrameLab from an explicit path at run time. |
| `d3d12-sr` | no | Raw bindings for `<framelab/d3d12_sr.h>` (`framelab_sys::d3d12_sr`). Windows x86_64 only. |
| `d3d12-fg` | no | Raw bindings for `<framelab/d3d12_fg.h>` (`framelab_sys::d3d12_fg`). Windows x86_64 only. |

On other targets `d3d12-sr` and `d3d12-fg` compile to nothing, so a cross-platform host can enable them unconditionally. Both D3D12 features build together. The shipped v0.4.0 components still reject an SR handle while an FG handle exists in the process, and the reverse, until FL-16. The bindings keep that rule: they do not work around it.

Each export list produces the linked declaration, a function pointer type in the module's `pfn` submodule and the runtime table field. All three share one source, so they cannot disagree.

## Linked use (default)

Build a shared SDK as described in the [SDK guide](../../docs/sdk/README.md) with `BUILD_SHARED_LIBS=ON`. Set `FRAMELAB_LIB_DIR` to the installed library directory, or to the Windows import library directory. The library must also be found at run time. On Windows, add the install's `bin` directory to `PATH`. On Linux, use your loader configuration or `LD_LIBRARY_PATH`. With `d3d12-sr` or `d3d12-fg`, the linked D3D12 functions resolve only if the SDK was built with those components (`FRAMELAB_BUILD_D3D12_SR` and `FRAMELAB_BUILD_D3D12_FG`). Declarations you never call need no export.

## Runtime loading

```toml
framelab-sys = { path = "...", default-features = false, features = ["runtime", "d3d12-sr", "d3d12-fg"] }
```

A build without `link` neither links nor imports FrameLab, so the host starts when the library is absent. `Library::open(path)` then:

1. Rejects a relative path (`LoadError::InvalidPath`). No search path is used. On Windows, the library's own dependencies are searched in its directory and the system directories only.
2. Loads the module (`LoadError::Open` carries the OS error).
3. Calls `framelab_abi_version` before using any other export. A different version returns `LoadError::AbiMismatch`.
4. Resolves every core export. A missing export returns `LoadError::MissingExport`.

`Library::d3d12_sr()` and `Library::d3d12_fg()` resolve their component separately. A library built without a component returns `LoadError::ComponentAbsent`. A partially exported component returns `MissingExport`. Resolving the exports does not prove native support: `framelab_d3d12_*_create` still checks the driver and NGX runtime, and returns `FRAMELAB_UNSUPPORTED` on a GPU without support. Treat every one of these errors as "use the fallback path", not as a fatal error. [`examples/runtime_probe.rs`](examples/runtime_probe.rs) shows a host that reports availability and keeps running in every case.

The default shared library names are `FrameLab.dll` (MSVC), `libFrameLab.dll` (MinGW) and `libFrameLab.so`. `runtime::LIBRARY_FILE_NAME` holds the MSVC and Unix names. Ship and load the exact file you built.

**Module lifetime.** `Library` and every `Component` returned by it share one reference to the module. The module is unloaded when the last one is dropped. Keep one alive while any of these exist:

- a FrameLab object created through it (FG stream, lease, SR or FG handle)
- recorded work that has not been retired or cancelled
- a callback FrameLab may still call

## Safety rules

- **Raw pointers.** Every function is `unsafe`. Descriptors must be fully initialized, with `struct_size` and `abi_version` set as the C header requires. Pointers must stay valid for the duration of the call. Handles are used from one thread at a time.
- **Callbacks.** Lease release functions and the SR/FG report functions are `extern "C"`. They must not unwind or re-enter FrameLab. Catch panics inside them, for example with `std::panic::catch_unwind`. Report callbacks run synchronously on the calling thread.
- **COM ownership.** D3D12 interfaces are passed as raw pointers to opaque types (`d3d12::ID3D12Device` and others). The crate does not depend on a `windows` crate version. With windows-rs, pass `interface.as_raw().cast()`. FrameLab takes no COM reference. The host keeps the device, queue and fence alive until the handle is destroyed. It keeps every resource alive until the frame that names it is retired, or until its lease's final release for SR images.
- **Leases.** A `FrameLabLease` owns an allocation until its release callback runs. The callback can run after `framelab_lease_destroy`, at retirement. Keep the callback's `user` data alive until then.
- **Memory.** FrameLab allocates and frees its own handles. Never free a FrameLab pointer with a Rust allocator. Strings returned by `framelab_status_string` are static.
- **Fence retirement.** Call `mark_submitted` only after the recorded list has executed and `Signal(value)` is queued. `retire` reads the actual completion fence, and `FRAMELAB_NOT_READY` keeps every reference. Call `cancel` only when the recorded work can never execute.
- **Failure behaviour.** A rejected descriptor leaves the host command list usable. After `FRAMELAB_NATIVE_FAILURE` or `FRAMELAB_OUT_OF_MEMORY` from a record call, discard the list and cancel. The [D3D12 SR](../../docs/sdk/D3D12_SR.md) and [D3D12 FG](../../docs/sdk/D3D12_FG.md) guides define the full sequences.

## Tests

The tests need neither a GPU nor the D3D12 components. Build the shared SDK and [the external consumer project](../../tests/external-consumer), then set:

| Variable | Value | Used by |
|---|---|---|
| `FRAMELAB_ABI_LAYOUT` | output of `abi_layout` | `tests/abi.rs`: core layout |
| `FRAMELAB_D3D12_ABI_LAYOUT` | output of `d3d12_abi_layout` (Windows x64) | `tests/d3d12_abi.rs`: every D3D12 field offset, size, alignment and constant |
| `FRAMELAB_LIBRARY` | absolute path of the installed shared library | `tests/runtime.rs` |
| `FRAMELAB_LOADER_FIXTURES` | absolute path of the consumer build's `loader-fixtures` directory | `tests/runtime.rs` |
| `FRAMELAB_EXPECT_D3D12_COMPONENTS` | set (any value) when `FRAMELAB_LIBRARY` was built with both D3D12 components | `tests/runtime.rs`: expects them present instead of absent (CI builds without them) |

`d3d12_abi_layout` compiles the installed component headers when both components are installed, otherwise this tree's headers. It also checks every D3D12 function signature at compile time against a C mirror of the Rust `pfn` type. It references no FrameLab function. `loader_fixture.c` builds stand-in libraries: core only, wrong ABI, a missing core export, SR only, FG only, both, and a partial SR. The runtime tests use them to cover successful load, missing library, wrong ABI, missing exports, independent component availability and unloading.

Run each feature set with `cargo test --offline --manifest-path bindings/rust/Cargo.toml` plus:

- (none): core, linked
- `--features runtime`: linked and runtime-loaded in one process
- `--no-default-features --features runtime`: runtime only
- `--features d3d12-sr`, `--features d3d12-fg`, `--features d3d12-sr,d3d12-fg`, `--all-features`, and `--no-default-features --features runtime,d3d12-sr` (or `d3d12-fg`): Windows x64

CI runs this matrix on Windows and Linux. It also runs the runtime-only example while FrameLab is not on the loader path.

**Not covered here:**

- The D3D12 tests on other targets: the features compile to nothing there.
- Native evaluation, readback, fence completion and lease retention on a GPU. The separately built consumers in [`samples/rust_d3d12`](../../samples/rust_d3d12) cover them for SR and FG; they need an NVIDIA GPU and the DLSS runtime, so CI does not run them.
