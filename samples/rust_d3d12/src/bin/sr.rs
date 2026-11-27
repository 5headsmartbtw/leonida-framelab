// SPDX-License-Identifier: MIT
//! Native D3D12 SR through FrameLab from Rust, adapted from the maintainer's standalone prototype
//! (FrameLab #43) to the `framelab-sys` D3D12 SR bindings and runtime loading.
//!
//! Configures 1280x720 (Quality), then evaluates frames 0, 1 and 3 and verifies each readback,
//! rejects frame 2 (missing depth) and renders it without SR, records frame 4 and cancels it
//! without submitting, reconfigures 960x540, and evaluates frame 5 behind a gate that proves the
//! four leases and their COM resources stay alive until the real fence completes.
//! No DXGI Present. No FG handle exists in this process.

use framelab_rust_d3d12::gpu::{Gpu, Texture};
use framelab_rust_d3d12::host::{self, Args, DllDirectory, Lease, EXIT_FALLBACK};
use framelab_rust_d3d12::{require, Result, SENTINEL};
use framelab_sys::d3d12_sr::*;
use framelab_sys::{
    CoreApi, FrameLabCamera, FrameLabExtent, FRAMELAB_ABI_VERSION, FRAMELAB_CAMERA_MOTION_INCLUDED,
    FRAMELAB_CAMERA_VERIFIED, FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE, FRAMELAB_INVALID_IMAGE_SEMANTICS,
    FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV, FRAMELAB_NOT_READY, FRAMELAB_OK, FRAMELAB_RESET_DISCONTINUITY,
    FRAMELAB_RESET_INITIAL, FRAMELAB_RESET_RESIZE, FRAMELAB_UNSUPPORTED,
};
use std::mem::size_of;
use std::ptr::{null_mut, NonNull};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::{
    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM,
};

const FIRST: FrameLabExtent = FrameLabExtent {
    width: 1280,
    height: 720,
};
const RESIZED: FrameLabExtent = FrameLabExtent {
    width: 960,
    height: 540,
};

/// An SR handle on the runtime-loaded component. Destroyed on drop when idle.
struct Sr<'a> {
    api: &'a D3D12SrApi,
    core: &'a CoreApi,
    handle: NonNull<FrameLabD3D12Sr>,
}

impl<'a> Sr<'a> {
    /// `Ok(None)` when the runtime reports no native SR support.
    fn new(api: &'a D3D12SrApi, core: &'a CoreApi, config: &FrameLabD3D12SrConfig) -> Result<Option<Self>> {
        let mut handle = null_mut();
        let status = unsafe { (api.framelab_d3d12_sr_create)(config, &mut handle) };
        if status == FRAMELAB_UNSUPPORTED {
            return Ok(None);
        }
        host::expect(core, status, FRAMELAB_OK, "create SR")?;
        Ok(Some(Self {
            api,
            core,
            handle: NonNull::new(handle).ok_or("SR create returned a null handle")?,
        }))
    }

    fn configure(&self, gpu: &mut Gpu, output: FrameLabExtent) -> Result<FrameLabExtent> {
        let zero = FrameLabExtent { width: 0, height: 0 };
        let options = FrameLabD3D12SrOptions {
            struct_size: size_of::<FrameLabD3D12SrOptions>() as u32,
            abi_version: FRAMELAB_ABI_VERSION,
            output_extent: output,
            mode: FRAMELAB_SR_QUALITY,
            flags: 0,
        };
        let mut settings = FrameLabD3D12SrSettings {
            struct_size: size_of::<FrameLabD3D12SrSettings>() as u32,
            abi_version: FRAMELAB_ABI_VERSION,
            optimal_render_extent: zero,
            min_render_extent: zero,
            max_render_extent: zero,
        };
        gpu.begin()?;
        let status = unsafe {
            (self.api.framelab_d3d12_sr_record_configure)(
                self.handle.as_ptr(),
                gpu.list.as_raw().cast(),
                &options,
                &mut settings,
            )
        };
        host::expect(self.core, status, FRAMELAB_OK, "record SR feature configuration")?;
        let render = settings.optimal_render_extent;
        require(
            render.width > 0 && render.height > 0,
            "runtime returned an empty optimal extent",
        )?;
        let value = gpu.submit()?;
        self.acknowledge(value)?;
        gpu.wait()?;
        self.retire(FRAMELAB_OK)?;
        println!(
            "RUST_SR_CONFIGURED render={}x{} output={}x{}",
            render.width, render.height, output.width, output.height
        );
        Ok(render)
    }

    fn record(&self, frame: &FrameLabD3D12SrFrame, want: u32) -> Result<()> {
        let status = unsafe { (self.api.framelab_d3d12_sr_record)(self.handle.as_ptr(), frame) };
        host::expect(self.core, status, want, "record SR evaluation")
    }

    fn acknowledge(&self, value: u64) -> Result<()> {
        let status = unsafe { (self.api.framelab_d3d12_sr_mark_submitted)(self.handle.as_ptr(), value) };
        host::expect(self.core, status, FRAMELAB_OK, "acknowledge the queued signal")
    }

    fn retire(&self, want: u32) -> Result<()> {
        let status = unsafe { (self.api.framelab_d3d12_sr_retire)(self.handle.as_ptr()) };
        host::expect(self.core, status, want, "retire SR packet")
    }

    fn cancel(&self) -> Result<()> {
        let status = unsafe { (self.api.framelab_d3d12_sr_cancel)(self.handle.as_ptr()) };
        host::expect(self.core, status, FRAMELAB_OK, "cancel the discarded SR recording")
    }

    fn close(self) -> Result<()> {
        let core = self.core;
        let status = unsafe { (self.api.framelab_d3d12_sr_destroy)(self.handle.as_ptr()) };
        if status == FRAMELAB_OK {
            std::mem::forget(self);
        }
        host::expect(core, status, FRAMELAB_OK, "destroy the idle SR handle")
    }
}

impl Drop for Sr<'_> {
    fn drop(&mut self) {
        let status = unsafe { (self.api.framelab_d3d12_sr_destroy)(self.handle.as_ptr()) };
        if status != FRAMELAB_OK {
            eprintln!("SR handle stays alive after destroy status {status}");
        }
    }
}

/// A moving orange rectangle on a grey checkerboard at the render extent, with a stationary
/// orthographic camera.
struct Scene {
    render: FrameLabExtent,
    left: u32,
    width: u32,
    motion_x_uv: f32,
}

impl Scene {
    fn new(render: FrameLabExtent, index: u64) -> Self {
        let left_at = |frame: u64| render.width * (15 + (frame % 6) as u32 * 7) / 100;
        let left = left_at(index);
        let previous = left_at(index.saturating_sub(1));
        Self {
            render,
            left,
            width: render.width * 18 / 100,
            motion_x_uv: (previous as f32 - left as f32) / render.width as f32,
        }
    }

    fn object(&self, x: u32, y: u32) -> bool {
        (self.left..self.left + self.width).contains(&x)
            && (self.render.height * 30 / 100..self.render.height * 65 / 100).contains(&y)
    }

    fn color(&self, x: u32, y: u32) -> u32 {
        if self.object(x, y) {
            0xff2080f0
        } else if (x / 32 + y / 32) % 2 == 0 {
            0xff303030
        } else {
            0xff181818
        }
    }

    fn camera(&self) -> FrameLabCamera {
        let projection = [2., 0., 0., 0., 0., -2., 0., 0., 0., 0., 1., 0., -1., 1., 0., 1.];
        let inverse = [0.5, 0., 0., 0., 0., -0.5, 0., 0., 0., 0., 1., 0., 0.5, 0.5, 0., 1.];
        let identity = [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.];
        FrameLabCamera {
            view_to_clip: projection,
            clip_to_view: inverse,
            clip_to_lens: identity,
            clip_to_previous: identity,
            previous_to_clip: identity,
            position: [0., 0., 0.],
            up: [0., 1., 0.],
            right: [1., 0., 0.],
            forward: [0., 0., 1.],
            jitter_pixels: [0., 0.],
            motion_vector_scale: [1., 1.],
            near_plane: 0.,
            far_plane: 1.,
            fov_radians: 0.,
            aspect: self.render.width as f32 / self.render.height as f32,
            projection_space: FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE,
            reprojection_space: FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE,
            motion_convention: FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV,
            flags: FRAMELAB_CAMERA_VERIFIED | FRAMELAB_CAMERA_MOTION_INCLUDED,
        }
    }

    /// RGBA8 color, R32_FLOAT depth and RG32_FLOAT motion (previous minus current, UV).
    fn inputs(&self) -> [Vec<u8>; 3] {
        let area = self.render.width as usize * self.render.height as usize;
        let (mut color, mut depth, mut motion) = (
            Vec::with_capacity(4 * area),
            Vec::with_capacity(4 * area),
            Vec::with_capacity(8 * area),
        );
        for y in 0..self.render.height {
            for x in 0..self.render.width {
                let object = self.object(x, y);
                color.extend(self.color(x, y).to_le_bytes());
                depth.extend((if object { 0.3f32 } else { 0.9f32 }).to_le_bytes());
                motion.extend((if object { self.motion_x_uv } else { 0. }).to_le_bytes());
                motion.extend(0f32.to_le_bytes());
            }
        }
        [color, depth, motion]
    }

    /// What the host shows without SR: the render-size image stretched to the output.
    fn ordinary_output(&self, output: FrameLabExtent) -> Vec<u32> {
        (0..output.height)
            .flat_map(|y| (0..output.width).map(move |x| (x, y)))
            .map(|(x, y)| {
                self.color(
                    x * self.render.width / output.width,
                    y * self.render.height / output.height,
                )
            })
            .collect()
    }

    /// The upscaled rectangle covers the expected area and sits where the scene put it.
    fn verify(&self, output: FrameLabExtent, pixels: &[u32], index: u64) -> Result<()> {
        let area = output.width as usize * output.height as usize;
        require(pixels.len() == area, "output readback dimensions")?;
        require(
            !pixels.contains(&SENTINEL),
            "SR output still contains the clear sentinel",
        )?;
        let (mut colored, mut x_sum) = (0usize, 0usize);
        for (i, pixel) in pixels.iter().enumerate() {
            let [r, g, b, _] = pixel.to_le_bytes();
            if r > 150 && r.saturating_sub(g) >= 30 && g.saturating_sub(b) >= 25 {
                colored += 1;
                x_sum += i % output.width as usize;
            }
        }
        let coverage = colored as f64 / area as f64;
        require(
            (0.045..0.09).contains(&coverage),
            format!("SR frame {index}: object coverage {coverage:.4}"),
        )?;
        let center = x_sum as f64 / colored as f64 / output.width as f64;
        let expected = (self.left as f64 + self.width as f64 / 2.) / self.render.width as f64;
        require(
            (center - expected).abs() < 0.035,
            format!("SR frame {index}: object center {center:.4}, expected {expected:.4}"),
        )?;
        println!("RUST_SR_VERIFIED frame={index} coverage={coverage:.4} center={center:.4} expected={expected:.4}");
        Ok(())
    }
}

/// Inputs and output of one configured extent, each owned by a lease.
struct Stage {
    render: FrameLabExtent,
    output: FrameLabExtent,
    base_id: u64,
    textures: [Texture; 4],
    leases: [Lease; 4],
    released: Arc<AtomicUsize>,
}

impl Stage {
    fn new(gpu: &Gpu, core: &CoreApi, render: FrameLabExtent, output: FrameLabExtent, base_id: u64) -> Result<Self> {
        let textures = [
            gpu.texture(render, DXGI_FORMAT_R8G8B8A8_UNORM, false)?,
            gpu.texture(render, DXGI_FORMAT_R32_FLOAT, false)?,
            gpu.texture(render, DXGI_FORMAT_R32G32_FLOAT, false)?,
            gpu.texture(output, DXGI_FORMAT_R8G8B8A8_UNORM, true)?,
        ];
        let released = Arc::new(AtomicUsize::new(0));
        let leases = [
            Lease::new(core, &textures[0].resource, &released)?,
            Lease::new(core, &textures[1].resource, &released)?,
            Lease::new(core, &textures[2].resource, &released)?,
            Lease::new(core, &textures[3].resource, &released)?,
        ];
        Ok(Self {
            render,
            output,
            base_id,
            textures,
            leases,
            released,
        })
    }

    fn begin(&mut self, gpu: &mut Gpu, scene: &Scene, index: u64, reset: u32) -> Result<FrameLabD3D12SrFrame> {
        gpu.begin()?;
        for (texture, data) in self.textures.iter_mut().zip(scene.inputs()) {
            gpu.upload(texture, &data, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)?;
        }
        let clear = SENTINEL
            .to_le_bytes()
            .repeat(self.output.width as usize * self.output.height as usize);
        gpu.upload(&mut self.textures[3], &clear, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)?;
        let image = |i: usize| FrameLabD3D12SrImage {
            resource_id: self.base_id + i as u64,
            resource: self.textures[i].raw(),
            state: self.textures[i].state.0 as u32,
            lease: self.leases[i].as_ptr(),
        };
        Ok(FrameLabD3D12SrFrame {
            struct_size: size_of::<FrameLabD3D12SrFrame>() as u32,
            abi_version: FRAMELAB_ABI_VERSION,
            command_stream_id: index + 1,
            commands: gpu.list.as_raw().cast(),
            frame_index: index,
            render_extent: self.render,
            reset_history: reset,
            pre_exposure: 1.,
            exposure_scale: 1.,
            color: image(0),
            depth: image(1),
            motion_vectors: image(2),
            output: image(3),
            camera: scene.camera(),
        })
    }

    fn evaluate(&mut self, gpu: &mut Gpu, sr: &Sr, index: u64, reset: u32) -> Result<()> {
        let scene = Scene::new(self.render, index);
        let frame = self.begin(gpu, &scene, index, reset)?;
        sr.record(&frame, FRAMELAB_OK)?;
        let capture = gpu.readback(&mut self.textures[3])?;
        let value = gpu.submit()?;
        sr.acknowledge(value)?;
        gpu.wait()?;
        sr.retire(FRAMELAB_OK)?;
        scene.verify(self.output, &capture.texels()?, index)
    }

    /// A frame without depth is rejected before recording; the same list then renders it without SR.
    fn fallback(&mut self, gpu: &mut Gpu, sr: &Sr, index: u64) -> Result<()> {
        let scene = Scene::new(self.render, index);
        let mut frame = self.begin(gpu, &scene, index, 0)?;
        frame.depth.resource = null_mut();
        sr.record(&frame, FRAMELAB_INVALID_IMAGE_SEMANTICS)?;
        let ordinary = scene.ordinary_output(self.output);
        let bytes: Vec<u8> = ordinary.iter().flat_map(|pixel| pixel.to_le_bytes()).collect();
        gpu.upload(&mut self.textures[3], &bytes, D3D12_RESOURCE_STATE_COPY_SOURCE)?;
        let capture = gpu.readback(&mut self.textures[3])?;
        gpu.submit()?;
        gpu.wait()?;
        require(
            capture.texels()? == ordinary,
            "fallback readback differs from the ordinary image",
        )?;
        println!("RUST_SR_FALLBACK frame={index} rejected=missing_depth ordinary_verified=1");
        Ok(())
    }

    /// Recorded, never submitted: cancel must release all four leases and the fence stays put.
    fn cancel(self, gpu: &mut Gpu, sr: &Sr, index: u64) -> Result<()> {
        let mut stage = self;
        let scene = Scene::new(stage.render, index);
        let before = unsafe { gpu.fence.GetCompletedValue() };
        let frame = stage.begin(gpu, &scene, index, 0)?;
        sr.record(&frame, FRAMELAB_OK)?;
        gpu.discard()?;
        let released = Arc::clone(&stage.released);
        drop(stage);
        require(
            released.load(Ordering::SeqCst) == 0,
            "leases released before cancellation",
        )?;
        sr.cancel()?;
        require(
            released.load(Ordering::SeqCst) == 4,
            "cancellation did not release all four leases",
        )?;
        require(
            unsafe { gpu.fence.GetCompletedValue() } == before,
            "the discarded list advanced the fence",
        )?;
        println!("RUST_SR_CANCEL frame={index} releases=4 submitted=0");
        Ok(())
    }

    /// The queue waits on a gate before the submission. Dropping every host reference must not
    /// release anything until the actual fence completes and the packet retires.
    fn evaluate_gated(self, gpu: &mut Gpu, sr: &Sr, index: u64) -> Result<()> {
        let mut stage = self;
        let scene = Scene::new(stage.render, index);
        let frame = stage.begin(gpu, &scene, index, FRAMELAB_RESET_RESIZE)?;
        sr.record(&frame, FRAMELAB_OK)?;
        let capture = gpu.readback(&mut stage.textures[3])?;
        let output = stage.output;
        let released = Arc::clone(&stage.released);
        let weak = stage
            .textures
            .each_ref()
            .map(|texture| Arc::downgrade(&texture.resource));
        let gate = gpu.gate_next_submission()?;
        let ticket = gpu.submit()?;
        sr.acknowledge(ticket)?;
        drop(stage);
        sr.retire(FRAMELAB_NOT_READY)?;
        let live = weak.iter().filter(|texture| texture.strong_count() > 0).count();
        require(
            released.load(Ordering::SeqCst) == 0 && live == 4,
            "resources released before GPU completion",
        )?;
        gate.open()?;
        gpu.wait()?;
        sr.retire(FRAMELAB_OK)?;
        let after = weak.iter().filter(|texture| texture.strong_count() > 0).count();
        require(
            released.load(Ordering::SeqCst) == 4 && after == 0,
            "retirement did not release all four owners",
        )?;
        scene.verify(output, &capture.texels()?, index)?;
        println!("RUST_SR_GATED frame={index} live_before_fence={live} releases_after_retire=4 live_after={after}");
        Ok(())
    }
}

/// Without SR the host still renders: frame 0 at 1280x720, read back and compared.
fn ordinary_only(gpu: &mut Gpu) -> Result<()> {
    let scene = Scene::new(FIRST, 0);
    let ordinary = scene.ordinary_output(FIRST);
    let mut texture = gpu.texture(FIRST, DXGI_FORMAT_R8G8B8A8_UNORM, false)?;
    gpu.begin()?;
    let bytes: Vec<u8> = ordinary.iter().flat_map(|pixel| pixel.to_le_bytes()).collect();
    gpu.upload(&mut texture, &bytes, D3D12_RESOURCE_STATE_COPY_SOURCE)?;
    let capture = gpu.readback(&mut texture)?;
    gpu.submit()?;
    gpu.wait()?;
    require(capture.texels()? == ordinary, "ordinary readback differs")?;
    println!("RUST_SR_ORDINARY frames=1 verified=1");
    Ok(())
}

fn run() -> Result<i32> {
    let args = Args::parse("framelab-rust-sr")?;
    let runtime = host::wide(&args.runtime);
    let logs = host::wide(&args.logs);
    let _search = DllDirectory::add(&runtime)?;
    let mut gpu = Gpu::new(args.debug_layer)?;
    println!(
        "RUST_SR_VALIDATION debug_layer={0} gpu_based={0}",
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
    let api = match library.d3d12_sr() {
        Ok(api) => api,
        Err(reason) => {
            host::fallback("d3d12-sr", &reason.to_string());
            ordinary_only(&mut gpu)?;
            return Ok(EXIT_FALLBACK);
        }
    };
    let config = FrameLabD3D12SrConfig {
        struct_size: size_of::<FrameLabD3D12SrConfig>() as u32,
        abi_version: FRAMELAB_ABI_VERSION,
        device: gpu.device.as_raw().cast(),
        queue: gpu.queue.as_raw().cast(),
        completion_fence: gpu.fence.as_raw().cast(),
        project_id: c"48304b3d-9931-4ac1-a3a9-629ef02ce08c".as_ptr(),
        engine_version: c"FrameLab Rust synthetic 0.1.0".as_ptr(),
        ngx_runtime_directory: runtime.as_ptr(),
        log_directory: logs.as_ptr(),
        report: Some(host::report),
        report_user: null_mut(),
    };
    let Some(sr) = Sr::new(&api, core, &config)? else {
        host::fallback("d3d12-sr", "FRAMELAB_UNSUPPORTED from create");
        ordinary_only(&mut gpu)?;
        return Ok(EXIT_FALLBACK);
    };
    let render = sr.configure(&mut gpu, FIRST)?;
    let mut stage = Stage::new(&gpu, core, render, FIRST, 10)?;
    stage.evaluate(&mut gpu, &sr, 0, FRAMELAB_RESET_INITIAL)?;
    stage.evaluate(&mut gpu, &sr, 1, 0)?;
    stage.fallback(&mut gpu, &sr, 2)?;
    stage.evaluate(&mut gpu, &sr, 3, FRAMELAB_RESET_DISCONTINUITY)?;
    stage.cancel(&mut gpu, &sr, 4)?;
    let render = sr.configure(&mut gpu, RESIZED)?;
    Stage::new(&gpu, core, render, RESIZED, 20)?.evaluate_gated(&mut gpu, &sr, 5)?;
    sr.close()?;
    println!("RUST_SR_SUMMARY evaluations=4 verified=4 fallback=1 cancelled=1 resized=1 presents=0");
    if let Some(messages) = gpu.write_messages(&args.logs.join("d3d12-debug.log"))? {
        require(
            messages.corruption == 0 && messages.error == 0 && messages.warning == 0 && messages.discarded == 0,
            "the D3D12 debug layer reported diagnostics; see d3d12-debug.log",
        )?;
    }
    // The library outlives every handle, lease and callback created through it.
    drop(api);
    drop(library);
    Ok(0)
}

fn main() {
    std::process::exit(match run() {
        Ok(code) => code,
        Err(error) => {
            eprintln!("framelab-rust-sr: {error}");
            1
        }
    });
}
