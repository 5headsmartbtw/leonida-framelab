// SPDX-License-Identifier: MIT
//! A host that starts with or without FrameLab. Pass the absolute path of the shared library:
//! `cargo run --example runtime_probe --no-default-features --features runtime -- <path>`.
//! It reports what is available and exits 0 in every case, as a renderer that falls back would.

use framelab_sys::runtime::{Library, LoadError};
use std::ffi::CStr;

fn main() {
    let Some(path) = std::env::args_os().nth(1) else {
        println!("framelab: no library path given; rendering without FrameLab");
        return;
    };
    // SAFETY: the path names a FrameLab build chosen by the user of this example.
    let library = match unsafe { Library::open(&path) } {
        Ok(library) => library,
        Err(error) => {
            println!("framelab: unavailable ({error}); rendering without FrameLab");
            return;
        }
    };
    let core = library.core();
    // SAFETY: both core functions have no preconditions; the status string is static.
    let (version, ok) = unsafe { ((core.framelab_abi_version)(), CStr::from_ptr((core.framelab_status_string)(0))) };
    println!("framelab: core ABI {version} loaded ({})", ok.to_string_lossy());
    report_components(&library);
}

#[allow(unused_variables)]
fn report_components(library: &Library) {
    #[cfg(all(windows, target_arch = "x86_64", feature = "d3d12-sr"))]
    report("d3d12-sr", library.d3d12_sr().map(drop));
    #[cfg(all(windows, target_arch = "x86_64", feature = "d3d12-fg"))]
    report("d3d12-fg", library.d3d12_fg().map(drop));
}

#[allow(dead_code)]
fn report(name: &str, result: Result<(), LoadError>) {
    match result {
        Ok(()) => println!("framelab: {name} exports resolved; native support is checked at create"),
        Err(error) => println!("framelab: {name} unavailable ({error}); using the fallback path"),
    }
}
