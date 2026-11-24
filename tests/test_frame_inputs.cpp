// SPDX-License-Identifier: MIT
// Copyright (c) 2026 5headsmartbtw
// Intentionally includes no rendering API or vendor SDK headers.
#include "frame_fixture.hpp"
#include <iostream>
#include <limits>
using namespace frame_inputs;
#define CHECK(x) do{++checks;if(!(x)){std::cerr<<"FAILED line "<<__LINE__<<": "<<#x<<'\n';return 1;}}while(false)
int main(){
    unsigned checks=0;
    auto good=test_fixture::valid();CHECK(validate(good));
    auto f=good;f.depth={};CHECK(validate(f).error==Error::image_semantics);
    f=good;f.motionVectors.provenance=Provenance::unavailable;CHECK(validate(f).error==Error::image_semantics);
    f=good;f.depth.allocationLease.reset();CHECK(validate(f).error==Error::lifetime);
    f=good;f.color.frameIndex=7;CHECK(validate(f).error==Error::sequence);
    f=good;f.motionVectors.extent.width--;CHECK(validate(f).error==Error::dimensions);
    f=good;f.depth.format=Format::rgba16_float;CHECK(validate(f).error==Error::image_semantics);
    f=good;f.motionVectors.id=f.color.id;CHECK(validate(f).error==Error::image_semantics);
    f=good;f.color.state=State::unknown;CHECK(validate(f).error==Error::state);
    f=good;f.deviceLost=true;CHECK(validate(f).error==Error::device_lost);
    f=good;f.deltaSeconds=std::numeric_limits<double>::quiet_NaN();CHECK(validate(f).error==Error::delta);
    f=good;f.deltaSeconds=0;CHECK(validate(f).error==Error::delta);
    f=good;f.camera.verified=false;CHECK(validate(f).error==Error::camera);
    f=good;f.camera.viewToClip[0]=std::numeric_limits<float>::infinity();CHECK(validate(f).error==Error::camera);
    f=good;f.camera.clipToPrevious={};CHECK(validate(f).error==Error::camera);
    f=good;f.camera.nearPlane=-1;CHECK(validate(f).error==Error::camera);
    f=good;f.camera.forward={};CHECK(validate(f).error==Error::camera);
    f=good;f.sync={};CHECK(validate(f).error==Error::sync);
    f=good;f.sync.commandStream=0;CHECK(validate(f).error==Error::sync);
    f=good;f.sync.completion=Completion::timeline_signal;CHECK(validate(f).error==Error::sync);
    f.sync.signal=7;f.sync.signalLease=std::make_shared<int>(0);CHECK(validate(f).error==Error::sync);
    f.sync.value=42;CHECK(validate(f));
    f.sync.completion=Completion::binary_signal;CHECK(validate(f).error==Error::sync);f.sync.value=0;CHECK(validate(f));
    f=good;f.resized=true;f.resetHistory=Reset::none;CHECK(validate(f).error==Error::history);
    Sequence sequence;f=good;f.resetHistory=Reset::none;CHECK(sequence.check(f).error==Error::history);
    CHECK(sequence.check(good));sequence.accept(good);CHECK(sequence.check(good).error==Error::sequence);
    f=good;test_fixture::index(f,1);f.resetHistory=Reset::none;CHECK(sequence.check(f));
    auto bad=f;bad.depth={};CHECK(!sequence.check(bad));CHECK(sequence.check(f)); // Rejection must not advance state.
    sequence.accept(f);sequence.invalidate();test_fixture::index(f,2);CHECK(sequence.check(f).error==Error::history);
    f.resetHistory=Reset::reactivated;CHECK(sequence.check(f));sequence.accept(f);
    test_fixture::index(f,3);f.generation=2;f.resetHistory=Reset::none;CHECK(sequence.check(f).error==Error::history);
    f.resetHistory=Reset::resize;CHECK(sequence.check(f));sequence.accept(f);
    test_fixture::index(f,4);f.generation=1;CHECK(sequence.check(f).error==Error::sequence);
    f.generation=2;f.color.allocationLease=std::make_shared<int>(5);CHECK(sequence.check(f).error==Error::history);
    f.generation=3;CHECK(sequence.check(f));sequence.accept(f);
    test_fixture::index(f,9);f.resetHistory=Reset::none;CHECK(sequence.check(f).error==Error::history);
    f.resetHistory=Reset::discontinuity;CHECK(sequence.check(f));
    bool destroyed=false;
    {auto owned=std::shared_ptr<int>(new int(4),[&](int* p){destroyed=true;delete p;});
     auto retained=good;retained.color.allocationLease=owned;owned.reset();CHECK(!destroyed);auto consumed=retained;retained.color.allocationLease.reset();CHECK(!destroyed);consumed.color.allocationLease.reset();CHECK(destroyed);}
    std::cout<<"PASS neutral FrameInputs checks="<<checks<<'\n';return 0;
}
