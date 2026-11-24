/* SPDX-License-Identifier: MIT */
#ifndef FRAMELAB_FRAMELAB_H
#define FRAMELAB_FRAMELAB_H

#include <stdint.h>

#if defined(_WIN32) && defined(FRAMELAB_SHARED)
#  if defined(FRAMELAB_BUILDING)
#    define FRAMELAB_API __declspec(dllexport)
#  else
#    define FRAMELAB_API __declspec(dllimport)
#  endif
#elif defined(FRAMELAB_SHARED) && defined(__GNUC__)
#  define FRAMELAB_API __attribute__((visibility("default")))
#else
#  define FRAMELAB_API
#endif

#if defined(_WIN32)
#  define FRAMELAB_CALL __cdecl
#else
#  define FRAMELAB_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define FRAMELAB_ABI_VERSION UINT32_C(1)

typedef struct FrameLabLease FrameLabLease;
typedef struct FrameLabFgStream FrameLabFgStream;
typedef uint32_t FrameLabStatus;
typedef void (FRAMELAB_CALL *FrameLabReleaseFn)(void* user);

enum {
    FRAMELAB_OK = 0,
    FRAMELAB_DEVICE_LOST = 1,
    FRAMELAB_INVALID_IDENTITY = 2,
    FRAMELAB_INVALID_DIMENSIONS = 3,
    FRAMELAB_INVALID_DELTA = 4,
    FRAMELAB_INVALID_CAMERA = 5,
    FRAMELAB_INVALID_IMAGE_SEMANTICS = 6,
    FRAMELAB_INVALID_IMAGE_DESCRIPTION = 7,
    FRAMELAB_MISSING_LEASE = 8,
    FRAMELAB_INVALID_RESOURCE_STATE = 9,
    FRAMELAB_INVALID_SYNC = 10,
    FRAMELAB_RESET_REQUIRED = 11,
    FRAMELAB_INVALID_SEQUENCE = 12,
    FRAMELAB_UNSUPPORTED = 14,
    FRAMELAB_INVALID_ARGUMENT = 20,
    FRAMELAB_ABI_MISMATCH = 21,
    FRAMELAB_BUSY = 22,
    FRAMELAB_INVALID_PHASE = 23,
    FRAMELAB_OUT_OF_MEMORY = 24
};

enum {
    FRAMELAB_FORMAT_RGBA8_UNORM = 1, FRAMELAB_FORMAT_BGRA8_UNORM = 2,
    FRAMELAB_FORMAT_RGBA16_FLOAT = 3, FRAMELAB_FORMAT_RG16_FLOAT = 4,
    FRAMELAB_FORMAT_RG32_FLOAT = 5, FRAMELAB_FORMAT_R32_FLOAT = 6,
    FRAMELAB_FORMAT_D24S8 = 7, FRAMELAB_FORMAT_D32_FLOAT = 8
};
enum {
    FRAMELAB_STATE_SHADER_READ = 1, FRAMELAB_STATE_COLOR_ATTACHMENT = 2,
    FRAMELAB_STATE_DEPTH_ATTACHMENT = 3, FRAMELAB_STATE_GENERAL = 4
};
enum { FRAMELAB_PROVENANCE_SYNTHETIC = 1, FRAMELAB_PROVENANCE_VERIFIED = 2 };
enum { FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE = 1, FRAMELAB_CLIP_Y_DOWN_ZERO_TO_ONE = 2 };
enum { FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV = 1 };
enum {
    FRAMELAB_COMPLETION_SAME_COMMAND_STREAM = 1, FRAMELAB_COMPLETION_HOST_COMPLETE = 2,
    FRAMELAB_COMPLETION_BINARY = 3, FRAMELAB_COMPLETION_TIMELINE = 4
};
enum {
    FRAMELAB_RESET_INITIAL = 1, FRAMELAB_RESET_RESIZE = 2, FRAMELAB_RESET_CAMERA_CUT = 4,
    FRAMELAB_RESET_REACTIVATED = 8, FRAMELAB_RESET_DISCONTINUITY = 16
};
enum {
    FRAMELAB_CAMERA_VERIFIED = 1, FRAMELAB_CAMERA_DEPTH_INVERTED = 2,
    FRAMELAB_CAMERA_MOTION_INCLUDED = 4, FRAMELAB_CAMERA_ORTHOGRAPHIC = 8
};
enum { FRAMELAB_FRAME_RESIZED = 1, FRAMELAB_FRAME_DEVICE_LOST = 2 };

typedef struct FrameLabExtent { uint32_t width, height; } FrameLabExtent;

typedef struct FrameLabImage {
    uint64_t resource_id, frame_index, owner_queue_id;
    FrameLabExtent extent;
    uint32_t format, state, provenance;
    const FrameLabLease* lease;
} FrameLabImage;

typedef struct FrameLabCamera {
    float view_to_clip[16], clip_to_view[16], clip_to_lens[16];
    float clip_to_previous[16], previous_to_clip[16];
    float position[3], up[3], right[3], forward[3];
    float jitter_pixels[2], motion_vector_scale[2];
    float near_plane, far_plane, fov_radians, aspect;
    uint32_t projection_space, reprojection_space, motion_convention, flags;
} FrameLabCamera;

typedef struct FrameLabProducerSync {
    uint64_t queue_id, command_stream_id, signal_id, timeline_value;
    uint32_t completion;
    const FrameLabLease* signal_lease;
} FrameLabProducerSync;

typedef struct FrameLabFgFrame {
    uint32_t struct_size, abi_version;
    uint64_t stream_id, device_id, generation, frame_index;
    FrameLabExtent render_extent, output_extent;
    double delta_seconds;
    FrameLabImage color, depth, motion_vectors;
    FrameLabCamera camera;
    FrameLabProducerSync producer;
    uint32_t reset_history, flags;
} FrameLabFgFrame;

FRAMELAB_API uint32_t FRAMELAB_CALL framelab_abi_version(void);
FRAMELAB_API const char* FRAMELAB_CALL framelab_status_string(FrameLabStatus status);

/* On success SDK owns one release obligation, fulfilled after the last reference.
 * The callback must not throw or re-enter the same stream. Destroy handles once. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_lease_create(
    void* user, FrameLabReleaseFn release, FrameLabLease** out_lease);
FRAMELAB_API void FRAMELAB_CALL framelab_lease_destroy(FrameLabLease* lease);

/* The host serializes calls on a stream and owns GPU submission. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_fg_stream_create(
    uint32_t abi_version, FrameLabFgStream** out_stream);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_fg_stream_destroy(FrameLabFgStream* stream);
/* Validate metadata and pin inputs for one frame. No native GPU calls. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_fg_prepare(
    FrameLabFgStream* stream, const FrameLabFgFrame* frame);
/* Commit history only after the host's successful GPU submission. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_fg_mark_submitted(FrameLabFgStream* stream);
/* Cancel before submission, when no GPU work can use the prepared resources. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_fg_cancel(FrameLabFgStream* stream);
/* Release the SDK's references only after the host observes GPU completion. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_fg_retire(FrameLabFgStream* stream);
/* Invalidate temporal history while idle, for example after falling back. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_fg_invalidate(FrameLabFgStream* stream);

#ifdef __cplusplus
}
#endif
#endif
