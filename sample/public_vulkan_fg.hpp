// SPDX-License-Identifier: MIT
#pragma once

#include <framelab/vulkan_fg.h>
#include "fg/FrameInputsVulkan.hpp" // Host-side metadata types shared with the original reference scene.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

class PublicVulkanFg {
public:
  VkImageView outputView{};
  bool enabled=true,reset=true;
  unsigned evaluations=0,verified=0,errors=0,rejectedFrames=0;
  const char* status="Initializing";
  std::shared_ptr<void> deviceOwner;
  VkQueue producerQueue{};
  uint32_t producerFamily=VK_QUEUE_FAMILY_IGNORED;
  bool crossFamily=false;
  unsigned retirementProbeFrame=0;

  void initialize(VkInstance instance,VkPhysicalDevice physical,VkDevice device,VkQueue queue,
                  uint32_t family,const std::filesystem::path& runtime,const std::filesystem::path& logs)
  {
    dev=device;consumer=queue;consumerFamily=family;
    auto owner=makeLease(deviceOwner);
    const FrameLabVulkanFgConfig config{sizeof config,FRAMELAB_ABI_VERSION,instance,physical,device,queue,
      family,1,1,owner.get(),"7d4c2f0e-5b1a-4c38-9e61-2f8a6b3d9c10","sample.direct.1",runtime.c_str(),logs.c_str()};
    const auto result=framelab_vulkan_fg_create(&config,&fg);
    if(result!=FRAMELAB_OK) {
      status=framelab_status_string(result);
      std::printf("PUBLIC_VULKAN_FG_UNAVAILABLE status=%s\n",status);
      enabled=false;
      if(result!=FRAMELAB_UNSUPPORTED)++errors;
      return;
    }
    if(producerQueue) {
      handoff=std::make_shared<Handoff>();
      handoff->owner=deviceOwner;
      handoff->device=device;
      handoff->ready=semaphore(!crossFamily);
      if(crossFamily) {
        handoff->toRelay=semaphore(false);
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex=producerFamily;
        vkcheck(vkCreateCommandPool(device,&pool,nullptr,&handoff->pool));
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool=handoff->pool;
        allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount=1;
        vkcheck(vkAllocateCommandBuffers(device,&allocation,&handoff->relay));
      }
      handoffLease=makeLease(handoff);
      std::printf("PUBLIC_VULKAN_HANDOFF mode=%s family=%u->%u\n",
                  crossFamily?"binary_cross_family":"timeline_same_family",producerFamily,consumerFamily);
    }
    status="Ready: installed VulkanFG SDK";
  }

  void resize(VkCommandBuffer cmd,VkExtent2D extent,bool diagnostics)
  {
    outputView={};
    reset=true;
    if(!fg)return;
    const auto result=framelab_vulkan_fg_record_resize(fg,cmd,{extent.width,extent.height},diagnostics);
    if(result!=FRAMELAB_OK) {
      framelab_vulkan_fg_cancel(fg);
      check(result);
    }
    checking=diagnostics;
    pending=true;
  }

  bool willSplit() const {return fg&&handoff&&enabled;}

  // The scene's actual rendering is submitted before the FG command buffer.
  // A transfer-family relay passes exclusive ownership back to the graphics queue.
  void splitProducer(VkCommandBuffer producerCmd,const frame_inputs::VulkanFrameInputs& inputs)
  {
    if(crossFamily) ownership(producerCmd,inputs,consumerFamily,producerFamily,false,true);
    vkcheck(vkEndCommandBuffer(producerCmd));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount=1;
    submit.pCommandBuffers=&producerCmd;
    submit.signalSemaphoreCount=1;
    if(crossFamily) {
      submit.pSignalSemaphores=&handoff->toRelay;
      vkcheck(vkQueueSubmit(consumer,1,&submit,VK_NULL_HANDLE));
      vkcheck(vkResetCommandBuffer(handoff->relay,0));
      const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,
                                          VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      vkcheck(vkBeginCommandBuffer(handoff->relay,&begin));
      ownership(handoff->relay,inputs,consumerFamily,producerFamily,true,false);
      ownership(handoff->relay,inputs,producerFamily,consumerFamily,false,false);
      vkcheck(vkEndCommandBuffer(handoff->relay));
      constexpr VkPipelineStageFlags all=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      submit.waitSemaphoreCount=1;
      submit.pWaitSemaphores=&handoff->toRelay;
      submit.pWaitDstStageMask=&all;
      submit.pCommandBuffers=&handoff->relay;
      submit.pSignalSemaphores=&handoff->ready;
      vkcheck(vkQueueSubmit(producerQueue,1,&submit,VK_NULL_HANDLE));
    } else {
      // Odd values are producer-ready. The consumer returns the shared sample resources
      // with the next even value, after both generated and real-frame reads.
      const uint64_t reusable=producerTicket?producerTicket+1:0;
      producerTicket=reusable+1;
      constexpr VkPipelineStageFlags all=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
      if(reusable) {
        timeline.waitSemaphoreValueCount=1;
        timeline.pWaitSemaphoreValues=&reusable;
        submit.waitSemaphoreCount=1;
        submit.pWaitSemaphores=&handoff->ready;
        submit.pWaitDstStageMask=&all;
      }
      timeline.signalSemaphoreValueCount=1;
      timeline.pSignalSemaphoreValues=&producerTicket;
      submit.pNext=&timeline;
      submit.pSignalSemaphores=&handoff->ready;
      vkcheck(vkQueueSubmit(producerQueue,1,&submit,VK_NULL_HANDLE));
      producerNeedsReturn=true;
    }
    external=true;
  }

  bool evaluate(VkCommandBuffer cmd,const frame_inputs::VulkanFrameInputs& inputs)
  {
    if(!fg||!enabled) {
      reset=true;
      if(fg)check(framelab_vulkan_fg_invalidate(fg));
      return false;
    }
    const auto& source=inputs.frame;
    if(auto& owner=source.color.allocationLease; owner&&
       (!frameLease||frameOwner.owner_before(owner)||owner.owner_before(frameOwner))) {
      frameLease=makeLease(owner,&releasedFrameLeases);
      frameOwner=owner;
    }
    FrameLabFgFrame frame{};
    frame.struct_size=sizeof frame;
    frame.abi_version=FRAMELAB_ABI_VERSION;
    frame.stream_id=source.stream;frame.device_id=source.device;frame.generation=source.generation;
    frame.frame_index=source.frameIndex;frame.delta_seconds=source.deltaSeconds;
    frame.render_extent=extent(source.renderExtent);frame.output_extent=extent(source.outputExtent);
    frame.reset_history=static_cast<uint32_t>(source.resetHistory);
    frame.flags=(source.resized?FRAMELAB_FRAME_RESIZED:0)|(source.deviceLost?FRAMELAB_FRAME_DEVICE_LOST:0);
    frame.color=toImage(source.color);frame.depth=toImage(source.depth);frame.motion_vectors=toImage(source.motionVectors);
    frame.camera=camera(source.camera);
    frame.producer={1,source.sync.commandStream,0,0,FRAMELAB_COMPLETION_SAME_COMMAND_STREAM,nullptr};
    FrameLabVulkanFgBindings bindings{sizeof bindings,FRAMELAB_ABI_VERSION,cmd,cmd,source.sync.commandStream,
      consumerFamily,VK_NULL_HANDLE,0,toNative(inputs.color),toNative(inputs.depth),toNative(inputs.motionVectors)};
    if(external) {
      frame.producer={2,0,1,crossFamily?0:producerTicket,
                      static_cast<uint32_t>(crossFamily?FRAMELAB_COMPLETION_BINARY:FRAMELAB_COMPLETION_TIMELINE),
                      handoffLease.get()};
      frame.color.owner_queue_id=frame.depth.owner_queue_id=frame.motion_vectors.owner_queue_id=2;
      bindings.producer_commands=VK_NULL_HANDLE;
      bindings.producer_queue_family=producerFamily;
      bindings.producer_semaphore=handoff->ready;
      bindings.producer_semaphore_id=1;
      bindings.color.queue_family=bindings.depth.queue_family=bindings.motion_vectors.queue_family=producerFamily;
    }
    uint32_t generated=0;
    const auto result=framelab_vulkan_fg_record(fg,&frame,&bindings,&generated);
    if(result==FRAMELAB_OK) {
      pending=true;framePending=true;pendingFrame=source.frameIndex;
      reset=false;
      if(checking)std::printf("FRAMEINPUTS_CONSUMED source_frame=%llu generation=%llu reset=%d leases_retained=1\n",
          static_cast<unsigned long long>(source.frameIndex),static_cast<unsigned long long>(source.generation),
          frame.reset_history!=0);
      return generated!=0;
    }
    reset=true;
    if(result==FRAMELAB_NATIVE_FAILURE||result==FRAMELAB_OUT_OF_MEMORY||result==FRAMELAB_DEVICE_LOST) {
      // The host must discard the command buffer after a partially recorded native failure.
      vkEndCommandBuffer(cmd);
      framelab_vulkan_fg_cancel(fg);
      check(result);
    }
    if(external&&crossFamily)ownership(cmd,inputs,producerFamily,consumerFamily,true,true);
    ++rejectedFrames;
    std::printf("FRAMEINPUTS_REJECTED source_frame=%llu reason=%s\n",
          static_cast<unsigned long long>(source.frameIndex),framelab_status_string(result));
    return false;
  }

  VkResult submit(VkSubmitInfo info,VkFence fence)
  {
    if(!pending&&!external)return vkQueueSubmit(consumer,1,&info,fence);
    FrameLabVulkanFgSubmission requirements{sizeof requirements,FRAMELAB_ABI_VERSION};
    if(pending)check(framelab_vulkan_fg_submission(fg,&requirements));
    else if(external) {
      requirements.wait_semaphore=handoff->ready;
      requirements.wait_stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      requirements.wait_value=crossFamily?0:producerTicket;
    }

    VkSemaphore waits[3]{},signals[2]{};
    VkPipelineStageFlags stages[3]{};
    uint64_t waitValues[3]{},signalValues[2]{};
    if(info.waitSemaphoreCount>1||info.signalSemaphoreCount>1||info.pNext)
      throw std::runtime_error("unexpected synthetic sample semaphore list");
    unsigned waitCount=info.waitSemaphoreCount,signalCount=info.signalSemaphoreCount;
    if(waitCount){waits[0]=info.pWaitSemaphores[0];stages[0]=info.pWaitDstStageMask[0];}
    if(signalCount)signals[0]=info.pSignalSemaphores[0];
    if(requirements.wait_semaphore) {
      waits[waitCount]=requirements.wait_semaphore;stages[waitCount]=requirements.wait_stage;
      waitValues[waitCount++]=requirements.wait_value;
    }
    // Hold a real FG submission behind a host test gate before probing SDK retirement.
    const bool probe=checking&&pending&&pendingFrame==retirementProbeFrame&&framePending&&!earlyRetireTest;
    if(probe) {
      if(!gate)gate=semaphore(true);
      waits[waitCount]=gate;stages[waitCount]=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;waitValues[waitCount++]=1;
    }
    if(pending) {
      signals[signalCount]=requirements.completion_timeline;
      signalValues[signalCount++]=++completionTicket;
    }
    const VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,nullptr,
      waitCount,waitValues,signalCount,signalValues};
    info.pNext=&timeline;
    info.waitSemaphoreCount=waitCount;info.pWaitSemaphores=waits;info.pWaitDstStageMask=stages;
    info.signalSemaphoreCount=signalCount;info.pSignalSemaphores=signals;
    const auto result=vkQueueSubmit(consumer,1,&info,fence);
    if(result!=VK_SUCCESS) {
      if(pending)framelab_vulkan_fg_cancel(fg);
      pending=false;
      return result;
    }
    const auto ack=pending?framelab_vulkan_fg_mark_submitted(fg,completionTicket):FRAMELAB_OK;
    if(probe) {
      releasesBeforeProbe=releasedFrameLeases;
      frameLease.reset();
      const bool retirement=framelab_vulkan_fg_retire(fg)==FRAMELAB_NOT_READY;
      const bool destruction=framelab_vulkan_fg_destroy(fg)==FRAMELAB_BUSY;
      const bool retained=releasedFrameLeases==releasesBeforeProbe;
      const VkSemaphoreSignalInfo release{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,nullptr,gate,1};
      vkcheck(vkSignalSemaphore(dev,&release));
      if(!retirement||!destruction||!retained)throw std::runtime_error("public Vulkan FG retired before its actual signal");
      earlyRetireTest=true;
      std::puts("PUBLIC_VULKAN_FG early_retire_rejected=1 pending_destroy_rejected=1");
    }
    check(ack);
    return result;
  }

  void onGpuCompleted()
  {
    if(pending) {
      check(framelab_vulkan_fg_retire(fg));
      if(earlyRetireTest&&framePending&&pendingFrame==retirementProbeFrame) {
        if(releasedFrameLeases!=releasesBeforeProbe+1)throw std::runtime_error("SDK did not release the final input lease");
        std::puts("PUBLIC_VULKAN_FG final_lease_released_after_gpu=1");
      }
      if(checking&&framePending)std::printf("FRAMEINPUTS_GPU_COMPLETE source_frame=%llu leases_released=1\n",
                                      static_cast<unsigned long long>(pendingFrame));
      pending=false;
      FrameLabVulkanFgOutput image{};
      check(framelab_vulkan_fg_output(fg,&image));
      outputView=image.view;
      FrameLabVulkanFgStats stats{};
      check(framelab_vulkan_fg_stats(fg,&stats));
      evaluations=stats.evaluated;verified=stats.verified;
      errors+=stats.native_errors-nativeErrors;nativeErrors=stats.native_errors;
    }
    framePending=false;external=false;
  }

  void finishFrame()
  {
    if(!producerNeedsReturn)return;
    const uint64_t reusable=producerTicket+1;
    const VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
      nullptr,0,nullptr,1,&reusable};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,&timeline};
    submit.signalSemaphoreCount=1;
    submit.pSignalSemaphores=&handoff->ready;
    vkcheck(vkQueueSubmit(consumer,1,&submit,VK_NULL_HANDLE));
    vkcheck(vkQueueWaitIdle(consumer));
    producerNeedsReturn=false;
  }

  bool hasInFlightInputs() const {return pending;}

  void shutdown()
  {
    if(fg)check(framelab_vulkan_fg_destroy(fg));
    fg=nullptr;
    if(gate)vkDestroySemaphore(dev,gate,nullptr);
    gate=VK_NULL_HANDLE;
    frameLease.reset();handoffLease.reset();handoff.reset();deviceOwner.reset();
  }

private:
  using Lease=std::unique_ptr<FrameLabLease,decltype(&framelab_lease_destroy)>;
  struct Handoff {
    std::shared_ptr<void> owner;
    VkDevice device{};
    VkSemaphore ready{},toRelay{};
    VkCommandPool pool{};
    VkCommandBuffer relay{};
    ~Handoff() {
      if(pool)vkDestroyCommandPool(device,pool,nullptr);
      if(toRelay)vkDestroySemaphore(device,toRelay,nullptr);
      if(ready)vkDestroySemaphore(device,ready,nullptr);
    }
  };
  Lease frameLease{nullptr,framelab_lease_destroy},handoffLease{nullptr,framelab_lease_destroy};
  std::weak_ptr<const void> frameOwner;
  std::shared_ptr<Handoff> handoff;
  FrameLabVulkanFg* fg{};
  VkDevice dev{};
  VkQueue consumer{};
  VkSemaphore gate{};
  uint32_t consumerFamily{},nativeErrors{};
  unsigned releasedFrameLeases{},releasesBeforeProbe{};
  uint64_t completionTicket{},producerTicket{},pendingFrame{};
  bool pending=false,framePending=false,external=false,checking=false,earlyRetireTest=false;
  bool producerNeedsReturn=false;

  static void check(FrameLabStatus result) {
    if(result!=FRAMELAB_OK)throw std::runtime_error(framelab_status_string(result));
  }
  static void vkcheck(VkResult result) {
    if(result!=VK_SUCCESS)throw std::runtime_error("Vulkan public FG sample GPU operation failed: "+std::to_string(result));
  }
  struct LeaseOwner {std::shared_ptr<const void> owner;unsigned* releases;};
  static void FRAMELAB_CALL drop(void* pointer) {
    const std::unique_ptr<LeaseOwner> payload(static_cast<LeaseOwner*>(pointer));
    if(payload->releases)++*payload->releases;
  }
  static Lease makeLease(std::shared_ptr<const void> owner,unsigned* releases=nullptr) {
    if(!owner)return {nullptr,framelab_lease_destroy};
    auto payload=std::make_unique<LeaseOwner>(LeaseOwner{std::move(owner),releases});
    FrameLabLease* lease{};
    check(framelab_lease_create(payload.get(),drop,&lease));
    payload.release();
    return {lease,framelab_lease_destroy};
  }
  VkSemaphore semaphore(bool timeline) {
    const VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,nullptr,
                                        VK_SEMAPHORE_TYPE_TIMELINE,0};
    const VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,timeline?&type:nullptr};
    VkSemaphore handle{};
    vkcheck(vkCreateSemaphore(dev,&info,nullptr,&handle));
    return handle;
  }
  static void ownership(VkCommandBuffer cmd,const frame_inputs::VulkanFrameInputs& frame,
                        uint32_t from,uint32_t to,bool acquire,bool accesses) {
    const frame_inputs::VulkanImage* native[]{&frame.color,&frame.depth,&frame.motionVectors};
    VkImageMemoryBarrier images[3]{};
    for(unsigned i=0;i<3;++i) {
      auto& barrier=images[i];
      barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.image=native[i]->image;
      barrier.oldLayout=barrier.newLayout=native[i]->layout;
      barrier.srcQueueFamilyIndex=from;barrier.dstQueueFamilyIndex=to;
      const auto aspects=i==1?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
      barrier.subresourceRange={VkImageAspectFlags(aspects),0,1,0,1};
      if(accesses)(acquire?barrier.dstAccessMask:barrier.srcAccessMask)=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0,0,nullptr,0,nullptr,3,images);
  }
  static FrameLabExtent extent(frame_inputs::Extent value) {return {value.width,value.height};}
  FrameLabImage toImage(const frame_inputs::Image& in) const {
    const auto& owner=in.allocationLease;
    const bool leased=owner&&!frameOwner.owner_before(owner)&&!owner.owner_before(frameOwner);
    return {in.id,in.frameIndex,in.ownerQueue,extent(in.extent),static_cast<uint32_t>(in.format),
           static_cast<uint32_t>(in.state),static_cast<uint32_t>(in.provenance),leased?frameLease.get():nullptr};
  }
  static FrameLabVulkanFgImage toNative(const frame_inputs::VulkanImage& in) {
    return {in.id,in.image,in.view,extent(in.extent),in.format,in.layout,in.aspects,in.queueFamily};
  }
  static FrameLabCamera camera(const frame_inputs::Camera& c) {
    FrameLabCamera out{};
    std::ranges::copy(c.viewToClip,out.view_to_clip);std::ranges::copy(c.clipToView,out.clip_to_view);
    std::ranges::copy(c.clipToLens,out.clip_to_lens);std::ranges::copy(c.clipToPrevious,out.clip_to_previous);
    std::ranges::copy(c.previousToClip,out.previous_to_clip);std::ranges::copy(c.position,out.position);
    std::ranges::copy(c.up,out.up);std::ranges::copy(c.right,out.right);std::ranges::copy(c.forward,out.forward);
    std::ranges::copy(c.jitterPixels,out.jitter_pixels);std::ranges::copy(c.motionVectorScale,out.motion_vector_scale);
    out.near_plane=c.nearPlane;out.far_plane=c.farPlane;out.fov_radians=c.fovRadians;out.aspect=c.aspect;
    out.projection_space=static_cast<uint32_t>(c.projectionSpace);
    out.reprojection_space=static_cast<uint32_t>(c.reprojectionSpace);
    out.motion_convention=static_cast<uint32_t>(c.motion);
    out.flags=(c.verified?FRAMELAB_CAMERA_VERIFIED:0)|(c.depthInverted?FRAMELAB_CAMERA_DEPTH_INVERTED:0)|
      (c.cameraMotionIncluded?FRAMELAB_CAMERA_MOTION_INCLUDED:0)|(c.orthographic?FRAMELAB_CAMERA_ORTHOGRAPHIC:0);
    return out;
  }
};
