// SPDX-License-Identifier: MIT
//! Opaque D3D12 COM interface types shared by the D3D12 SR and FG bindings.
//!
//! The bindings take raw COM interface pointers and do not depend on a `windows` crate version.
//! Pass the raw pointer of a live interface, for example `device.as_raw().cast()` with
//! windows-rs. FrameLab does not take a COM reference: the host keeps every interface alive until
//! the handle that uses it is destroyed, and every resource alive until the frame that names it is
//! retired (or until its lease's final release, for SR images).

use core::marker::{PhantomData, PhantomPinned};

macro_rules! com_interfaces {
    ($($name:ident),+ $(,)?) => {$(
        #[doc = concat!("Opaque `", stringify!($name), "` COM interface. Only used behind a raw pointer.")]
        #[repr(C)]
        pub struct $name {
            _opaque: [u8; 0],
            _marker: PhantomData<(*mut u8, PhantomPinned)>,
        }
    )+};
}

com_interfaces!(ID3D12Device, ID3D12CommandQueue, ID3D12Fence, ID3D12Resource, ID3D12GraphicsCommandList);

/// C `wchar_t` on Windows: a UTF-16 code unit. Strings are NUL-terminated.
pub type WChar = u16;
