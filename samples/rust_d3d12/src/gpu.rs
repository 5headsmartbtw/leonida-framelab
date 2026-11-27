// SPDX-License-Identifier: MIT
//! A minimal D3D12 host: one direct queue, one allocator and list, one completion fence, committed
//! textures, uploads and readbacks. Adapted from the maintainer's standalone Rust SR prototype
//! (FrameLab #43).

use crate::{require, Result};
use framelab_sys::FrameLabExtent;
use std::ffi::{c_void, CStr};
use std::io::Write;
use std::mem::{size_of, ManuallyDrop};
use std::path::Path;
use std::ptr::{copy_nonoverlapping, null_mut};
use std::slice;
use std::sync::Arc;
use windows::core::{Interface, PCWSTR};
use windows::Win32::Foundation::{CloseHandle, HANDLE, WAIT_OBJECT_0};
use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_12_0;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT, DXGI_FORMAT_UNKNOWN, DXGI_SAMPLE_DESC};
use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory2, IDXGIFactory4, DXGI_CREATE_FACTORY_FLAGS};
use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};

pub struct Texture {
    pub resource: Arc<ID3D12Resource>,
    pub state: D3D12_RESOURCE_STATES,
}

impl Texture {
    /// The raw interface pointer FrameLab's bindings take. FrameLab adds no COM reference.
    pub fn raw<T>(&self) -> *mut T {
        self.resource.as_raw().cast()
    }
}

pub struct Gpu {
    pub device: ID3D12Device,
    pub queue: ID3D12CommandQueue,
    pub fence: ID3D12Fence,
    pub list: ID3D12GraphicsCommandList,
    allocator: ID3D12CommandAllocator,
    messages: Option<ID3D12InfoQueue>,
    event: HANDLE,
    recording: bool,
    pending: bool,
    value: u64,
    transfers: Vec<ID3D12Resource>,
}

/// Holds the queue before the next submission until `open`; opens on drop so an early return
/// cannot strand the queue.
pub struct Gate(ID3D12Fence);

impl Gate {
    pub fn open(&self) -> Result<()> {
        unsafe { self.0.Signal(1)? };
        Ok(())
    }
}

impl Drop for Gate {
    fn drop(&mut self) {
        let _ = unsafe { self.0.Signal(1) };
    }
}

#[derive(Clone, Copy)]
struct Footprint {
    layout: D3D12_PLACED_SUBRESOURCE_FOOTPRINT,
    size: usize,
    row_bytes: usize,
    rows: usize,
}

impl Footprint {
    fn row_offset(&self, row: usize) -> usize {
        self.layout.Offset as usize + row * self.layout.Footprint.RowPitch as usize
    }
}

pub struct Readback {
    resource: ID3D12Resource,
    footprint: Footprint,
}

impl Readback {
    /// Texels of a four-byte format, row by row. Map only after the copy's fence completed.
    pub fn texels(&self) -> Result<Vec<u32>> {
        let mut mapped = null_mut();
        let range = D3D12_RANGE {
            Begin: 0,
            End: self.footprint.size,
        };
        unsafe { self.resource.Map(0, Some(&range), Some(&mut mapped))? };
        let mut result = Vec::with_capacity(self.footprint.rows * self.footprint.row_bytes / 4);
        for row in 0..self.footprint.rows {
            let data = unsafe {
                slice::from_raw_parts(
                    mapped.cast::<u8>().add(self.footprint.row_offset(row)),
                    self.footprint.row_bytes,
                )
            };
            result.extend(
                data.chunks_exact(4)
                    .map(|t| u32::from_le_bytes([t[0], t[1], t[2], t[3]])),
            );
        }
        let no_writes = D3D12_RANGE::default();
        unsafe { self.resource.Unmap(0, Some(&no_writes)) };
        Ok(result)
    }
}

/// Debug-layer message counts per severity.
#[derive(Default)]
pub struct Messages {
    pub corruption: u64,
    pub error: u64,
    pub warning: u64,
    pub info: u64,
    pub message: u64,
    pub discarded: u64,
}

impl Gpu {
    /// Creates a device on the first NVIDIA hardware adapter. With `validate`, the D3D12 debug
    /// layer, GPU-based validation and synchronized command-queue validation are enabled first,
    /// and every message is kept for [`Gpu::write_messages`].
    pub fn new(validate: bool) -> Result<Self> {
        if validate {
            let mut debug = None::<ID3D12Debug1>;
            unsafe { D3D12GetDebugInterface(&mut debug)? };
            let debug = debug.ok_or("D3D12 debug interface unavailable (install Graphics Tools)")?;
            unsafe {
                debug.EnableDebugLayer();
                debug.SetEnableGPUBasedValidation(true);
                debug.SetEnableSynchronizedCommandQueueValidation(true);
            }
        }
        let factory: IDXGIFactory4 = unsafe { CreateDXGIFactory2(DXGI_CREATE_FACTORY_FLAGS(0))? };
        let mut device = None::<ID3D12Device>;
        for index in 0.. {
            let Ok(adapter) = (unsafe { factory.EnumAdapters1(index) }) else {
                break;
            };
            let info = unsafe { adapter.GetDesc1()? };
            if info.VendorId == 0x10de
                && unsafe { D3D12CreateDevice(&adapter, D3D_FEATURE_LEVEL_12_0, &mut device) }.is_ok()
            {
                let end = info
                    .Description
                    .iter()
                    .position(|c| *c == 0)
                    .unwrap_or(info.Description.len());
                println!("GPU {}", String::from_utf16_lossy(&info.Description[..end]));
                break;
            }
        }
        let device = device.ok_or("no NVIDIA D3D12 hardware adapter")?;
        let messages = if validate {
            let queue: ID3D12InfoQueue = device.cast()?;
            unsafe { queue.SetMessageCountLimit(u64::MAX)? };
            Some(queue)
        } else {
            None
        };
        let desc = D3D12_COMMAND_QUEUE_DESC {
            Type: D3D12_COMMAND_LIST_TYPE_DIRECT,
            ..Default::default()
        };
        let queue = unsafe { device.CreateCommandQueue(&desc)? };
        let allocator: ID3D12CommandAllocator = unsafe { device.CreateCommandAllocator(desc.Type)? };
        let list: ID3D12GraphicsCommandList =
            unsafe { device.CreateCommandList(0, desc.Type, &allocator, None::<&ID3D12PipelineState>)? };
        unsafe { list.Close()? };
        let fence = unsafe { device.CreateFence(0, D3D12_FENCE_FLAG_NONE)? };
        let event = unsafe { CreateEventW(None, false, false, PCWSTR::null())? };
        Ok(Self {
            device,
            queue,
            fence,
            list,
            allocator,
            messages,
            event,
            recording: false,
            pending: false,
            value: 0,
            transfers: Vec::new(),
        })
    }

    /// The fence value of the latest submission.
    pub fn value(&self) -> u64 {
        self.value
    }

    pub fn begin(&mut self) -> Result<()> {
        require(
            !self.pending && !self.recording,
            "attempt to reuse an allocator before completion",
        )?;
        self.transfers.clear();
        unsafe {
            self.allocator.Reset()?;
            self.list.Reset(&self.allocator, None::<&ID3D12PipelineState>)?;
        }
        self.recording = true;
        Ok(())
    }

    /// Closes the list without executing it.
    pub fn discard(&mut self) -> Result<()> {
        require(self.recording, "discard outside a recording")?;
        unsafe { self.list.Close()? };
        self.recording = false;
        Ok(())
    }

    pub fn gate_next_submission(&self) -> Result<Gate> {
        let gate: ID3D12Fence = unsafe { self.device.CreateFence(0, D3D12_FENCE_FLAG_NONE)? };
        unsafe { self.queue.Wait(&gate, 1)? };
        Ok(Gate(gate))
    }

    /// Executes the list and queues `Signal(value)` on the completion fence; returns the value.
    pub fn submit(&mut self) -> Result<u64> {
        require(
            self.recording && !self.pending,
            "submit requires an active list and idle queue",
        )?;
        unsafe { self.list.Close()? };
        self.recording = false;
        let command: ID3D12CommandList = self.list.cast()?;
        self.value += 1;
        self.pending = true;
        unsafe {
            self.queue.ExecuteCommandLists(&[Some(command)]);
            if let Err(error) = self.queue.Signal(&self.fence, self.value) {
                eprintln!("Failed to signal after ExecuteCommandLists: {error}");
                std::process::abort();
            }
        }
        Ok(self.value)
    }

    pub fn wait(&mut self) -> Result<()> {
        require(self.pending, "wait has no pending submission")?;
        unsafe {
            self.fence.SetEventOnCompletion(self.value, self.event)?;
            require(
                WaitForSingleObject(self.event, 30_000) == WAIT_OBJECT_0,
                "D3D12 fence timeout",
            )?;
            self.device.GetDeviceRemovedReason()?;
            require(
                self.fence.GetCompletedValue() >= self.value,
                "fence event arrived before signal",
            )?;
        }
        self.pending = false;
        self.transfers.clear();
        Ok(())
    }

    /// A committed 2D texture in COPY_DEST, with a UAV flag when `writable`.
    pub fn texture(&self, extent: FrameLabExtent, format: DXGI_FORMAT, writable: bool) -> Result<Texture> {
        let desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            Width: extent.width.into(),
            Height: extent.height,
            DepthOrArraySize: 1,
            MipLevels: 1,
            Format: format,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Layout: D3D12_TEXTURE_LAYOUT_UNKNOWN,
            Flags: if writable {
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
            } else {
                D3D12_RESOURCE_FLAG_NONE
            },
            ..Default::default()
        };
        let state = D3D12_RESOURCE_STATE_COPY_DEST;
        let resource = Arc::new(self.allocate(&desc, D3D12_HEAP_TYPE_DEFAULT, state)?);
        Ok(Texture { resource, state })
    }

    fn allocate(
        &self,
        desc: &D3D12_RESOURCE_DESC,
        kind: D3D12_HEAP_TYPE,
        state: D3D12_RESOURCE_STATES,
    ) -> Result<ID3D12Resource> {
        let heap = D3D12_HEAP_PROPERTIES {
            Type: kind,
            CreationNodeMask: 1,
            VisibleNodeMask: 1,
            ..Default::default()
        };
        let mut resource = None;
        unsafe {
            self.device
                .CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, desc, state, None, &mut resource)?
        };
        Ok(resource.ok_or("D3D12 resource creation returned no resource")?)
    }

    fn buffer(&self, size: usize, kind: D3D12_HEAP_TYPE, state: D3D12_RESOURCE_STATES) -> Result<ID3D12Resource> {
        let desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
            Width: size as u64,
            Height: 1,
            DepthOrArraySize: 1,
            MipLevels: 1,
            Format: DXGI_FORMAT_UNKNOWN,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            ..Default::default()
        };
        self.allocate(&desc, kind, state)
    }

    fn footprint(&self, texture: &Texture) -> Footprint {
        let mut layout = D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default();
        let (mut size, mut row_bytes, mut rows) = (0, 0, 0);
        unsafe {
            self.device.GetCopyableFootprints(
                &texture.resource.GetDesc(),
                0,
                1,
                0,
                Some(&mut layout),
                Some(&mut rows),
                Some(&mut row_bytes),
                Some(&mut size),
            )
        };
        Footprint {
            layout,
            size: size as usize,
            row_bytes: row_bytes as usize,
            rows: rows as usize,
        }
    }

    pub fn transition(&self, texture: &mut Texture, state: D3D12_RESOURCE_STATES) {
        if texture.state == state {
            return;
        }
        let mut barrier = D3D12_RESOURCE_BARRIER {
            Type: D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
            Anonymous: D3D12_RESOURCE_BARRIER_0 {
                Transition: ManuallyDrop::new(D3D12_RESOURCE_TRANSITION_BARRIER {
                    pResource: ManuallyDrop::new(Some(texture.resource.as_ref().clone())),
                    Subresource: D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    StateBefore: texture.state,
                    StateAfter: state,
                }),
            },
        };
        unsafe {
            self.list.ResourceBarrier(slice::from_ref(&barrier));
            ManuallyDrop::drop(&mut (*barrier.Anonymous.Transition).pResource);
        }
        texture.state = state;
    }

    /// Orders the generated image's UAV writes before a following copy.
    fn uav_barrier(&self, texture: &Texture) {
        let mut barrier = D3D12_RESOURCE_BARRIER {
            Type: D3D12_RESOURCE_BARRIER_TYPE_UAV,
            Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
            Anonymous: D3D12_RESOURCE_BARRIER_0 {
                UAV: ManuallyDrop::new(D3D12_RESOURCE_UAV_BARRIER {
                    pResource: ManuallyDrop::new(Some(texture.resource.as_ref().clone())),
                }),
            },
        };
        unsafe {
            self.list.ResourceBarrier(slice::from_ref(&barrier));
            ManuallyDrop::drop(&mut (*barrier.Anonymous.UAV).pResource);
        }
    }

    fn copy(&self, texture: &Texture, buffer: &ID3D12Resource, layout: Footprint, upload: bool) {
        let mut image_location = D3D12_TEXTURE_COPY_LOCATION {
            pResource: ManuallyDrop::new(Some(texture.resource.as_ref().clone())),
            Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 },
        };
        let mut buffer_location = D3D12_TEXTURE_COPY_LOCATION {
            pResource: ManuallyDrop::new(Some(buffer.clone())),
            Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                PlacedFootprint: layout.layout,
            },
        };
        let (destination, source) = if upload {
            (&image_location, &buffer_location)
        } else {
            (&buffer_location, &image_location)
        };
        unsafe {
            self.list.CopyTextureRegion(destination, 0, 0, 0, source, None);
            ManuallyDrop::drop(&mut image_location.pResource);
            ManuallyDrop::drop(&mut buffer_location.pResource);
        }
    }

    /// Records an upload of tightly packed rows and leaves the texture in `final_state`.
    pub fn upload(&mut self, texture: &mut Texture, data: &[u8], final_state: D3D12_RESOURCE_STATES) -> Result<()> {
        let footprint = self.footprint(texture);
        require(
            data.len() == footprint.rows * footprint.row_bytes,
            "texture upload length",
        )?;
        let upload = self.buffer(
            footprint.size,
            D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ,
        )?;
        let mut mapped: *mut c_void = null_mut();
        let no_reads = D3D12_RANGE::default();
        unsafe {
            upload.Map(0, Some(&no_reads), Some(&mut mapped))?;
            for (row, data_row) in data.chunks_exact(footprint.row_bytes).enumerate() {
                copy_nonoverlapping(
                    data_row.as_ptr(),
                    mapped.cast::<u8>().add(footprint.row_offset(row)),
                    footprint.row_bytes,
                );
            }
            upload.Unmap(0, None);
        }
        self.transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
        self.copy(texture, &upload, footprint, true);
        self.transition(texture, final_state);
        self.transfers.push(upload);
        Ok(())
    }

    /// Records a copy of the texture into a readback buffer, after a UAV barrier.
    pub fn readback(&mut self, texture: &mut Texture) -> Result<Readback> {
        let footprint = self.footprint(texture);
        let resource = self.buffer(footprint.size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST)?;
        if texture.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS {
            self.uav_barrier(texture);
        }
        self.transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
        self.copy(texture, &resource, footprint, false);
        self.transfers.push(resource.clone());
        Ok(Readback { resource, footprint })
    }

    /// Writes every stored debug-layer message to `path` (severity, ID, text) and prints a count
    /// per severity. FrameLab and NGX share the device, so a message may come from either.
    /// Without the debug layer this does nothing and returns `None`.
    pub fn write_messages(&self, path: &Path) -> Result<Option<Messages>> {
        let Some(queue) = &self.messages else {
            return Ok(None);
        };
        let mut file = std::fs::File::create(path)?;
        let mut counts = Messages::default();
        unsafe {
            for index in 0..queue.GetNumStoredMessages() {
                let mut bytes = 0;
                queue.GetMessage(index, None, &mut bytes)?;
                let mut aligned = vec![0usize; bytes.div_ceil(size_of::<usize>())];
                let pointer = aligned.as_mut_ptr().cast::<D3D12_MESSAGE>();
                queue.GetMessage(index, Some(pointer), &mut bytes)?;
                let message = &*pointer;
                match message.Severity {
                    D3D12_MESSAGE_SEVERITY_CORRUPTION => counts.corruption += 1,
                    D3D12_MESSAGE_SEVERITY_ERROR => counts.error += 1,
                    D3D12_MESSAGE_SEVERITY_WARNING => counts.warning += 1,
                    D3D12_MESSAGE_SEVERITY_INFO => counts.info += 1,
                    _ => counts.message += 1,
                }
                let text = CStr::from_ptr(message.pDescription.cast()).to_string_lossy();
                writeln!(file, "{} {} {text}", message.Severity.0, message.ID.0)?;
            }
            counts.discarded = queue.GetNumMessagesDiscardedByMessageCountLimit();
            queue.ClearStoredMessages();
        }
        println!(
            "RUST_D3D12_DEBUG corruption={} error={} warning={} info={} message={} discarded={}",
            counts.corruption, counts.error, counts.warning, counts.info, counts.message, counts.discarded
        );
        Ok(Some(counts))
    }
}

impl Drop for Gpu {
    fn drop(&mut self) {
        if self.pending && self.wait().is_err() {
            eprintln!("Cannot prove GPU completion. Aborting the test process.");
            std::process::abort();
        }
        let _ = unsafe { CloseHandle(self.event) };
    }
}
