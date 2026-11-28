fn main() {
    println!("cargo:rerun-if-env-changed=FRAMELAB_LIB_DIR");
    // Runtime-only builds (`--no-default-features --features runtime`) neither link nor import FrameLab.
    if std::env::var_os("CARGO_FEATURE_LINK").is_none() {
        return;
    }
    if let Some(dir) = std::env::var_os("FRAMELAB_LIB_DIR") {
        println!("cargo:rustc-link-search=native={}", std::path::Path::new(&dir).display());
    }
    println!("cargo:rustc-link-lib=dylib=FrameLab");
}
