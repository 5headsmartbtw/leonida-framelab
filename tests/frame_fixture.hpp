// SPDX-License-Identifier: MIT
// Copyright (c) 2026 5headsmartbtw
#pragma once
#include "../fg/FrameInputs.hpp"
namespace test_fixture {
using namespace frame_inputs;
inline Matrix identity(){return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};}
inline void index(FrameInputs& f,std::uint64_t n){f.frameIndex=n;f.color.frameIndex=f.depth.frameIndex=f.motionVectors.frameIndex=n;}
inline FrameInputs valid(){
    FrameInputs f;f.stream=f.device=f.generation=1;f.renderExtent=f.outputExtent={1280,720};f.deltaSeconds=1./60;f.resetHistory=Reset::initial;
    auto lease=std::make_shared<int>(7);
    f.color={1,0,1,Format::rgba16_float,f.outputExtent,State::shader_read,Provenance::synthetic_renderer,lease};
    f.depth={2,0,1,Format::d24s8,f.renderExtent,State::depth_attachment,Provenance::synthetic_renderer,lease};
    f.motionVectors={3,0,1,Format::rg16_float,f.renderExtent,State::color_attachment,Provenance::synthetic_renderer,lease};
    auto& c=f.camera;c.viewToClip=c.clipToView=c.clipToLens=c.clipToPrevious=c.previousToClip=identity();c.up={0,1,0};c.right={1,0,0};c.forward={0,0,-1};
    c.nearPlane=.1f;c.farPlane=100;c.fovRadians=1;c.aspect=1280.f/720;c.projectionSpace=ClipSpace::y_down_zero_to_one;c.reprojectionSpace=ClipSpace::y_up_zero_to_one;c.motion=MotionConvention::previous_minus_current_uv;c.verified=true;c.cameraMotionIncluded=true;
    f.sync.completion=Completion::same_command_stream;f.sync.queue=1;f.sync.commandStream=1;return f;
}
}
