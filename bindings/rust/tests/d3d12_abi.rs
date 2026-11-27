// SPDX-License-Identifier: MIT
// Compares the Rust D3D12 SR/FG bindings with the native probe tests/external-consumer/d3d12_abi_layout.c.
// Set FRAMELAB_D3D12_ABI_LAYOUT to its output. Only the enabled components are compared.
#![cfg(all(windows, target_arch = "x86_64", any(feature = "d3d12-sr", feature = "d3d12-fg")))]

use std::collections::BTreeMap;
#[allow(unused_imports)]
use std::mem::{align_of, offset_of, size_of};

macro_rules! layout {
    ($map:ident, $t:ident { $($f:ident),+ $(,)? }) => {
        $map.insert(concat!(stringify!($t), ".size").to_owned(), size_of::<$t>() as u64);
        $map.insert(concat!(stringify!($t), ".align").to_owned(), align_of::<$t>() as u64);
        $( $map.insert(concat!(stringify!($t), ".", stringify!($f)).to_owned(), offset_of!($t, $f) as u64); )+
    };
}

#[allow(unused_macros)]
macro_rules! constants {
    ($map:ident, $($c:ident),+ $(,)?) => { $( $map.insert(stringify!($c).to_owned(), u64::from($c)); )+ };
}

fn enabled(key: &str) -> bool {
    let sr = key.starts_with("FrameLabD3D12Sr") || key.starts_with("FRAMELAB_SR_") || key.starts_with("FRAMELAB_D3D12_SR_");
    let fg = key.starts_with("FrameLabD3D12Fg");
    key.starts_with("wchar_t.") || (sr && cfg!(feature = "d3d12-sr")) || (fg && cfg!(feature = "d3d12-fg"))
}

#[test]
fn d3d12_layout_matches_compiled_c_headers() {
    let native = std::env::var("FRAMELAB_D3D12_ABI_LAYOUT").expect("run the d3d12_abi_layout probe first");
    let native: BTreeMap<String, u64> = native
        .split_whitespace()
        .map(|pair| {
            let (key, value) = pair.split_once('=').expect("key=value");
            (key.to_owned(), value.parse().expect("integer value"))
        })
        .filter(|(key, _)| enabled(key))
        .collect();

    let mut rust = BTreeMap::new();
    rust.insert("wchar_t.size".to_owned(), size_of::<framelab_sys::d3d12::WChar>() as u64);
    rust.insert("wchar_t.align".to_owned(), align_of::<framelab_sys::d3d12::WChar>() as u64);
    #[cfg(feature = "d3d12-sr")]
    {
        use framelab_sys::d3d12_sr::*;
        layout!(rust, FrameLabD3D12SrConfig { struct_size, abi_version, device, queue, completion_fence,
            project_id, engine_version, ngx_runtime_directory, log_directory, report, report_user });
        layout!(rust, FrameLabD3D12SrOptions { struct_size, abi_version, output_extent, mode, flags });
        layout!(rust, FrameLabD3D12SrSettings { struct_size, abi_version, optimal_render_extent,
            min_render_extent, max_render_extent });
        layout!(rust, FrameLabD3D12SrImage { resource_id, resource, state, lease });
        layout!(rust, FrameLabD3D12SrFrame { struct_size, abi_version, command_stream_id, commands, frame_index,
            render_extent, reset_history, pre_exposure, exposure_scale, color, depth, motion_vectors, output, camera });
        constants!(rust, FRAMELAB_SR_PERFORMANCE, FRAMELAB_SR_BALANCED, FRAMELAB_SR_QUALITY, FRAMELAB_D3D12_SR_HDR,
            FRAMELAB_D3D12_SR_DEPTH_INVERTED, FRAMELAB_D3D12_SR_AUTO_EXPOSURE);
        assert_eq!(EXPORT_NAMES.len(), 10);
    }
    #[cfg(feature = "d3d12-fg")]
    {
        use framelab_sys::d3d12_fg::*;
        layout!(rust, FrameLabD3D12FgConfig { struct_size, abi_version, device_id, queue_id, device, queue,
            completion_fence, extent, project_id, engine_version, ngx_runtime_directory, log_directory, report,
            report_user });
        layout!(rust, FrameLabD3D12FgImage { resource_id, resource, state });
        layout!(rust, FrameLabD3D12FgBindings { struct_size, abi_version, command_stream_id, commands, color,
            depth, motion_vectors, generated });
        assert_eq!(EXPORT_NAMES.len(), 9);
    }

    let missing: Vec<_> = native.keys().filter(|key| !rust.contains_key(*key)).collect();
    let extra: Vec<_> = rust.keys().filter(|key| !native.contains_key(*key)).collect();
    assert!(missing.is_empty() && extra.is_empty(), "not probed in Rust: {missing:?}; not in C: {extra:?}");
    let differing: Vec<_> = native.iter().filter(|(key, value)| rust[*key] != **value)
        .map(|(key, value)| format!("{key}: C {value}, Rust {}", rust[key])).collect();
    assert!(differing.is_empty(), "{differing:#?}");
    assert!(native.len() >= 2, "probe output is empty");
}
