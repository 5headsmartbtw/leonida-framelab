// SPDX-License-Identifier: MIT
// Runtime-loader tests. GPU-free: they load the installed shared FrameLab library and the stand-in
// fixtures built by tests/external-consumer (loader_fixture.c).
//   FRAMELAB_LIBRARY: absolute path of the installed shared library.
//   FRAMELAB_LOADER_FIXTURES: absolute path of the consumer build's loader-fixtures directory.
#![cfg(feature = "runtime")]

use framelab_sys::runtime::{Library, LoadError};
use framelab_sys::*;
use std::ffi::{c_void, CStr};
use std::path::{Path, PathBuf};
use std::ptr;

fn fixture(name: &str) -> PathBuf {
    let dir = std::env::var_os("FRAMELAB_LOADER_FIXTURES").expect("set FRAMELAB_LOADER_FIXTURES");
    Path::new(&dir).join(format!("{name}{}", std::env::consts::DLL_SUFFIX))
}

fn installed_library() -> PathBuf {
    PathBuf::from(std::env::var_os("FRAMELAB_LIBRARY").expect("set FRAMELAB_LIBRARY"))
}

fn open(path: &Path) -> Result<Library, LoadError> {
    // SAFETY: every path is the installed FrameLab or a fixture from loader_fixture.c.
    unsafe { Library::open(path) }
}

/// Whether the OS has the module loaded, without loading it.
fn is_loaded(path: &Path) -> bool {
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        extern "system" {
            fn GetModuleHandleW(name: *const u16) -> *mut c_void;
        }
        let wide: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        // SAFETY: NUL-terminated; no reference count is taken.
        !unsafe { GetModuleHandleW(wide.as_ptr()) }.is_null()
    }
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        #[cfg_attr(all(target_os = "linux", target_env = "gnu"), link(name = "dl"))]
        extern "C" {
            fn dlopen(file: *const std::ffi::c_char, mode: i32) -> *mut c_void;
            fn dlclose(handle: *mut c_void) -> i32;
        }
        const RTLD_NOW: i32 = 2;
        #[cfg(target_os = "macos")]
        const RTLD_NOLOAD: i32 = 0x10;
        #[cfg(not(target_os = "macos"))]
        const RTLD_NOLOAD: i32 = 4;
        let name = std::ffi::CString::new(path.as_os_str().as_bytes()).unwrap();
        // SAFETY: RTLD_NOLOAD never loads; a returned handle is released immediately.
        unsafe {
            let handle = dlopen(name.as_ptr(), RTLD_NOW | RTLD_NOLOAD);
            if handle.is_null() {
                return false;
            }
            dlclose(handle);
        }
        true
    }
}

/// A private copy, so no other test holds the same module.
fn private_copy(name: &str, test: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("framelab-sys-{}-{test}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join(fixture(name).file_name().unwrap());
    std::fs::copy(fixture(name), &path).unwrap();
    path
}

#[test]
fn installed_library_loads_and_runs_a_lease_lifecycle() {
    let library = open(&installed_library()).expect("installed FrameLab loads");
    let core = library.core();
    unsafe extern "C" fn on_release(user: *mut c_void) {
        // SAFETY: the test keeps the counter alive until after the callback.
        unsafe { *user.cast::<u32>() += 1 };
    }
    let mut released = 0u32;
    let mut lease = ptr::null_mut();
    let mut stream = ptr::null_mut();
    // SAFETY: all pointers are to initialized, in-scope storage; no GPU work is involved.
    unsafe {
        assert_eq!((core.framelab_abi_version)(), FRAMELAB_ABI_VERSION);
        assert!(!(core.framelab_status_string)(FRAMELAB_OK).is_null());
        assert_eq!((core.framelab_fg_stream_create)(0, &mut stream), FRAMELAB_ABI_MISMATCH);
        assert_eq!((core.framelab_fg_stream_create)(FRAMELAB_ABI_VERSION, &mut stream), FRAMELAB_OK);
        assert_eq!((core.framelab_fg_mark_submitted)(stream), FRAMELAB_INVALID_PHASE);
        assert_eq!((core.framelab_fg_stream_destroy)(stream), FRAMELAB_OK);
        assert_eq!(
            (core.framelab_lease_create)(ptr::addr_of_mut!(released).cast(), Some(on_release), &mut lease),
            FRAMELAB_OK
        );
        (core.framelab_lease_destroy)(lease);
    }
    assert_eq!(released, 1);
}

#[cfg(feature = "link")]
#[test]
fn linked_and_runtime_loaded_apis_coexist() {
    let library = open(&installed_library()).expect("installed FrameLab loads");
    // SAFETY: both calls have no preconditions.
    unsafe {
        assert_eq!(framelab_abi_version(), (library.core().framelab_abi_version)());
        let linked = CStr::from_ptr(framelab_status_string(FRAMELAB_BUSY));
        let loaded = CStr::from_ptr((library.core().framelab_status_string)(FRAMELAB_BUSY));
        assert_eq!(linked, loaded);
    }
}

#[test]
fn fixture_with_the_core_exports_loads() {
    let library = open(&fixture("fixture_core")).expect("fixture loads");
    // SAFETY: the fixture's status string is static.
    let text = unsafe { CStr::from_ptr((library.core().framelab_status_string)(0)) };
    assert_eq!(text.to_str(), Ok("fixture"));
}

#[test]
fn missing_library_is_reported() {
    let path = fixture("fixture_that_does_not_exist");
    match open(&path) {
        Err(LoadError::Open { path: reported, .. }) => assert_eq!(reported, path),
        other => panic!("expected Open error, got {:?}", other.err()),
    }
}

#[test]
fn a_file_that_is_not_a_library_is_reported() {
    let dir = std::env::temp_dir().join(format!("framelab-sys-{}-notalib", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join(format!("not_a_library{}", std::env::consts::DLL_SUFFIX));
    std::fs::write(&path, b"not a shared library").unwrap();
    assert!(matches!(open(&path), Err(LoadError::Open { .. })));
}

#[test]
fn relative_paths_are_rejected_without_searching() {
    let name = Path::new(framelab_sys::runtime::LIBRARY_FILE_NAME);
    assert_eq!(open(name).err(), Some(LoadError::InvalidPath(name.to_path_buf())));
}

#[test]
fn wrong_abi_is_rejected_before_other_exports() {
    assert_eq!(
        open(&fixture("fixture_wrong_abi")).err(),
        Some(LoadError::AbiMismatch { expected: FRAMELAB_ABI_VERSION, found: 2 })
    );
}

#[test]
fn missing_core_export_is_named() {
    assert_eq!(open(&fixture("fixture_missing_core")).err(), Some(LoadError::MissingExport("framelab_fg_invalidate")));
}

#[test]
fn dropping_the_library_unloads_the_module() {
    let path = private_copy("fixture_core", "unload");
    assert!(!is_loaded(&path));
    let library = open(&path).expect("fixture loads");
    assert!(is_loaded(&path));
    drop(library);
    assert!(!is_loaded(&path), "module still loaded after the last reference was dropped");
    // A second load after unloading works.
    drop(open(&path).expect("fixture loads again"));
    assert!(!is_loaded(&path));
}

#[cfg(all(windows, target_arch = "x86_64", feature = "d3d12-sr", feature = "d3d12-fg"))]
mod d3d12 {
    use super::*;

    fn availability(name: &str) -> (Result<(), LoadError>, Result<(), LoadError>) {
        let library = open(&fixture(name)).expect("fixture loads");
        (library.d3d12_sr().map(drop), library.d3d12_fg().map(drop))
    }

    #[test]
    fn components_are_available_independently() {
        let absent_sr = Err(LoadError::ComponentAbsent("d3d12-sr"));
        let absent_fg = Err(LoadError::ComponentAbsent("d3d12-fg"));
        assert_eq!(availability("fixture_core"), (absent_sr.clone(), absent_fg.clone()));
        assert_eq!(availability("fixture_sr"), (Ok(()), absent_fg));
        assert_eq!(availability("fixture_fg"), (absent_sr, Ok(())));
        assert_eq!(availability("fixture_sr_fg"), (Ok(()), Ok(())));
        assert_eq!(
            availability("fixture_sr_partial"),
            (Err(LoadError::MissingExport("framelab_d3d12_sr_last_ngx_result")), Ok(()))
        );
    }

    #[test]
    fn installed_library_without_components_reports_them_absent() {
        // CI builds the SDK without the D3D12 components (they need the external DLSS SDK).
        if std::env::var_os("FRAMELAB_EXPECT_D3D12_COMPONENTS").is_some() {
            let library = open(&installed_library()).unwrap();
            assert!(library.d3d12_sr().is_ok() && library.d3d12_fg().is_ok());
            return;
        }
        let library = open(&installed_library()).unwrap();
        assert_eq!(library.d3d12_sr().err(), Some(LoadError::ComponentAbsent("d3d12-sr")));
        assert_eq!(library.d3d12_fg().err(), Some(LoadError::ComponentAbsent("d3d12-fg")));
    }

    #[test]
    fn resolved_component_calls_the_library() {
        let library = open(&fixture("fixture_sr_fg")).unwrap();
        let sr = library.d3d12_sr().unwrap();
        let fg = library.d3d12_fg().unwrap();
        let mut sr_handle = 1usize as *mut framelab_sys::d3d12_sr::FrameLabD3D12Sr;
        let mut fg_handle = 1usize as *mut framelab_sys::d3d12_fg::FrameLabD3D12Fg;
        // SAFETY: the fixture ignores the configuration and only clears the output handle.
        unsafe {
            assert_eq!((sr.framelab_d3d12_sr_create)(ptr::null(), &mut sr_handle), FRAMELAB_UNSUPPORTED);
            assert_eq!((fg.framelab_d3d12_fg_create)(ptr::null(), &mut fg_handle), FRAMELAB_UNSUPPORTED);
        }
        assert!(sr_handle.is_null() && fg_handle.is_null());
    }

    #[test]
    fn a_component_keeps_the_module_loaded() {
        let path = private_copy("fixture_sr_fg", "component-lifetime");
        let library = open(&path).unwrap();
        let sr = library.d3d12_sr().unwrap();
        drop(library);
        assert!(is_loaded(&path), "SR table must keep the module loaded");
        // SAFETY: the fixture's function has no preconditions.
        assert_eq!(unsafe { (sr.framelab_d3d12_sr_last_ngx_result)(ptr::null()) }, 0);
        drop(sr);
        assert!(!is_loaded(&path));
    }
}
