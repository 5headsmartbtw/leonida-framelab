// SPDX-License-Identifier: MIT
//! Raw bindings for `<framelab/d3d12_fg.h>` (Windows x86_64, feature `d3d12-fg`).
//!
//! Every function is a raw FFI call. The FG guide (docs/sdk/D3D12_FG.md) defines the recording,
//! submission, retirement and cancellation rules. Until FL-16 is merged the shipped component
//! rejects an FG handle while a D3D12 SR handle exists in the process, and the reverse.

use crate::d3d12::{ID3D12CommandQueue, ID3D12Device, ID3D12Fence, ID3D12GraphicsCommandList, ID3D12Resource, WChar};
use crate::{FrameLabExtent, FrameLabFgFrame, FrameLabStatus};
use core::ffi::{c_char, c_void};

/// Opaque FG handle.
#[repr(C)]
pub struct FrameLabD3D12Fg {
    _opaque: [u8; 0],
}

/// Optional, called synchronously during create. It must not unwind or re-enter FrameLab.
pub type FrameLabD3D12InitReportFn =
    Option<unsafe extern "C" fn(user: *mut c_void, operation: *const c_char, ngx_result: u32)>;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FrameLabD3D12FgConfig {
    pub struct_size: u32,
    pub abi_version: u32,
    pub device_id: u64,
    pub queue_id: u64,
    pub device: *mut ID3D12Device,
    pub queue: *mut ID3D12CommandQueue,
    pub completion_fence: *mut ID3D12Fence,
    pub extent: FrameLabExtent,
    pub project_id: *const c_char,
    pub engine_version: *const c_char,
    pub ngx_runtime_directory: *const WChar,
    pub log_directory: *const WChar,
    pub report: FrameLabD3D12InitReportFn,
    pub report_user: *mut c_void,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FrameLabD3D12FgImage {
    pub resource_id: u64,
    pub resource: *mut ID3D12Resource,
    /// Producer-tracked `D3D12_RESOURCE_STATES`.
    pub state: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FrameLabD3D12FgBindings {
    pub struct_size: u32,
    pub abi_version: u32,
    pub command_stream_id: u64,
    pub commands: *mut ID3D12GraphicsCommandList,
    pub color: FrameLabD3D12FgImage,
    pub depth: FrameLabD3D12FgImage,
    pub motion_vectors: FrameLabD3D12FgImage,
    pub generated: FrameLabD3D12FgImage,
}

framelab_exports! {
    /// The D3D12 FG exports, resolved at run time independently of SR.
    table D3D12FgApi;
    fn framelab_d3d12_fg_create(
        config: *const FrameLabD3D12FgConfig,
        out_fg: *mut *mut FrameLabD3D12Fg,
    ) -> FrameLabStatus;
    fn framelab_d3d12_fg_destroy(fg: *mut FrameLabD3D12Fg) -> FrameLabStatus;
    fn framelab_d3d12_fg_record_initialize(
        fg: *mut FrameLabD3D12Fg,
        commands: *mut ID3D12GraphicsCommandList,
    ) -> FrameLabStatus;
    fn framelab_d3d12_fg_record(
        fg: *mut FrameLabD3D12Fg,
        frame: *const FrameLabFgFrame,
        bindings: *const FrameLabD3D12FgBindings,
    ) -> FrameLabStatus;
    fn framelab_d3d12_fg_mark_submitted(fg: *mut FrameLabD3D12Fg, value: u64) -> FrameLabStatus;
    fn framelab_d3d12_fg_cancel(fg: *mut FrameLabD3D12Fg) -> FrameLabStatus;
    fn framelab_d3d12_fg_retire(fg: *mut FrameLabD3D12Fg) -> FrameLabStatus;
    fn framelab_d3d12_fg_invalidate(fg: *mut FrameLabD3D12Fg) -> FrameLabStatus;
    fn framelab_d3d12_fg_last_ngx_result(fg: *const FrameLabD3D12Fg) -> u32;
}
