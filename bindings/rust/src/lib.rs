//! Raw FrameLab ABI v1 bindings. The caller owns GPU submission and completion.
//! See the C header and SDK guide before constructing pointers or retiring a frame.
//!
//! Features (all additive):
//! - `link` (default): link the shared FrameLab library and declare its exports.
//! - `runtime`: load FrameLab from an explicit path with [`runtime::Library`].
//! - `d3d12-sr`, `d3d12-fg`: raw bindings for the optional Windows x86_64 D3D12 components.

use core::ffi::{c_char, c_void};

pub type FrameLabStatus = u32;
pub type FrameLabReleaseFn = Option<unsafe extern "C" fn(*mut c_void)>;

#[macro_use]
mod macros;
mod constants;
pub use constants::*;

#[cfg(all(windows, target_arch = "x86_64", any(feature = "d3d12-sr", feature = "d3d12-fg")))]
pub mod d3d12;
#[cfg(all(windows, target_arch = "x86_64", feature = "d3d12-fg"))]
pub mod d3d12_fg;
#[cfg(all(windows, target_arch = "x86_64", feature = "d3d12-sr"))]
pub mod d3d12_sr;
#[cfg(feature = "runtime")]
pub mod runtime;

#[repr(C)]
pub struct FrameLabLease {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct FrameLabFgStream {
    _opaque: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FrameLabExtent {
    pub width: u32,
    pub height: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FrameLabImage {
    pub resource_id: u64,
    pub frame_index: u64,
    pub owner_queue_id: u64,
    pub extent: FrameLabExtent,
    pub format: u32,
    pub state: u32,
    pub provenance: u32,
    pub lease: *const FrameLabLease,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FrameLabCamera {
    pub view_to_clip: [f32; 16],
    pub clip_to_view: [f32; 16],
    pub clip_to_lens: [f32; 16],
    pub clip_to_previous: [f32; 16],
    pub previous_to_clip: [f32; 16],
    pub position: [f32; 3],
    pub up: [f32; 3],
    pub right: [f32; 3],
    pub forward: [f32; 3],
    pub jitter_pixels: [f32; 2],
    pub motion_vector_scale: [f32; 2],
    pub near_plane: f32,
    pub far_plane: f32,
    pub fov_radians: f32,
    pub aspect: f32,
    pub projection_space: u32,
    pub reprojection_space: u32,
    pub motion_convention: u32,
    pub flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FrameLabProducerSync {
    pub queue_id: u64,
    pub command_stream_id: u64,
    pub signal_id: u64,
    pub timeline_value: u64,
    pub completion: u32,
    pub signal_lease: *const FrameLabLease,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FrameLabFgFrame {
    pub struct_size: u32,
    pub abi_version: u32,
    pub stream_id: u64,
    pub device_id: u64,
    pub generation: u64,
    pub frame_index: u64,
    pub render_extent: FrameLabExtent,
    pub output_extent: FrameLabExtent,
    pub delta_seconds: f64,
    pub color: FrameLabImage,
    pub depth: FrameLabImage,
    pub motion_vectors: FrameLabImage,
    pub camera: FrameLabCamera,
    pub producer: FrameLabProducerSync,
    pub reset_history: u32,
    pub flags: u32,
}

framelab_exports! {
    /// The portable core exports of <framelab/framelab.h>, resolved at run time.
    table CoreApi;
    fn framelab_abi_version() -> u32;
    fn framelab_status_string(status: FrameLabStatus) -> *const c_char;
    fn framelab_lease_create(
        user: *mut c_void,
        release: FrameLabReleaseFn,
        out_lease: *mut *mut FrameLabLease,
    ) -> FrameLabStatus;
    fn framelab_lease_destroy(lease: *mut FrameLabLease) -> ();
    fn framelab_fg_stream_create(
        abi_version: u32,
        out_stream: *mut *mut FrameLabFgStream,
    ) -> FrameLabStatus;
    fn framelab_fg_stream_destroy(stream: *mut FrameLabFgStream) -> FrameLabStatus;
    fn framelab_fg_prepare(
        stream: *mut FrameLabFgStream,
        frame: *const FrameLabFgFrame,
    ) -> FrameLabStatus;
    fn framelab_fg_mark_submitted(stream: *mut FrameLabFgStream) -> FrameLabStatus;
    fn framelab_fg_cancel(stream: *mut FrameLabFgStream) -> FrameLabStatus;
    fn framelab_fg_retire(stream: *mut FrameLabFgStream) -> FrameLabStatus;
    fn framelab_fg_invalidate(stream: *mut FrameLabFgStream) -> FrameLabStatus;
}
