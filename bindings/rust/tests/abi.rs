use framelab_sys::*;
#[cfg(feature = "link")]
use std::ffi::c_void;
use std::mem::{align_of, offset_of, size_of};
#[cfg(feature = "link")]
use std::ptr;

#[test]
fn layout_matches_compiled_c_header() {
    let native = std::env::var("FRAMELAB_ABI_LAYOUT").expect("run the C layout probe first");
    let native: Vec<usize> = native
        .split_whitespace()
        .map(|value| value.parse().expect("integer layout value"))
        .collect();
    let rust = [
        size_of::<FrameLabExtent>(), align_of::<FrameLabExtent>(),
        size_of::<FrameLabImage>(), align_of::<FrameLabImage>(), offset_of!(FrameLabImage, lease),
        size_of::<FrameLabCamera>(), align_of::<FrameLabCamera>(), offset_of!(FrameLabCamera, flags),
        size_of::<FrameLabProducerSync>(), align_of::<FrameLabProducerSync>(),
        offset_of!(FrameLabProducerSync, signal_lease),
        size_of::<FrameLabFgFrame>(), align_of::<FrameLabFgFrame>(),
        offset_of!(FrameLabFgFrame, color), offset_of!(FrameLabFgFrame, camera),
        offset_of!(FrameLabFgFrame, producer), offset_of!(FrameLabFgFrame, flags),
    ];
    assert_eq!(native, rust);
}

#[cfg(feature = "link")]
unsafe extern "C" fn on_release(user: *mut c_void) {
    // SAFETY: the test retains this counter until after the callback.
    unsafe { *user.cast::<u32>() += 1 };
}

#[cfg(feature = "link")]
#[test]
fn installed_library_retains_frame_lease_until_retirement() {
    let mut released: u32 = 0;
    let mut stream = ptr::null_mut();
    let mut lease = ptr::null_mut();
    // SAFETY: pointers refer to initialized, in-scope storage. This fixture has no GPU work.
    unsafe {
        assert_eq!(framelab_abi_version(), FRAMELAB_ABI_VERSION);
        assert_eq!(framelab_fg_stream_create(0, &mut stream), FRAMELAB_ABI_MISMATCH);
        assert_eq!(framelab_fg_stream_create(FRAMELAB_ABI_VERSION, &mut stream), FRAMELAB_OK);
        assert_eq!(framelab_lease_create(ptr::addr_of_mut!(released).cast(), Some(on_release), &mut lease), FRAMELAB_OK);

        // Every field is an integer, float, array, or nullable raw pointer.
        let mut frame: FrameLabFgFrame = std::mem::zeroed();
        frame.struct_size = size_of::<FrameLabFgFrame>() as u32;
        frame.abi_version = FRAMELAB_ABI_VERSION;
        frame.stream_id = 1;
        frame.device_id = 1;
        frame.generation = 1;
        frame.delta_seconds = 1.0 / 60.0;
        frame.reset_history = FRAMELAB_RESET_INITIAL;
        frame.render_extent = FrameLabExtent { width: 640, height: 360 };
        frame.output_extent = frame.render_extent;
        let image = |resource_id, format| FrameLabImage {
            resource_id,
            frame_index: 0,
            owner_queue_id: 1,
            extent: frame.render_extent,
            format,
            state: FRAMELAB_STATE_SHADER_READ,
            provenance: FRAMELAB_PROVENANCE_SYNTHETIC,
            lease,
        };
        frame.color = image(11, FRAMELAB_FORMAT_RGBA16_FLOAT);
        frame.depth = image(12, FRAMELAB_FORMAT_D24S8);
        frame.motion_vectors = image(13, FRAMELAB_FORMAT_RG16_FLOAT);
        for i in (0..16).step_by(5) {
            frame.camera.view_to_clip[i] = 1.0;
        }
        frame.camera.clip_to_view = frame.camera.view_to_clip;
        frame.camera.clip_to_lens = frame.camera.view_to_clip;
        frame.camera.clip_to_previous = frame.camera.view_to_clip;
        frame.camera.previous_to_clip = frame.camera.view_to_clip;
        frame.camera.up[1] = 1.0;
        frame.camera.right[0] = 1.0;
        frame.camera.forward[2] = 1.0;
        frame.camera.motion_vector_scale = [1.0; 2];
        frame.camera.near_plane = 0.1;
        frame.camera.far_plane = 100.0;
        frame.camera.fov_radians = 1.0;
        frame.camera.aspect = 16.0 / 9.0;
        frame.camera.projection_space = FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE;
        frame.camera.reprojection_space = FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE;
        frame.camera.motion_convention = FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV;
        frame.camera.flags = FRAMELAB_CAMERA_VERIFIED;
        frame.producer.queue_id = 1;
        frame.producer.command_stream_id = 1;
        frame.producer.completion = FRAMELAB_COMPLETION_SAME_COMMAND_STREAM;

        assert_eq!(framelab_fg_prepare(stream, &frame), FRAMELAB_OK);
        framelab_lease_destroy(lease);
        assert_eq!(released, 0);
        assert_eq!(framelab_fg_mark_submitted(stream), FRAMELAB_OK);
        assert_eq!(framelab_fg_stream_destroy(stream), FRAMELAB_BUSY);
        assert_eq!(framelab_fg_retire(stream), FRAMELAB_OK);
        assert_eq!(released, 1);
        assert_eq!(framelab_fg_stream_destroy(stream), FRAMELAB_OK);
    }
}
