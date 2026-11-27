// SPDX-License-Identifier: MIT
//! FrameLab-facing host pieces shared by the SR and FG consumers: runtime loading, status checks,
//! resource leases, the NGX runtime search path and the command line.

use crate::{require, Result};
use framelab_sys::runtime::Library;
use framelab_sys::{CoreApi, FrameLabLease, FrameLabStatus, FRAMELAB_OK};
use std::ffi::{c_char, c_void, CStr};
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::ptr::{null_mut, NonNull};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use windows::core::PCWSTR;
use windows::Win32::Graphics::Direct3D12::ID3D12Resource;
use windows::Win32::System::LibraryLoader::{
    AddDllDirectory, RemoveDllDirectory, SetDefaultDllDirectories, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
    LOAD_LIBRARY_SEARCH_USER_DIRS,
};

/// Exit code when FrameLab or the component is unavailable and the host rendered without it.
pub const EXIT_FALLBACK: i32 = 3;

pub struct Args {
    pub library: PathBuf,
    pub runtime: PathBuf,
    pub logs: PathBuf,
    pub debug_layer: bool,
}

impl Args {
    /// `<FrameLab.dll> <NGX runtime directory> <evidence directory> [--debug-layer]`, absolute paths.
    pub fn parse(program: &str) -> Result<Self> {
        let args: Vec<_> = std::env::args_os().skip(1).collect();
        let usage = format!(
            "usage: {program} <absolute FrameLab.dll> <absolute NGX runtime directory> <absolute evidence directory> [--debug-layer]"
        );
        require(
            args.len() == 3 || (args.len() == 4 && args[3] == "--debug-layer"),
            usage.clone(),
        )?;
        let [library, runtime, logs] = [&args[0], &args[1], &args[2]].map(PathBuf::from);
        require(
            library.is_absolute() && runtime.is_absolute() && logs.is_absolute(),
            usage,
        )?;
        require(runtime.is_dir(), "the NGX runtime directory does not exist")?;
        std::fs::create_dir_all(&logs)?;
        Ok(Self {
            library,
            runtime,
            logs,
            debug_layer: args.len() == 4,
        })
    }
}

/// Loads FrameLab from the given absolute path. The host treats every error as "render without
/// FrameLab": it prints why and continues.
pub fn open(path: &Path) -> std::result::Result<Library, String> {
    // SAFETY: the path names the FrameLab build installed for this consumer.
    match unsafe { Library::open(path) } {
        Ok(library) => {
            let core = library.core();
            // SAFETY: no preconditions.
            println!(
                "FRAMELAB_LOADED abi={} path={}",
                unsafe { (core.framelab_abi_version)() },
                path.display()
            );
            Ok(library)
        }
        Err(error) => Err(error.to_string()),
    }
}

pub fn wide(path: &Path) -> Vec<u16> {
    path.as_os_str().encode_wide().chain([0]).collect()
}

pub fn status_text(core: &CoreApi, status: FrameLabStatus) -> String {
    // SAFETY: FrameLab returns a static NUL-terminated string for every value.
    unsafe { CStr::from_ptr((core.framelab_status_string)(status)) }
        .to_string_lossy()
        .into_owned()
}

/// Fails unless `status` is `want`, naming both.
pub fn expect(core: &CoreApi, status: FrameLabStatus, want: FrameLabStatus, operation: &str) -> Result<()> {
    if status == want {
        return Ok(());
    }
    Err(format!(
        "{operation}: got {} ({status}), expected {} ({want})",
        status_text(core, status),
        status_text(core, want)
    )
    .into())
}

/// Adds the separately acquired NGX runtime directory to the DLL search path for this process.
pub struct DllDirectory(*mut c_void);

impl DllDirectory {
    pub fn add(path: &[u16]) -> Result<Self> {
        unsafe {
            SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS)?;
            let cookie = AddDllDirectory(PCWSTR(path.as_ptr()));
            require(!cookie.is_null(), "cannot add the NGX runtime directory")?;
            Ok(Self(cookie))
        }
    }
}

impl Drop for DllDirectory {
    fn drop(&mut self) {
        let _ = unsafe { RemoveDllDirectory(self.0) };
    }
}

/// NGX result reporter for create. It must not unwind into FrameLab.
///
/// # Safety
/// Only FrameLab calls it, with a NUL-terminated `operation` valid for the call.
pub unsafe extern "C" fn report(_: *mut c_void, operation: *const c_char, result: u32) {
    let _ = std::panic::catch_unwind(|| {
        // SAFETY: FrameLab passes a NUL-terminated operation name valid for the call.
        let name = unsafe { CStr::from_ptr(operation) }.to_string_lossy();
        println!("NGX {name} {result:#x}");
    });
}

struct LeasePayload {
    _resource: Arc<ID3D12Resource>,
    released: Arc<AtomicUsize>,
}

unsafe extern "C" fn release_owner(user: *mut c_void) {
    // Runs once, possibly after framelab_lease_destroy, when FrameLab drops its last reference.
    let _ = std::panic::catch_unwind(|| {
        // SAFETY: `user` is the payload leaked in `Lease::new`; FrameLab calls this exactly once.
        let payload = unsafe { Box::from_raw(user.cast::<LeasePayload>()) };
        payload.released.fetch_add(1, Ordering::SeqCst);
    });
}

/// A FrameLab lease that owns one reference to a resource until FrameLab's final release.
/// The `Library` that provided `core` must outlive every lease and its release callback.
pub struct Lease {
    core: CoreApi,
    handle: NonNull<FrameLabLease>,
}

impl Lease {
    pub fn new(core: &CoreApi, resource: &Arc<ID3D12Resource>, released: &Arc<AtomicUsize>) -> Result<Self> {
        let payload = Box::into_raw(Box::new(LeasePayload {
            _resource: Arc::clone(resource),
            released: Arc::clone(released),
        }));
        let mut lease = null_mut();
        // SAFETY: the callback and payload follow the lease contract above.
        let status = unsafe { (core.framelab_lease_create)(payload.cast(), Some(release_owner), &mut lease) };
        if status != FRAMELAB_OK {
            // SAFETY: FrameLab did not take ownership.
            drop(unsafe { Box::from_raw(payload) });
        }
        expect(core, status, FRAMELAB_OK, "create resource lease")?;
        Ok(Self {
            core: *core,
            handle: NonNull::new(lease).ok_or("lease create returned a null handle")?,
        })
    }

    pub fn as_ptr(&self) -> *const FrameLabLease {
        self.handle.as_ptr()
    }
}

impl Drop for Lease {
    fn drop(&mut self) {
        // SAFETY: the handle came from framelab_lease_create and is destroyed once.
        unsafe { (self.core.framelab_lease_destroy)(self.handle.as_ptr()) };
    }
}

/// Prints the fallback reason and the exit code the consumer returns after ordinary rendering.
pub fn fallback(component: &str, reason: &str) {
    println!("FRAMELAB_UNAVAILABLE component={component} reason={reason}");
}
