// SPDX-License-Identifier: MIT
#include <framelab/framelab.h>

#include "fg_stream_internal.hpp"

#include <algorithm>
#include <memory>
#include <new>
#include <optional>

namespace fi = frame_inputs;

namespace {
struct LeaseOwner {
    LeaseOwner(void* user, FrameLabReleaseFn release) noexcept : user(user), release(release) {}
    ~LeaseOwner() { release(user); }
    void* user;
    FrameLabReleaseFn release;
};
} // namespace

struct FrameLabLease {
    std::shared_ptr<const void> owner;
};

struct FrameLabFgStream {
    fi::Sequence history;
    std::optional<fi::FrameInputs> frame;
    bool submitted = false;
};

std::shared_ptr<const void> framelab::detail::retainLease(const FrameLabLease* lease) noexcept
{
    return lease ? lease->owner : nullptr;
}

const fi::FrameInputs* framelab::detail::preparedInputs(const FrameLabFgStream* stream) noexcept
{
    return stream && stream->frame ? &*stream->frame : nullptr;
}

namespace {
FrameLabStatus statusFor(fi::Error error)
{
    switch (error) {
    case fi::Error::none: return FRAMELAB_OK;
    case fi::Error::device_lost: return FRAMELAB_DEVICE_LOST;
    case fi::Error::identity: return FRAMELAB_INVALID_IDENTITY;
    case fi::Error::dimensions: return FRAMELAB_INVALID_DIMENSIONS;
    case fi::Error::delta: return FRAMELAB_INVALID_DELTA;
    case fi::Error::camera: return FRAMELAB_INVALID_CAMERA;
    case fi::Error::image_semantics: return FRAMELAB_INVALID_IMAGE_SEMANTICS;
    case fi::Error::image_description: return FRAMELAB_INVALID_IMAGE_DESCRIPTION;
    case fi::Error::lifetime: return FRAMELAB_MISSING_LEASE;
    case fi::Error::state: return FRAMELAB_INVALID_RESOURCE_STATE;
    case fi::Error::sync: return FRAMELAB_INVALID_SYNC;
    case fi::Error::history: return FRAMELAB_RESET_REQUIRED;
    case fi::Error::sequence: return FRAMELAB_INVALID_SEQUENCE;
    case fi::Error::binding:
    case fi::Error::unsupported: return FRAMELAB_UNSUPPORTED;
    }
    return FRAMELAB_UNSUPPORTED;
}

constexpr fi::Format formats[] = {
    fi::Format::unknown, fi::Format::rgba8_unorm, fi::Format::bgra8_unorm,
    fi::Format::rgba16_float, fi::Format::rg16_float, fi::Format::rg32_float,
    fi::Format::r32_float, fi::Format::d24s8, fi::Format::d32_float
};
constexpr fi::State states[] = {
    fi::State::unknown, fi::State::shader_read, fi::State::color_attachment,
    fi::State::depth_attachment, fi::State::general
};
constexpr fi::Provenance provenances[] = {
    fi::Provenance::unavailable, fi::Provenance::synthetic_renderer,
    fi::Provenance::verified_engine_binding
};
constexpr fi::ClipSpace clipSpaces[] = {
    fi::ClipSpace::unknown, fi::ClipSpace::y_up_zero_to_one, fi::ClipSpace::y_down_zero_to_one
};
constexpr fi::Completion completions[] = {
    fi::Completion::unavailable, fi::Completion::same_command_stream, fi::Completion::host_complete,
    fi::Completion::binary_signal, fi::Completion::timeline_signal
};

template<typename T, std::size_t N>
T decode(const T (&values)[N], uint32_t index)
{
    return index < N ? values[index] : values[0];
}

fi::Extent extent(FrameLabExtent in) { return {in.width, in.height}; }

fi::Image image(const FrameLabImage& in)
{
    return {in.resource_id, in.frame_index, in.owner_queue_id, decode(formats, in.format),
            extent(in.extent), decode(states, in.state), decode(provenances, in.provenance),
            in.lease ? in.lease->owner : std::shared_ptr<const void>{}};
}

fi::Camera camera(const FrameLabCamera& in)
{
    fi::Camera out;
    std::ranges::copy(in.view_to_clip, out.viewToClip.begin());
    std::ranges::copy(in.clip_to_view, out.clipToView.begin());
    std::ranges::copy(in.clip_to_lens, out.clipToLens.begin());
    std::ranges::copy(in.clip_to_previous, out.clipToPrevious.begin());
    std::ranges::copy(in.previous_to_clip, out.previousToClip.begin());
    std::ranges::copy(in.position, out.position.begin());
    std::ranges::copy(in.up, out.up.begin());
    std::ranges::copy(in.right, out.right.begin());
    std::ranges::copy(in.forward, out.forward.begin());
    std::ranges::copy(in.jitter_pixels, out.jitterPixels.begin());
    std::ranges::copy(in.motion_vector_scale, out.motionVectorScale.begin());
    out.nearPlane = in.near_plane;
    out.farPlane = in.far_plane;
    out.fovRadians = in.fov_radians;
    out.aspect = in.aspect;
    out.projectionSpace = decode(clipSpaces, in.projection_space);
    out.reprojectionSpace = decode(clipSpaces, in.reprojection_space);
    if (in.motion_convention == FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV)
        out.motion = fi::MotionConvention::previous_minus_current_uv;
    out.verified = (in.flags & FRAMELAB_CAMERA_VERIFIED) != 0;
    out.depthInverted = (in.flags & FRAMELAB_CAMERA_DEPTH_INVERTED) != 0;
    out.cameraMotionIncluded = (in.flags & FRAMELAB_CAMERA_MOTION_INCLUDED) != 0;
    out.orthographic = (in.flags & FRAMELAB_CAMERA_ORTHOGRAPHIC) != 0;
    return out;
}

fi::FrameInputs translate(const FrameLabFgFrame& frame)
{
    const auto& sync = frame.producer;
    fi::FrameInputs out;
    out.stream = frame.stream_id;
    out.device = frame.device_id;
    out.generation = frame.generation;
    out.frameIndex = frame.frame_index;
    out.renderExtent = extent(frame.render_extent);
    out.outputExtent = extent(frame.output_extent);
    out.deltaSeconds = frame.delta_seconds;
    out.color = image(frame.color);
    out.depth = image(frame.depth);
    out.motionVectors = image(frame.motion_vectors);
    out.camera = camera(frame.camera);
    out.sync = {decode(completions, sync.completion), sync.queue_id, sync.command_stream_id,
                sync.signal_id, sync.timeline_value,
                sync.signal_lease ? sync.signal_lease->owner : std::shared_ptr<const void>{}};
    out.resetHistory = static_cast<fi::Reset>(frame.reset_history);
    out.resized = (frame.flags & FRAMELAB_FRAME_RESIZED) != 0;
    out.deviceLost = (frame.flags & FRAMELAB_FRAME_DEVICE_LOST) != 0;
    return out;
}
} // namespace

uint32_t FRAMELAB_CALL framelab_abi_version(void) { return FRAMELAB_ABI_VERSION; }

const char* FRAMELAB_CALL framelab_status_string(FrameLabStatus status)
{
    switch (status) {
    case FRAMELAB_OK: return "success";
    case FRAMELAB_DEVICE_LOST: return "device lost";
    case FRAMELAB_INVALID_IDENTITY: return "invalid stream, device or generation";
    case FRAMELAB_INVALID_DIMENSIONS: return "invalid frame or image dimensions";
    case FRAMELAB_INVALID_DELTA: return "invalid frame delta";
    case FRAMELAB_INVALID_CAMERA: return "invalid camera metadata";
    case FRAMELAB_INVALID_IMAGE_SEMANTICS: return "invalid image semantics";
    case FRAMELAB_INVALID_IMAGE_DESCRIPTION: return "invalid image description";
    case FRAMELAB_MISSING_LEASE: return "missing allocation lease";
    case FRAMELAB_INVALID_RESOURCE_STATE: return "invalid resource state or owner";
    case FRAMELAB_INVALID_SYNC: return "invalid producer synchronization metadata";
    case FRAMELAB_RESET_REQUIRED: return "history reset required";
    case FRAMELAB_INVALID_SEQUENCE: return "invalid frame sequence";
    case FRAMELAB_UNSUPPORTED: return "unsupported profile";
    case FRAMELAB_INVALID_ARGUMENT: return "invalid argument";
    case FRAMELAB_ABI_MISMATCH: return "ABI version or descriptor size mismatch";
    case FRAMELAB_BUSY: return "stream has an outstanding frame";
    case FRAMELAB_INVALID_PHASE: return "invalid frame lifecycle phase";
    case FRAMELAB_OUT_OF_MEMORY: return "out of memory";
    case FRAMELAB_NATIVE_FAILURE: return "native backend operation failed";
    case FRAMELAB_NOT_READY: return "GPU completion not observed";
    default: return "unknown status";
    }
}

FrameLabStatus FRAMELAB_CALL framelab_lease_create(void* user, FrameLabReleaseFn release,
                                                   FrameLabLease** out_lease)
{
    if (!out_lease) return FRAMELAB_INVALID_ARGUMENT;
    *out_lease = nullptr;
    if (!release) return FRAMELAB_INVALID_ARGUMENT;
    try {
        auto lease = std::make_unique<FrameLabLease>();
        lease->owner = std::make_shared<LeaseOwner>(user, release);
        *out_lease = lease.release();
        return FRAMELAB_OK;
    } catch (const std::bad_alloc&) {
        return FRAMELAB_OUT_OF_MEMORY;
    }
}

void FRAMELAB_CALL framelab_lease_destroy(FrameLabLease* lease) { delete lease; }

FrameLabStatus FRAMELAB_CALL framelab_fg_stream_create(uint32_t version, FrameLabFgStream** out)
{
    if (!out) return FRAMELAB_INVALID_ARGUMENT;
    *out = nullptr;
    if (version != FRAMELAB_ABI_VERSION) return FRAMELAB_ABI_MISMATCH;
    *out = new (std::nothrow) FrameLabFgStream;
    return *out ? FRAMELAB_OK : FRAMELAB_OUT_OF_MEMORY;
}

FrameLabStatus FRAMELAB_CALL framelab_fg_stream_destroy(FrameLabFgStream* stream)
{
    if (stream && stream->frame) return FRAMELAB_BUSY;
    delete stream;
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_fg_prepare(FrameLabFgStream* stream, const FrameLabFgFrame* frame)
{
    if (!stream || !frame) return FRAMELAB_INVALID_ARGUMENT;
    if (stream->frame) return FRAMELAB_BUSY;
    if (frame->struct_size < sizeof(FrameLabFgFrame) || frame->abi_version != FRAMELAB_ABI_VERSION)
        return FRAMELAB_ABI_MISMATCH;
    if ((frame->flags & ~UINT32_C(3)) || (frame->camera.flags & ~UINT32_C(15))
        || (frame->reset_history & ~UINT32_C(31)))
        return FRAMELAB_INVALID_ARGUMENT;
    auto inputs = translate(*frame);
    auto result = stream->history.check(inputs);
    if (!result) return statusFor(result.error);
    stream->frame.emplace(std::move(inputs));
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_fg_mark_submitted(FrameLabFgStream* stream)
{
    if (!stream) return FRAMELAB_INVALID_ARGUMENT;
    if (!stream->frame || stream->submitted) return FRAMELAB_INVALID_PHASE;
    stream->history.accept(*stream->frame);
    stream->submitted = true;
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_fg_cancel(FrameLabFgStream* stream)
{
    if (!stream) return FRAMELAB_INVALID_ARGUMENT;
    if (!stream->frame || stream->submitted) return FRAMELAB_INVALID_PHASE;
    stream->history.invalidate();
    stream->frame.reset();
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_fg_retire(FrameLabFgStream* stream)
{
    if (!stream) return FRAMELAB_INVALID_ARGUMENT;
    if (!stream->submitted) return FRAMELAB_INVALID_PHASE;
    stream->submitted = false;
    stream->frame.reset();
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_fg_invalidate(FrameLabFgStream* stream)
{
    if (!stream) return FRAMELAB_INVALID_ARGUMENT;
    if (stream->frame) return FRAMELAB_BUSY;
    stream->history.invalidate();
    return FRAMELAB_OK;
}
