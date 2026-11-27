// SPDX-License-Identifier: MIT
//! Runtime loading of the shared FrameLab library (feature `runtime`).
//!
//! A build without the `link` feature neither links nor imports FrameLab: the host renderer starts
//! when the library is absent, has another ABI or lacks a component, and [`Library::open`] reports
//! why. The core ABI version is checked before any other export is used. The D3D12 SR and FG
//! components are resolved separately, so either can be available without the other.
//!
//! # Lifetime
//! [`Library`] and every [`Component`] share one reference to the loaded module. The module is
//! unloaded when the last of them is dropped. Keep one alive while any FrameLab object created
//! through it (stream, lease, SR or FG handle) exists, while recorded work is not retired and while
//! FrameLab may call a host callback. Dropping the last reference earlier leaves those objects
//! pointing into unmapped code.
//!
//! # Callbacks
//! Callbacks given to FrameLab (lease release, init and SR report functions) are `extern "C"` and
//! must not unwind. Catch panics inside them, for example with `std::panic::catch_unwind`.

use crate::{CoreApi, FRAMELAB_ABI_VERSION};
use core::ffi::c_void;
use core::fmt;
use std::path::{Path, PathBuf};
use std::sync::Arc;

/// File name of the shared library produced by the FrameLab CMake package on this target.
#[cfg(windows)]
pub const LIBRARY_FILE_NAME: &str = "FrameLab.dll";
/// File name of the shared library produced by the FrameLab CMake package on this target.
#[cfg(target_os = "macos")]
pub const LIBRARY_FILE_NAME: &str = "libFrameLab.dylib";
/// File name of the shared library produced by the FrameLab CMake package on this target.
#[cfg(all(unix, not(target_os = "macos")))]
pub const LIBRARY_FILE_NAME: &str = "libFrameLab.so";

/// Why FrameLab or one of its components cannot be used.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum LoadError {
    /// The path is not absolute or contains a NUL character. No search path is used.
    InvalidPath(PathBuf),
    /// The operating system could not load the library (absent, wrong architecture, missing
    /// dependency). `code` is the Windows error code, or 0 on Unix where `message` comes from dlerror.
    Open { path: PathBuf, code: u32, message: String },
    /// `framelab_abi_version` returned another version. No other export was used.
    AbiMismatch { expected: u32, found: u32 },
    /// A required export is missing: a core export, or one export of a partially exported component.
    MissingExport(&'static str),
    /// The library exports none of this component's functions; it was built without it.
    ComponentAbsent(&'static str),
}

impl fmt::Display for LoadError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidPath(path) => write!(f, "FrameLab path must be absolute without NUL: {}", path.display()),
            Self::Open { path, code, message } => {
                write!(f, "cannot load {} (error {code}", path.display())?;
                if !message.is_empty() {
                    write!(f, ": {message}")?;
                }
                f.write_str(")")
            }
            Self::AbiMismatch { expected, found } => {
                write!(f, "FrameLab ABI version {found} does not match the bindings' version {expected}")
            }
            Self::MissingExport(name) => write!(f, "FrameLab export {name} is missing"),
            Self::ComponentAbsent(name) => write!(f, "FrameLab was built without the {name} component"),
        }
    }
}

impl std::error::Error for LoadError {}

/// A loaded FrameLab library whose core ABI version and core exports were verified.
pub struct Library {
    module: Arc<Module>,
    core: CoreApi,
}

impl Library {
    /// Loads FrameLab from an absolute path, checks the ABI version, then resolves the core exports.
    ///
    /// On Windows, dependencies are searched in the library's directory and the system directories
    /// (`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`), not the current
    /// directory or `PATH`.
    ///
    /// # Safety
    /// Loading runs the library's initialisers, and the ABI check calls one of its exports. The path
    /// must name a FrameLab build for this target, or a library that is safe to load and whose
    /// FrameLab-named exports have the documented C signatures.
    pub unsafe fn open(path: impl AsRef<Path>) -> Result<Self, LoadError> {
        let module = Arc::new(Module::open(path.as_ref())?);
        let version = module.symbol("framelab_abi_version\0");
        if version.is_null() {
            return Err(LoadError::MissingExport("framelab_abi_version"));
        }
        // SAFETY: the export has the C signature `uint32_t (void)` (caller's contract).
        let version = unsafe { core::mem::transmute::<*mut c_void, crate::pfn::framelab_abi_version>(version) };
        // SAFETY: as above; the function has no preconditions.
        let found = unsafe { version() };
        if found != FRAMELAB_ABI_VERSION {
            return Err(LoadError::AbiMismatch { expected: FRAMELAB_ABI_VERSION, found });
        }
        let core = match CoreApi::resolve(&module) {
            Ok(Some(core)) => core,
            Ok(None) => unreachable!("framelab_abi_version was resolved"),
            Err(name) => return Err(LoadError::MissingExport(name)),
        };
        Ok(Self { module, core })
    }

    /// The core function table.
    pub fn core(&self) -> &CoreApi {
        &self.core
    }

    /// The D3D12 SR component, or why it is unavailable. Independent of FG availability.
    #[cfg(all(windows, target_arch = "x86_64", feature = "d3d12-sr"))]
    pub fn d3d12_sr(&self) -> Result<Component<crate::d3d12_sr::D3D12SrApi>, LoadError> {
        self.component("d3d12-sr", crate::d3d12_sr::D3D12SrApi::resolve)
    }

    /// The D3D12 FG component, or why it is unavailable. Independent of SR availability.
    #[cfg(all(windows, target_arch = "x86_64", feature = "d3d12-fg"))]
    pub fn d3d12_fg(&self) -> Result<Component<crate::d3d12_fg::D3D12FgApi>, LoadError> {
        self.component("d3d12-fg", crate::d3d12_fg::D3D12FgApi::resolve)
    }

    #[allow(dead_code)]
    fn component<T>(
        &self,
        name: &'static str,
        resolve: fn(&Module) -> Result<Option<T>, &'static str>,
    ) -> Result<Component<T>, LoadError> {
        match resolve(&self.module) {
            Ok(Some(api)) => Ok(Component { api, _module: Arc::clone(&self.module) }),
            Ok(None) => Err(LoadError::ComponentAbsent(name)),
            Err(missing) => Err(LoadError::MissingExport(missing)),
        }
    }
}

/// A component function table that keeps the module loaded while it exists.
pub struct Component<T> {
    api: T,
    _module: Arc<Module>,
}

impl<T> core::ops::Deref for Component<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.api
    }
}

/// An OS module handle, closed on drop.
pub(crate) struct Module {
    handle: *mut c_void,
}

// SAFETY: module handles and resolved symbols may be used and released from any thread.
unsafe impl Send for Module {}
// SAFETY: as above; `symbol` only reads.
unsafe impl Sync for Module {}

fn checked_path(path: &Path) -> Result<(), LoadError> {
    if path.is_absolute() && !path.as_os_str().to_string_lossy().contains('\0') {
        Ok(())
    } else {
        Err(LoadError::InvalidPath(path.to_path_buf()))
    }
}

#[cfg(windows)]
mod os {
    use core::ffi::{c_char, c_void};

    pub const LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR: u32 = 0x0000_0100;
    pub const LOAD_LIBRARY_SEARCH_DEFAULT_DIRS: u32 = 0x0000_1000;

    #[link(name = "kernel32")]
    extern "system" {
        pub fn LoadLibraryExW(name: *const u16, file: *mut c_void, flags: u32) -> *mut c_void;
        pub fn GetProcAddress(module: *mut c_void, name: *const c_char) -> *mut c_void;
        pub fn FreeLibrary(module: *mut c_void) -> i32;
        pub fn GetLastError() -> u32;
    }
}

#[cfg(windows)]
impl Module {
    fn open(path: &Path) -> Result<Self, LoadError> {
        use std::os::windows::ffi::OsStrExt;
        checked_path(path)?;
        let wide: Vec<u16> = path.as_os_str().encode_wide().chain(core::iter::once(0)).collect();
        let flags = os::LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | os::LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
        // SAFETY: `wide` is NUL-terminated; initialiser safety is the caller's contract (`Library::open`).
        let handle = unsafe { os::LoadLibraryExW(wide.as_ptr(), core::ptr::null_mut(), flags) };
        if handle.is_null() {
            // SAFETY: no preconditions.
            let code = unsafe { os::GetLastError() };
            return Err(LoadError::Open { path: path.to_path_buf(), code, message: String::new() });
        }
        Ok(Self { handle })
    }

    pub(crate) fn symbol(&self, name: &str) -> *mut c_void {
        debug_assert!(name.ends_with('\0'));
        // SAFETY: the handle is live and the name is NUL-terminated.
        unsafe { os::GetProcAddress(self.handle, name.as_ptr().cast()) }
    }
}

#[cfg(windows)]
impl Drop for Module {
    fn drop(&mut self) {
        // SAFETY: the handle came from LoadLibraryExW and is released once.
        unsafe { os::FreeLibrary(self.handle) };
    }
}

#[cfg(unix)]
mod os {
    use core::ffi::{c_char, c_int, c_void};

    pub const RTLD_NOW: c_int = 2;
    #[cfg(target_os = "macos")]
    pub const RTLD_LOCAL: c_int = 4;
    #[cfg(not(target_os = "macos"))]
    pub const RTLD_LOCAL: c_int = 0;

    #[cfg_attr(all(target_os = "linux", target_env = "gnu"), link(name = "dl"))]
    extern "C" {
        pub fn dlopen(file: *const c_char, mode: c_int) -> *mut c_void;
        pub fn dlsym(handle: *mut c_void, name: *const c_char) -> *mut c_void;
        pub fn dlclose(handle: *mut c_void) -> c_int;
        pub fn dlerror() -> *mut c_char;
    }
}

#[cfg(unix)]
impl Module {
    fn open(path: &Path) -> Result<Self, LoadError> {
        use std::os::unix::ffi::OsStrExt;
        checked_path(path)?;
        let name = std::ffi::CString::new(path.as_os_str().as_bytes())
            .map_err(|_| LoadError::InvalidPath(path.to_path_buf()))?;
        // SAFETY: `name` is NUL-terminated; initialiser safety is the caller's contract (`Library::open`).
        let handle = unsafe { os::dlopen(name.as_ptr(), os::RTLD_NOW | os::RTLD_LOCAL) };
        if handle.is_null() {
            // SAFETY: dlerror returns null or a NUL-terminated string valid until the next dl call.
            let message = unsafe {
                let text = os::dlerror();
                if text.is_null() {
                    String::new()
                } else {
                    std::ffi::CStr::from_ptr(text).to_string_lossy().into_owned()
                }
            };
            return Err(LoadError::Open { path: path.to_path_buf(), code: 0, message });
        }
        Ok(Self { handle })
    }

    pub(crate) fn symbol(&self, name: &str) -> *mut c_void {
        debug_assert!(name.ends_with('\0'));
        // SAFETY: the handle is live and the name is NUL-terminated.
        unsafe { os::dlsym(self.handle, name.as_ptr().cast()) }
    }
}

#[cfg(unix)]
impl Drop for Module {
    fn drop(&mut self) {
        // SAFETY: the handle came from dlopen and is released once.
        unsafe { os::dlclose(self.handle) };
    }
}
