// SPDX-License-Identifier: MIT
// Copyright (c) 2026 5headsmartbtw
#pragma once
// Rendering-API-neutral frame description. No Vulkan, D3D, Streamline or NGX types.
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace frame_inputs {
using Id = std::uint64_t;
struct Extent { std::uint32_t width=0,height=0; bool operator==(const Extent&) const = default; };
enum class Format { unknown,rgba8_unorm,bgra8_unorm,rgba16_float,rg16_float,rg32_float,r32_float,d24s8,d32_float,rgb10a2_unorm };
enum class Provenance { unavailable,synthetic_renderer,verified_engine_binding };
enum class State { unknown,shader_read,color_attachment,depth_attachment,general };
enum class ClipSpace { unknown,y_up_zero_to_one,y_down_zero_to_one };
enum class MotionConvention { unknown,previous_minus_current_uv };
enum class Completion { unavailable,same_command_stream,host_complete,binary_signal,timeline_signal };
enum class Reset : std::uint32_t { none=0,initial=1,resize=2,camera_cut=4,reactivated=8,discontinuity=16 };
using Matrix = std::array<float,16>; // Row-major, unjittered. Jitter is separate, in render pixels.
struct Image {
    Id id=0,frameIndex=0,ownerQueue=0;
    Format format=Format::unknown;
    Extent extent{};
    State state=State::unknown;
    Provenance provenance=Provenance::unavailable;
    // Must own the allocation AND views, including any allocator needed by their destructor.
    std::shared_ptr<const void> allocationLease;
};
struct Camera {
    Matrix viewToClip{},clipToView{},clipToLens{},clipToPrevious{},previousToClip{};
    std::array<float,3> position{},up{},right{},forward{};
    std::array<float,2> jitterPixels{},motionVectorScale{1,1};
    float nearPlane=0,farPlane=0,fovRadians=0,aspect=0;
    ClipSpace projectionSpace=ClipSpace::unknown,reprojectionSpace=ClipSpace::unknown;
    MotionConvention motion=MotionConvention::unknown;
    bool verified=false,depthInverted=false,cameraMotionIncluded=false,orthographic=false;
};
struct Synchronization {
    Completion completion=Completion::unavailable;
    Id queue=0,commandStream=0,signal=0;
    std::uint64_t value=0;
    // External signal primitives must be retained until the consumer finishes waiting.
    std::shared_ptr<const void> signalLease;
};
struct FrameInputs {
    Id stream=0,device=0,generation=0;
    std::uint64_t frameIndex=0;
    Extent renderExtent{},outputExtent{};
    double deltaSeconds=0;
    Image color,depth,motionVectors;
    std::optional<Image> hudless,ui;
    Camera camera;
    Synchronization sync;
    Reset resetHistory=Reset::none;
    bool resized=false,deviceLost=false;
};
enum class Error { none,device_lost,identity,dimensions,delta,camera,image_semantics,image_description,lifetime,state,sync,history,sequence,binding,unsupported };
struct Result {
    Error error=Error::none;
    std::string_view reason{};
    explicit operator bool() const { return error==Error::none; }
};
// Colour formats the neutral description accepts. Each backend's profile decides which it supports.
inline bool colorFormat(Format f) {return f==Format::rgba8_unorm||f==Format::bgra8_unorm||f==Format::rgba16_float||f==Format::rgb10a2_unorm;}
template<std::size_t N> inline bool finite(const std::array<float,N>& a) {for(float f:a)if(!std::isfinite(f))return false;return true;}
inline Result validate(const FrameInputs& f) {
    if(f.deviceLost)return {Error::device_lost,"device lost"};
    if(!f.stream||!f.device||!f.generation)return {Error::identity,"stream/device/generation missing"};
    if(!f.renderExtent.width||!f.renderExtent.height||!f.outputExtent.width||!f.outputExtent.height)return {Error::dimensions,"zero frame extent"};
    if(!std::isfinite(f.deltaSeconds)||f.deltaSeconds<=0)return {Error::delta,"invalid frame delta"};
    const auto& c=f.camera;
    if(!c.verified||c.projectionSpace==ClipSpace::unknown||c.reprojectionSpace==ClipSpace::unknown||c.motion==MotionConvention::unknown||
       !finite(c.viewToClip)||!finite(c.clipToView)||!finite(c.clipToLens)||!finite(c.clipToPrevious)||!finite(c.previousToClip)||
       !finite(c.position)||!finite(c.up)||!finite(c.right)||!finite(c.forward)||!finite(c.jitterPixels)||!finite(c.motionVectorScale)||
       !(c.nearPlane>0&&std::isfinite(c.nearPlane)&&c.farPlane>c.nearPlane&&std::isfinite(c.farPlane))||
       !(c.fovRadians>0&&c.fovRadians<3.141593f)||!(c.aspect>0&&std::isfinite(c.aspect))||
       c.motionVectorScale[0]==0||c.motionVectorScale[1]==0)
        return {Error::camera,"camera/MV conventions missing or invalid"};
    for(const Matrix* m:{&c.viewToClip,&c.clipToView,&c.clipToLens,&c.clipToPrevious,&c.previousToClip}) {
        bool nonzero=false;for(float x:*m)nonzero|=x!=0;
        if(!nonzero)return {Error::camera,"zero camera matrix"};
    }
    for(const auto* v:{&c.up,&c.right,&c.forward}) {float n=0;for(float x:*v)n+=x*x;if(!(n>0))return {Error::camera,"zero camera basis"};}
    for(const Image* im:{&f.color,&f.depth,&f.motionVectors,f.hudless?&*f.hudless:nullptr,f.ui?&*f.ui:nullptr}) {
        if(!im)continue;
        if(!im->id||im->provenance==Provenance::unavailable)return {Error::image_semantics,"required image semantics unavailable"};
        if(im->frameIndex!=f.frameIndex)return {Error::sequence,"image belongs to another frame"};
        if(!im->extent.width||!im->extent.height||im->format==Format::unknown)return {Error::image_description,"image extent/format missing"};
        if(!im->allocationLease)return {Error::lifetime,"image allocation lease missing"};
        if(!im->ownerQueue||im->state==State::unknown)return {Error::state,"image ownership/state unknown"};
    }
    if(f.color.id==f.depth.id||f.color.id==f.motionVectors.id||f.depth.id==f.motionVectors.id)return {Error::image_semantics,"required images alias"};
    if(!colorFormat(f.color.format)||!(f.depth.format==Format::d24s8||f.depth.format==Format::d32_float||f.depth.format==Format::r32_float)||
       !(f.motionVectors.format==Format::rg16_float||f.motionVectors.format==Format::rg32_float))return {Error::image_semantics,"format does not match image semantic"};
    if(f.depth.extent!=f.renderExtent||f.motionVectors.extent!=f.renderExtent||f.color.extent!=f.outputExtent)return {Error::dimensions,"semantic image/frame extent mismatch"};
    const auto& sync=f.sync;
    if(sync.completion==Completion::unavailable||!sync.queue)return {Error::sync,"producer completion missing"};
    if(sync.completion==Completion::same_command_stream&&!sync.commandStream)return {Error::sync,"ordered command stream missing"};
    if((sync.completion==Completion::binary_signal||sync.completion==Completion::timeline_signal)&&(!sync.signal||!sync.signalLease))return {Error::sync,"external completion signal/lease missing"};
    if(sync.completion==Completion::timeline_signal&&!sync.value)return {Error::sync,"timeline value missing"};
    if(sync.completion==Completion::binary_signal&&sync.value)return {Error::sync,"binary signal has timeline value"};
    if(f.resized&&f.resetHistory==Reset::none)return {Error::history,"resize requires history reset"};
    return {};
}

// Stateful continuity check. Validate before API calls; accept only after successful recording.
// A failed submission/device loss must invalidate the stream before it is used again.
class Sequence {
public:
    Result check(const FrameInputs& f) const {
        if(auto r=validate(f);!r)return r;
        if(needsReset&&f.resetHistory==Reset::none)return {Error::history,"initial/discontinuous frame requires reset"};
        if(!last)return {};
        if(f.stream!=last->stream||f.device!=last->device)return {Error::identity,"stream/device changed"};
        if(f.frameIndex<=last->index)return {Error::sequence,"non-monotonic frame index"};
        if(f.generation<last->generation)return {Error::sequence,"resource generation regressed"};
        const bool changed=signature(f)!=last->images||f.renderExtent!=last->render||f.outputExtent!=last->output;
        if(changed&&f.generation==last->generation)return {Error::history,"resources changed without new generation"};
        if((changed||f.generation!=last->generation||f.frameIndex!=last->index+1)&&f.resetHistory==Reset::none)
            return {Error::history,"frame/resource discontinuity requires reset"};
        return {};
    }
    void accept(const FrameInputs& f) {last=Snapshot{f.stream,f.device,f.generation,f.frameIndex,f.renderExtent,f.outputExtent,signature(f)};needsReset=false;}
    void invalidate(){needsReset=true;}
private:
    struct ImageSignature {
        Id id;Format format;Extent extent;std::weak_ptr<const void> allocation;
        bool operator==(const ImageSignature& b) const {return id==b.id&&format==b.format&&extent==b.extent&&!allocation.owner_before(b.allocation)&&!b.allocation.owner_before(allocation);}
    };
    static std::array<ImageSignature,5> signature(const FrameInputs& f){
        const Image empty{};std::array<ImageSignature,5> result{};
        const Image* images[]={&f.color,&f.depth,&f.motionVectors,f.hudless?&*f.hudless:&empty,f.ui?&*f.ui:&empty};
        for(int i=0;i<5;++i){const auto& im=*images[i];result[i]={im.id,im.format,im.extent,im.allocationLease};}
        return result;
    }
    struct Snapshot {Id stream,device,generation,index;Extent render,output;std::array<ImageSignature,5> images;};
    std::optional<Snapshot> last;
    bool needsReset=true;
};
} // namespace frame_inputs
