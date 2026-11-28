// SPDX-License-Identifier: MIT
//! Native D3D12 FG through FrameLab from Rust: the scene, frame metadata and checks of the C++
//! `samples/d3d12_fg` consumer, driven through the `framelab-sys` D3D12 FG bindings and runtime
//! loading.
//!
//! Records NGX feature creation, then six 1280x720 frames of a rectangle moving 32 pixels per
//! frame. Frame 0 also checks that a wrong native state is rejected and frame 2 that stale depth
//! is rejected, each before the valid frame is recorded on the same list. The last submission is
//! gated: with every host reference dropped, early retirement and destruction must fail and no
//! lease may be released until the real fence completes. Each generated frame is read back and
//! the rectangle located on the middle row. No DXGI Present. No SR handle exists in this process.

use framelab_rust_d3d12::gpu::{Gpu, Texture};
use framelab_rust_d3d12::host::{self, Args, DllDirectory, Lease, EXIT_FALLBACK};
use framelab_rust_d3d12::{half, require, Result, SENTINEL};
use framelab_sys::d3d12_fg::*;
use framelab_sys::{
    CoreApi, FrameLabCamera, FrameLabExtent, FrameLabFgFrame, FrameLabImage, FrameLabProducerSync,
    FRAMELAB_ABI_VERSION, FRAMELAB_BUSY, FRAMELAB_CAMERA_MOTION_INCLUDED, FRAMELAB_CAMERA_VERIFIED,
    FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE, FRAMELAB_COMPLETION_SAME_COMMAND_STREAM, FRAMELAB_FORMAT_R32_FLOAT,
    FRAMELAB_FORMAT_RG16_FLOAT, FRAMELAB_FORMAT_RGBA8_UNORM, FRAMELAB_INVALID_RESOURCE_STATE,
    FRAMELAB_INVALID_SEQUENCE, FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV, FRAMELAB_NOT_READY, FRAMELAB_OK,
    FRAMELAB_PROVENANCE_SYNTHETIC, FRAMELAB_RESET_INITIAL, FRAMELAB_STATE_SHADER_READ, FRAMELAB_UNSUPPORTED,
};
use std::mem::size_of;
use std::ptr::{null, null_mut, NonNull};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Instant;
use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::{
    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM,
};

const EXTENT: FrameLabExtent = FrameLabExtent {
    width: 1280,
    height: 720,
};
const FRAMES: u64 = 6;
const RECTANGLE: u32 = 0xff2080f0;

/// An FG handle on the runtime-loaded component. Destroyed on drop when idle.
struct Fg<'a> {
    api: &'a D3D12FgApi,
    core: &'a CoreApi,
    handle: NonNull<FrameLabD3D12Fg>,
}

impl<'a> Fg<'a> {
    /// `Ok(None)` when the runtime reports no native FG support.
    fn new(api: &'a D3D12FgApi, core: &'a CoreApi, config: &FrameLabD3D12FgConfig) -> Result<Option<Self>> {
        let mut handle = null_mut();
        let status = unsafe { (api.framelab_d3d12_fg_create)(config, &mut handle) };
        if status == FRAMELAB_UNSUPPORTED {
            return Ok(None);
        }
        host::expect(core, status, FRAMELAB_OK, "create FG")?;
        Ok(Some(Self {
            api,
            core,
            handle: NonNull::new(handle).ok_or("FG create returned a null handle")?,
        }))
    }

    fn raw(&self) -> *mut FrameLabD3D12Fg {
        self.handle.as_ptr()
    }

    fn check(&self, status: u32, want: u32, operation: &str) -> Result<()> {
        host::expect(self.core, status, want, operation)
    }

    fn record_initialize(&self, gpu: &mut Gpu) -> Result<()> {
        gpu.begin()?;
        let status = unsafe { (self.api.framelab_d3d12_fg_record_initialize)(self.raw(), gpu.list.as_raw().cast()) };
        self.check(status, FRAMELAB_OK, "record native FG creation")?;
        let value = gpu.submit()?;
        self.acknowledge(value)?;
        gpu.wait()?;
        self.retire(FRAMELAB_OK)
    }

    fn record(&self, frame: &FrameLabFgFrame, bindings: &FrameLabD3D12FgBindings) -> u32 {
        unsafe { (self.api.framelab_d3d12_fg_record)(self.raw(), frame, bindings) }
    }

    fn acknowledge(&self, value: u64) -> Result<()> {
        let status = unsafe { (self.api.framelab_d3d12_fg_mark_submitted)(self.raw(), value) };
        self.check(status, FRAMELAB_OK, "acknowledge the queued signal")
    }

    fn retire(&self, want: u32) -> Result<()> {
        let status = unsafe { (self.api.framelab_d3d12_fg_retire)(self.raw()) };
        self.check(status, want, "retire FG packet")
    }

    fn last_ngx_result(&self) -> u32 {
        unsafe { (self.api.framelab_d3d12_fg_last_ngx_result)(self.raw()) }
    }

    fn close(self) -> Result<()> {
        let core = self.core;
        let status = unsafe { (self.api.framelab_d3d12_fg_destroy)(self.raw()) };
        if status == FRAMELAB_OK {
            std::mem::forget(self);
        }
        host::expect(core, status, FRAMELAB_OK, "destroy the idle FG handle")
    }
}

impl Drop for Fg<'_> {
    fn drop(&mut self) {
        let status = unsafe { (self.api.framelab_d3d12_fg_destroy)(self.raw()) };
        if status != FRAMELAB_OK {
            eprintln!("FG handle stays alive after destroy status {status}");
        }
    }
}

/// The C++ sample's camera: a left-handed perspective projection (vertical FOV 1 rad, near 0.1,
/// far 1000) as DirectXMath builds it, its inverse, and identity reprojection.
fn camera() -> FrameLabCamera {
    let aspect = EXTENT.width as f32 / EXTENT.height as f32;
    let (near, far) = (0.1f32, 1000f32);
    let (sin, cos) = (0.5f32).sin_cos();
    let height = cos / sin;
    let width = height / aspect;
    let range = far / (far - near);
    #[rustfmt::skip]
    let projection = [
        width, 0., 0., 0.,
        0., height, 0., 0.,
        0., 0., range, 1.,
        0., 0., -range * near, 0.,
    ];
    #[rustfmt::skip]
    let inverse = [
        1. / width, 0., 0., 0.,
        0., 1. / height, 0., 0.,
        0., 0., 0., -1. / (range * near),
        0., 0., 1., 1. / near,
    ];
    let identity = [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.];
    FrameLabCamera {
        view_to_clip: projection,
        clip_to_view: inverse,
        clip_to_lens: identity,
        clip_to_previous: identity,
        previous_to_clip: identity,
        position: [0.; 3],
        up: [0., 1., 0.],
        right: [1., 0., 0.],
        forward: [0., 0., 1.],
        jitter_pixels: [0.; 2],
        motion_vector_scale: [1.; 2],
        near_plane: near,
        far_plane: far,
        fov_radians: 1.,
        aspect,
        projection_space: FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE,
        reprojection_space: FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE,
        motion_convention: FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV,
        flags: FRAMELAB_CAMERA_VERIFIED | FRAMELAB_CAMERA_MOTION_INCLUDED,
    }
}

fn left(frame: u64) -> u32 {
    160 + frame as u32 * 32
}

fn object(x: u32, y: u32, frame: u64) -> bool {
    x >= left(frame) && x < left(frame) + 160 && (240..480).contains(&y)
}

/// RGBA8 color, R32_FLOAT depth and RG16_FLOAT motion (previous minus current, UV) of a frame.
fn scene(frame: u64) -> (Vec<u32>, Vec<u8>, Vec<u8>) {
    let area = EXTENT.width as usize * EXTENT.height as usize;
    let (mut pixels, mut depth, mut motion) = (
        Vec::with_capacity(area),
        Vec::with_capacity(4 * area),
        Vec::with_capacity(4 * area),
    );
    let velocity = half(-32. / EXTENT.width as f32);
    for y in 0..EXTENT.height {
        for x in 0..EXTENT.width {
            let inside = object(x, y, frame);
            pixels.push(if inside {
                RECTANGLE
            } else if (x / 32 + y / 32) % 2 == 1 {
                0xff303030
            } else {
                0xff181818
            });
            depth.extend((if inside { 0.3f32 } else { 0.9f32 }).to_le_bytes());
            motion.extend((if inside { velocity } else { 0 }).to_le_bytes());
            motion.extend(0u16.to_le_bytes());
        }
    }
    (pixels, depth, motion)
}

fn bytes(texels: &[u32]) -> Vec<u8> {
    texels.iter().flat_map(|texel| texel.to_le_bytes()).collect()
}

/// Left and right edges of the orange rectangle on the middle row, or `None` when absent.
fn edges(image: &[u32]) -> Option<(i64, i64)> {
    let row = &image[(EXTENT.height / 2 * EXTENT.width) as usize..][..EXTENT.width as usize];
    let mut found = None;
    for (x, texel) in row.iter().enumerate() {
        let [red, _, blue, _] = texel.to_le_bytes();
        if i32::from(red) - i32::from(blue) >= 0x60 {
            let (first, _) = found.unwrap_or((x as i64, 0));
            found = Some((first, x as i64 + 1));
        }
    }
    found
}

fn image(id: u64, frame: u64, format: u32, lease: &Lease) -> FrameLabImage {
    FrameLabImage {
        resource_id: id,
        frame_index: frame,
        owner_queue_id: 1,
        extent: EXTENT,
        format,
        state: FRAMELAB_STATE_SHADER_READ,
        provenance: FRAMELAB_PROVENANCE_SYNTHETIC,
        lease: lease.as_ptr(),
    }
}

fn binding(id: u64, texture: &Texture) -> FrameLabD3D12FgImage {
    FrameLabD3D12FgImage {
        resource_id: id,
        resource: texture.raw(),
        state: texture.state.0 as u32,
    }
}

struct Outcome {
    verified: u32,
    releases: usize,
    max_edge_error: i64,
    last: Vec<u32>,
}

fn frames(gpu: &mut Gpu, core: &CoreApi, fg: &Fg) -> Result<Outcome> {
    let mut color = gpu.texture(EXTENT, DXGI_FORMAT_R8G8B8A8_UNORM, false)?;
    let mut depth = gpu.texture(EXTENT, DXGI_FORMAT_R32_FLOAT, false)?;
    let mut motion = gpu.texture(EXTENT, DXGI_FORMAT_R16G16_FLOAT, false)?;
    let mut generated = gpu.texture(EXTENT, DXGI_FORMAT_R8G8B8A8_UNORM, true)?;
    let released = Arc::new(AtomicUsize::new(0));
    let mut leases = Some([
        Lease::new(core, &color.resource, &released)?,
        Lease::new(core, &depth.resource, &released)?,
        Lease::new(core, &motion.resource, &released)?,
    ]);
    let weak = [&color, &depth, &motion].map(|texture| Arc::downgrade(&texture.resource));
    let clear = bytes(&vec![SENTINEL; EXTENT.width as usize * EXTENT.height as usize]);
    let view = camera();
    let mut previous = Vec::new();
    let mut outcome = Outcome {
        verified: 0,
        releases: 0,
        max_edge_error: 0,
        last: Vec::new(),
    };
    let mut time = Instant::now();
    for i in 0..FRAMES {
        let now = Instant::now();
        let (pixels, depths, vectors) = scene(i);
        gpu.begin()?;
        gpu.upload(
            &mut color,
            &bytes(&pixels),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        )?;
        gpu.upload(&mut depth, &depths, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)?;
        gpu.upload(&mut motion, &vectors, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)?;
        gpu.upload(&mut generated, &clear, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)?;
        let [color_lease, depth_lease, motion_lease] = leases.as_ref().ok_or("leases dropped early")?;
        let frame = FrameLabFgFrame {
            struct_size: size_of::<FrameLabFgFrame>() as u32,
            abi_version: FRAMELAB_ABI_VERSION,
            stream_id: 1,
            device_id: 1,
            generation: 1,
            frame_index: i,
            render_extent: EXTENT,
            output_extent: EXTENT,
            delta_seconds: if i == 0 { 1. / 60. } else { (now - time).as_secs_f64() },
            color: image(11, i, FRAMELAB_FORMAT_RGBA8_UNORM, color_lease),
            depth: image(12, i, FRAMELAB_FORMAT_R32_FLOAT, depth_lease),
            motion_vectors: image(13, i, FRAMELAB_FORMAT_RG16_FLOAT, motion_lease),
            camera: view,
            producer: FrameLabProducerSync {
                queue_id: 1,
                command_stream_id: i + 1,
                signal_id: 0,
                timeline_value: 0,
                completion: FRAMELAB_COMPLETION_SAME_COMMAND_STREAM,
                signal_lease: null(),
            },
            reset_history: if i == 0 { FRAMELAB_RESET_INITIAL } else { 0 },
            flags: 0,
        };
        time = now;
        let bindings = FrameLabD3D12FgBindings {
            struct_size: size_of::<FrameLabD3D12FgBindings>() as u32,
            abi_version: FRAMELAB_ABI_VERSION,
            command_stream_id: i + 1,
            commands: gpu.list.as_raw().cast(),
            color: binding(11, &color),
            depth: binding(12, &depth),
            motion_vectors: binding(13, &motion),
            generated: binding(14, &generated),
        };
        // Rejections happen before recording and leave the host list usable for the valid frame.
        // Frame 0 carries the initial reset, so its rejection leaves continuity intact.
        if i == 0 {
            let mut bad = bindings;
            bad.color.state = D3D12_RESOURCE_STATE_COPY_DEST.0 as u32;
            fg.check(
                fg.record(&frame, &bad),
                FRAMELAB_INVALID_RESOURCE_STATE,
                "wrong native state",
            )?;
            println!("RUST_FG_CHECK invalid_native_state_rejected=1");
        }
        if i == 2 {
            let mut stale = frame;
            stale.depth.frame_index -= 1;
            fg.check(fg.record(&stale, &bindings), FRAMELAB_INVALID_SEQUENCE, "stale depth")?;
            println!("RUST_FG_CHECK stale_depth_rejected=1");
        }
        fg.check(fg.record(&frame, &bindings), FRAMELAB_OK, "record native FG evaluate")?;
        let capture = gpu.readback(&mut generated)?;
        let gate = if i + 1 == FRAMES {
            Some(gpu.gate_next_submission()?)
        } else {
            None
        };
        let value = gpu.submit()?;
        fg.acknowledge(value)?;
        if let Some(gate) = gate {
            // Drop every host reference to the inputs and the generated image while the GPU waits.
            leases = None;
            for texture in [&mut color, &mut depth, &mut motion, &mut generated] {
                *texture = gpu.texture(
                    FrameLabExtent { width: 1, height: 1 },
                    DXGI_FORMAT_R8G8B8A8_UNORM,
                    false,
                )?;
            }
            fg.retire(FRAMELAB_NOT_READY)?;
            let live = weak.iter().filter(|texture| texture.strong_count() > 0).count();
            require(
                released.load(Ordering::SeqCst) == 0 && live == 3,
                "early retirement released pending resources",
            )?;
            let status = unsafe { (fg.api.framelab_d3d12_fg_destroy)(fg.raw()) };
            fg.check(status, FRAMELAB_BUSY, "destroy with pending work")?;
            gate.open()?;
            println!("RUST_FG_CHECK early_retire_rejected=1 pending_destroy_rejected=1 live_before_fence={live}");
        }
        gpu.wait()?;
        fg.retire(FRAMELAB_OK)?;
        let output = capture.texels()?;
        let written = !output.contains(&SENTINEL);
        let interpolated = i > 0 && written && output != pixels && output != previous;
        // The generated frame lies halfway between frames i - 1 and i: 16 pixels left of frame i.
        let expected = i64::from(left(i)) - 16;
        let found = edges(&output);
        if interpolated {
            outcome.verified += 1;
            let error = match found {
                Some((first, end)) => (first - expected).abs().max((end - (expected + 160)).abs()),
                None => i64::from(EXTENT.width),
            };
            outcome.max_edge_error = outcome.max_edge_error.max(error);
        }
        let (first, end) = found.unwrap_or((-1, -1));
        println!(
            "RUST_FG_FRAME frame={i} fence={} ngx={:#x} left={first} right={end} expected_left={}",
            gpu.value(),
            fg.last_ngx_result(),
            if i > 0 { expected } else { -1 }
        );
        previous = pixels;
        outcome.last = output;
    }
    let after = weak.iter().filter(|texture| texture.strong_count() > 0).count();
    outcome.releases = released.load(Ordering::SeqCst);
    require(after == 0, "retirement left lease owners alive")?;
    Ok(outcome)
}

/// Without FG the host still renders and shows its real frames: frame 0, read back and compared.
fn ordinary_only(gpu: &mut Gpu) -> Result<()> {
    let (pixels, _, _) = scene(0);
    let mut texture = gpu.texture(EXTENT, DXGI_FORMAT_R8G8B8A8_UNORM, false)?;
    gpu.begin()?;
    gpu.upload(&mut texture, &bytes(&pixels), D3D12_RESOURCE_STATE_COPY_SOURCE)?;
    let capture = gpu.readback(&mut texture)?;
    gpu.submit()?;
    gpu.wait()?;
    require(capture.texels()? == pixels, "ordinary readback differs")?;
    println!("RUST_FG_ORDINARY frames=1 verified=1");
    Ok(())
}

fn run() -> Result<i32> {
    let args = Args::parse("framelab-rust-fg")?;
    let runtime = host::wide(&args.runtime);
    let logs = host::wide(&args.logs);
    let _search = DllDirectory::add(&runtime)?;
    let mut gpu = Gpu::new(args.debug_layer)?;
    println!(
        "RUST_FG_VALIDATION debug_layer={0} gpu_based={0}",
        args.debug_layer as u8
    );
    let library = match host::open(&args.library) {
        Ok(library) => library,
        Err(reason) => {
            host::fallback("library", &reason);
            ordinary_only(&mut gpu)?;
            return Ok(EXIT_FALLBACK);
        }
    };
    let core = library.core();
    let api = match library.d3d12_fg() {
        Ok(api) => api,
        Err(reason) => {
            host::fallback("d3d12-fg", &reason.to_string());
            ordinary_only(&mut gpu)?;
            return Ok(EXIT_FALLBACK);
        }
    };
    // The C++ sample's IDs, so both consumers drive NGX identically.
    let config = FrameLabD3D12FgConfig {
        struct_size: size_of::<FrameLabD3D12FgConfig>() as u32,
        abi_version: FRAMELAB_ABI_VERSION,
        device_id: 1,
        queue_id: 1,
        device: gpu.device.as_raw().cast(),
        queue: gpu.queue.as_raw().cast(),
        completion_fence: gpu.fence.as_raw().cast(),
        extent: EXTENT,
        project_id: c"31246941-7a24-484c-a4a7-e436c26c6f95".as_ptr(),
        engine_version: c"phase2.1".as_ptr(),
        ngx_runtime_directory: runtime.as_ptr(),
        log_directory: logs.as_ptr(),
        report: Some(host::report),
        report_user: null_mut(),
    };
    let Some(fg) = Fg::new(&api, core, &config)? else {
        host::fallback("d3d12-fg", "FRAMELAB_UNSUPPORTED from create");
        ordinary_only(&mut gpu)?;
        return Ok(EXIT_FALLBACK);
    };
    fg.record_initialize(&mut gpu)?;
    let outcome = frames(&mut gpu, core, &fg)?;
    fg.close()?;
    std::fs::write(args.logs.join("generated-5.rgba"), bytes(&outcome.last))?;
    println!(
        "RUST_FG_SUMMARY real_frames={FRAMES} verified={} final_lease_releases={} max_edge_error={} presents=0",
        outcome.verified, outcome.releases, outcome.max_edge_error
    );
    require(
        outcome.verified == FRAMES as u32 - 1 && outcome.releases == 3,
        "not every frame interpolated and retired",
    )?;
    // Closer to the halfway position than to either real frame (each 16 pixels away).
    require(
        outcome.max_edge_error < 16,
        "a generated rectangle is not between its real frames",
    )?;
    if let Some(messages) = gpu.write_messages(&args.logs.join("d3d12-debug.log"))? {
        require(
            messages.corruption == 0 && messages.error == 0 && messages.warning == 0 && messages.discarded == 0,
            "the D3D12 debug layer reported diagnostics; see d3d12-debug.log",
        )?;
    }
    drop(api);
    drop(library);
    Ok(0)
}

fn main() {
    std::process::exit(match run() {
        Ok(code) => code,
        Err(error) => {
            eprintln!("framelab-rust-fg: {error}");
            1
        }
    });
}
