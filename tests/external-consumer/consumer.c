/* SPDX-License-Identifier: MIT */
#include <framelab/framelab.h>

#include <stdio.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (0)

static void FRAMELAB_CALL release(void* user) { ++*(unsigned*)user; }

static FrameLabFgFrame make_frame(const FrameLabLease* lease, const FrameLabLease* signal)
{
    FrameLabFgFrame f = {0};
    f.struct_size = sizeof f;
    f.abi_version = FRAMELAB_ABI_VERSION;
    f.stream_id = 1;
    f.device_id = 2;
    f.generation = 1;
    f.render_extent = (FrameLabExtent){640, 360};
    f.output_extent = f.render_extent;
    f.delta_seconds = 1.0 / 60.0;
    f.color = (FrameLabImage){11, 0, 3, f.output_extent, FRAMELAB_FORMAT_RGBA16_FLOAT,
        FRAMELAB_STATE_SHADER_READ, FRAMELAB_PROVENANCE_SYNTHETIC, lease};
    f.depth = (FrameLabImage){12, 0, 3, f.render_extent, FRAMELAB_FORMAT_D24S8,
        FRAMELAB_STATE_DEPTH_ATTACHMENT, FRAMELAB_PROVENANCE_SYNTHETIC, lease};
    f.motion_vectors = (FrameLabImage){13, 0, 3, f.render_extent, FRAMELAB_FORMAT_RG16_FLOAT,
        FRAMELAB_STATE_COLOR_ATTACHMENT, FRAMELAB_PROVENANCE_SYNTHETIC, lease};
    for (unsigned i = 0; i < 16; i += 5) {
        f.camera.view_to_clip[i] = f.camera.clip_to_view[i] = f.camera.clip_to_lens[i] =
            f.camera.clip_to_previous[i] = f.camera.previous_to_clip[i] = 1.0f;
    }
    f.camera.up[1] = f.camera.right[0] = f.camera.forward[2] = 1.0f;
    f.camera.motion_vector_scale[0] = f.camera.motion_vector_scale[1] = 1.0f;
    f.camera.near_plane = 0.1f;
    f.camera.far_plane = 100.0f;
    f.camera.fov_radians = 1.0f;
    f.camera.aspect = 16.0f / 9.0f;
    f.camera.flags = FRAMELAB_CAMERA_VERIFIED;
    f.camera.projection_space = f.camera.reprojection_space = FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE;
    f.camera.motion_convention = FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV;
    f.producer = (FrameLabProducerSync){3, 0, 9, 5, FRAMELAB_COMPLETION_TIMELINE, signal};
    f.reset_history = FRAMELAB_RESET_INITIAL;
    return f;
}

int main(void)
{
    unsigned image_releases = 0, signal_releases = 0;
    FrameLabFgStream* stream = NULL;
    FrameLabLease* image = NULL;
    FrameLabLease* signal = NULL;
    CHECK(framelab_abi_version() == FRAMELAB_ABI_VERSION);
    CHECK(framelab_fg_stream_create(0, &stream) == FRAMELAB_ABI_MISMATCH && !stream);
    CHECK(framelab_fg_stream_create(FRAMELAB_ABI_VERSION, &stream) == FRAMELAB_OK);
    CHECK(framelab_lease_create(&image_releases, release, &image) == FRAMELAB_OK);
    CHECK(framelab_lease_create(&signal_releases, release, &signal) == FRAMELAB_OK);
    FrameLabFgFrame f = make_frame(image, signal);
    f.struct_size--;
    CHECK(framelab_fg_prepare(stream, &f) == FRAMELAB_ABI_MISMATCH);
    f.struct_size++;
    f.reset_history = 0;
    CHECK(framelab_fg_prepare(stream, &f) == FRAMELAB_RESET_REQUIRED);
    f.reset_history = FRAMELAB_RESET_INITIAL;
    f.depth.lease = NULL;
    CHECK(framelab_fg_prepare(stream, &f) == FRAMELAB_MISSING_LEASE);
    f.depth.lease = image;
    CHECK(framelab_fg_prepare(stream, &f) == FRAMELAB_OK);
    CHECK(framelab_fg_retire(stream) == FRAMELAB_INVALID_PHASE);
    CHECK(framelab_fg_stream_destroy(stream) == FRAMELAB_BUSY);
    CHECK(framelab_fg_cancel(stream) == FRAMELAB_OK);
    CHECK(image_releases == 0 && signal_releases == 0);
    CHECK(framelab_fg_prepare(stream, &f) == FRAMELAB_OK);
    CHECK(framelab_fg_mark_submitted(stream) == FRAMELAB_OK);
    CHECK(framelab_fg_cancel(stream) == FRAMELAB_INVALID_PHASE);
    CHECK(framelab_fg_prepare(stream, &f) == FRAMELAB_BUSY);
    CHECK(framelab_fg_retire(stream) == FRAMELAB_OK);
    CHECK(framelab_fg_prepare(stream, &f) == FRAMELAB_INVALID_SEQUENCE);

    f.frame_index = f.color.frame_index = f.depth.frame_index = f.motion_vectors.frame_index = 1;
    f.reset_history = 0;
    CHECK(framelab_fg_prepare(stream, &f) == FRAMELAB_OK);
    CHECK(framelab_fg_mark_submitted(stream) == FRAMELAB_OK);
    framelab_lease_destroy(image);
    framelab_lease_destroy(signal);
    CHECK(image_releases == 0 && signal_releases == 0);
    CHECK(framelab_fg_retire(stream) == FRAMELAB_OK);
    CHECK(image_releases == 1 && signal_releases == 1);
    CHECK(framelab_fg_stream_destroy(stream) == FRAMELAB_OK);
    return 0;
}
