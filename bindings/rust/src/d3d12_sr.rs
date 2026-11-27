// SPDX-License-Identifier: MIT
//! Raw bindings for `<framelab/d3d12_sr.h>` (Windows x86_64, feature `d3d12-sr`).
//!
//! Every function is a raw FFI call. The SR guide (docs/sdk/D3D12_SR.md) defines the recording,
//! submission, retirement and cancellation rules. Until FL-16 is merged the shipped component
//! rejects an SR handle while a D3D12 FG handle exists in the process, and the reverse.

use crate::d3d12::{ID3D12CommandQueue, ID3D12Device, ID3D12Fence, ID3D12GraphicsCommandList, ID3D12Resource, WChar};
use crate::{FrameLabCamera, FrameLabExtent, FrameLabLease, FrameLabStatus};
use core::ffi::{c_char, c_void};

/// Opaque SR handle.
#[repr(C)]
pub struct FrameLabD3D12Sr {
    _opaque: [u8; 0],
}

/// Called synchronously on the calling thread. It must not unwind or re-enter FrameLab.
pub type FrameLabD3D12SrReportFn =
    Option<unsafe extern "C" fn(user: *mut c_void, operation: *const c_char, ngx_result: u32)>;

pub const FRAMELAB_SR_PERFORMANCE: u32 = 1;
pub const FRAMELAB_SR_BALANCED: u32 = 2;
pub const FRAMELAB_SR_QUALITY: u32 = 3;
/// `FrameLabD3D12SrOptions::flags`: linear HDR colour; requires RGBA16F colour and output.
pub const FRAMELAB_D3D12_SR_HDR: u32 = 1;
/// `FrameLabD3D12SrOptions::flags`: near depth 1, far depth 0.
pub const FRAMELAB_D3D12_SR_DEPTH_INVERTED: u32 = 2;
/// `FrameLabD3D12SrOptions::flags`: the runtime computes exposure.
pub const FRAMELAB_D3D12_SR_AUTO_EXPOSURE: u32 = 4;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FrameLabD3D12SrConfig {
    pub struct_size: u32,
    pub abi_version: u32,
    pub device: *mut ID3D12Device,
    /// Direct queue that executes every recorded SR list.
    pub queue: *mut ID3D12CommandQueue,
    /// Signaled by the host on that queue after each SR submission.
    pub completion_fence: *mut ID3D12Fence,
    pub project_id: *const c_char,
    pub engine_version: *const c_char,
    pub ngx_runtime_directory: *const WChar,
    pub log_directory: *const WChar,
    pub report: FrameLabD3D12SrReportFn,
    pub report_user: *mut c_void,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FrameLabD3D12SrOptions {
    pub struct_size: u32,
    pub abi_version: u32,
    pub output_extent: FrameLabExtent,
    pub mode: u32,
    pub flags: u32,
}

/// The caller sets `struct_size` and `abi_version`; FrameLab fills the extents.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FrameLabD3D12SrSettings {
    pub struct_size: u32,
    pub abi_version: u32,
    pub optimal_render_extent: FrameLabExtent,
    pub min_render_extent: FrameLabExtent,
    pub max_render_extent: FrameLabExtent,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FrameLabD3D12SrImage {
    pub resource_id: u64,
    pub resource: *mut ID3D12Resource,
    /// Producer-tracked `D3D12_RESOURCE_STATES` when the list executes.
    pub state: u32,
    /// Owns the allocation until its final release callback.
    pub lease: *const FrameLabLease,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FrameLabD3D12SrFrame {
    pub struct_size: u32,
    pub abi_version: u32,
    pub command_stream_id: u64,
    pub commands: *mut ID3D12GraphicsCommandList,
    pub frame_index: u64,
    pub render_extent: FrameLabExtent,
    pub reset_history: u32,
    pub pre_exposure: f32,
    pub exposure_scale: f32,
    pub color: FrameLabD3D12SrImage,
    pub depth: FrameLabD3D12SrImage,
    pub motion_vectors: FrameLabD3D12SrImage,
    pub output: FrameLabD3D12SrImage,
    pub camera: FrameLabCamera,
}

framelab_exports! {
    /// The D3D12 SR exports, resolved at run time independently of FG.
    table D3D12SrApi;
    fn framelab_d3d12_sr_create(
        config: *const FrameLabD3D12SrConfig,
        out_sr: *mut *mut FrameLabD3D12Sr,
    ) -> FrameLabStatus;
    fn framelab_d3d12_sr_destroy(sr: *mut FrameLabD3D12Sr) -> FrameLabStatus;
    fn framelab_d3d12_sr_query_settings(
        sr: *mut FrameLabD3D12Sr,
        options: *const FrameLabD3D12SrOptions,
        out_settings: *mut FrameLabD3D12SrSettings,
    ) -> FrameLabStatus;
    fn framelab_d3d12_sr_record_configure(
        sr: *mut FrameLabD3D12Sr,
        commands: *mut ID3D12GraphicsCommandList,
        options: *const FrameLabD3D12SrOptions,
        out_settings: *mut FrameLabD3D12SrSettings,
    ) -> FrameLabStatus;
    fn framelab_d3d12_sr_record(sr: *mut FrameLabD3D12Sr, frame: *const FrameLabD3D12SrFrame) -> FrameLabStatus;
    fn framelab_d3d12_sr_mark_submitted(sr: *mut FrameLabD3D12Sr, value: u64) -> FrameLabStatus;
    fn framelab_d3d12_sr_cancel(sr: *mut FrameLabD3D12Sr) -> FrameLabStatus;
    fn framelab_d3d12_sr_retire(sr: *mut FrameLabD3D12Sr) -> FrameLabStatus;
    fn framelab_d3d12_sr_invalidate(sr: *mut FrameLabD3D12Sr) -> FrameLabStatus;
    fn framelab_d3d12_sr_last_ngx_result(sr: *const FrameLabD3D12Sr) -> u32;
}
