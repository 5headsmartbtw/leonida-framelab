# FrameLab raw Rust bindings

The `framelab-sys` crate binds FrameLab C ABI v1. It needs Rust 1.77 or newer and a shared SDK build installed for the same target. Follow the [SDK guide](../../docs/sdk/README.md) and select `BUILD_SHARED_LIBS=ON`. Set `FRAMELAB_LIB_DIR` to the SDK's installed library or Windows import library directory. Make the shared library discoverable at runtime. On Windows, add the install's `bin` directory to `PATH`. On Linux, use your loader configuration or add the installed library directory to `LD_LIBRARY_PATH`.

Use the local crate as a Cargo path dependency. These are raw FFI functions. Serialize stream access, prevent callbacks from unwinding, and retire only when actual GPU completion is established. The crate does not install a driver, acquire an NVIDIA SDK, or dispatch a GPU feature.

Run the installed SDK ABI checks with `cargo test --manifest-path bindings/rust/Cargo.toml`. Set `FRAMELAB_ABI_LAYOUT` to the output of the `abi_layout` program built from [the external consumer project](../../tests/external-consumer). CI runs both the native layout comparison and a synthetic lease lifecycle.
