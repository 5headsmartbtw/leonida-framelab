#pragma once
// Direct NGX backend for the sample's own rasterized scene. NVIDIA SDK is external.
// Probe architecture informed by dlssg-open, MIT (see THIRD_PARTY_LICENSES).
#include "fg/FrameInputsVulkan.hpp"
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>
#include <array>
#include <vector>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

class DirectNgxFG {
public:
  VkImage output{}; VkImageView outputView{};
  bool enabled=true, reset=true, generated=false;
  unsigned evaluations=0, verified=0, errors=0, rejectedFrames=0;
  const char* status="Initializing";

  void initialize(VkInstance instance,VkPhysicalDevice physical,VkDevice device,VkQueue queue,
                  std::uint32_t queueFamily,const std::filesystem::path& runtimeDir,
                  const std::filesystem::path& outputDir) {
    pd=physical; dev=device;expectedQueue=queue;expectedQueueFamily=queueFamily;
    logDirectory=outputDir;
    logFile=_wfopen((logDirectory / L"ngx.log").c_str(),L"wb");
    const wchar_t* paths[]={runtimeDir.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{}; info.PathListInfo.Path=paths;info.PathListInfo.Length=1;
    info.LoggingInfo.LoggingCallback=log;info.LoggingInfo.MinimumLoggingLevel=NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks=true;
    auto r=NVSDK_NGX_VULKAN_Init_with_ProjectID("7d4c2f0e-5b1a-4c38-9e61-2f8a6b3d9c10",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"sample.direct.1",logDirectory.c_str(),instance,pd,dev,vkGetInstanceProcAddr,vkGetDeviceProcAddr,&info,NVSDK_NGX_Version_API);
    printf("DIRECT_NGX_INIT=%#x\n",unsigned(r));
    if(!ok(r,"NGX initialization failed"))return;
    initialized=true;
    NVSDK_NGX_Parameter* caps{}; r=NVSDK_NGX_VULKAN_GetCapabilityParameters(&caps);
    int available=0; if(NVSDK_NGX_SUCCEED(r)&&caps)caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available,&available);
    printf("DIRECT_NGX_AVAILABLE=%d\n",available);
    if(caps)NVSDK_NGX_VULKAN_DestroyParameters(caps);
    if(!available){fail("NGX FG is unavailable");return;}
    ok(NVSDK_NGX_VULKAN_AllocateParameters(&params),"NGX parameters failed");
  }

  void resize(VkCommandBuffer cmd,VkExtent2D size,bool readback) {
    releaseImages(); extent=size;checking=readback;reset=true;
    if(!initialized||!params||errors)return;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;
    ci.extent={size.width,size.height,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;
    ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    vkcheck(vkCreateImage(dev,&ci,nullptr,&output));VkMemoryRequirements req{};vkGetImageMemoryRequirements(dev,output,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkcheck(vkAllocateMemory(dev,&ai,nullptr,&outputMemory));vkcheck(vkBindImageMemory(dev,output,outputMemory,0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=output;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkcheck(vkCreateImageView(dev,&vi,nullptr,&outputView));outputLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if(checking) {
      bytes=VkDeviceSize(size.width)*size.height*8;
      VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};b.size=bytes*2;b.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      vkcheck(vkCreateBuffer(dev,&b,nullptr,&buffer));vkGetBufferMemoryRequirements(dev,buffer,&req);
      ai.allocationSize=req.size;ai.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      vkcheck(vkAllocateMemory(dev,&ai,nullptr,&bufferMemory));vkcheck(vkBindBufferMemory(dev,buffer,bufferMemory,0));vkcheck(vkMapMemory(dev,bufferMemory,0,VK_WHOLE_SIZE,0,&mapped));
    }
    NVSDK_NGX_DLSSG_Create_Params cp{};cp.Width=cp.RenderWidth=size.width;cp.Height=cp.RenderHeight=size.height;cp.NativeBackbufferFormat=format;
    params->Set("DLSSG.SynchronousInit",1u);params->Set("DLSSG.EnableInterp",1u);
    auto r=NGX_VK_CREATE_DLSSG(cmd,1,1,&feature,params,&cp);
    printf("DIRECT_NGX_CREATE=%#x extent=%ux%u format=%d\n",unsigned(r),size.width,size.height,int(format));
    if(ok(r,"NGX feature creation failed"))status="Ready: native scene, FG 2X";
  }

  bool evaluate(VkCommandBuffer cmd,const frame_inputs::VulkanFrameInputs& inputs) {
    namespace fi=frame_inputs;
    if(inFlightInputs){
      ++rejectedFrames;reset=true;sequence.invalidate();status="previous GPU frame has not completed";
      printf("FRAMEINPUTS_REJECTED source_frame=%llu reason=%s\n",static_cast<unsigned long long>(inputs.frame.frameIndex),status);
      return false; // Preserve the earlier frame's pending readback and leases.
    }
    generated=false;readbackPending=false;
    if(!enabled||errors||!feature){reset=true;sequence.invalidate();return false;}
    auto checked=sequence.check(inputs.frame);
    if(checked)checked=fi::validateVulkan(inputs,cmd,dev,expectedQueue,expectedQueueFamily);
    if(checked&&inputs.frame.outputExtent!=fi::Extent{extent.width,extent.height})checked={fi::Error::dimensions,"frame/NGX feature extent mismatch"};
    if(checked&&reset&&inputs.frame.resetHistory==fi::Reset::none)checked={fi::Error::history,"backend requires history reset"};
    if(!checked){
      ++rejectedFrames;reset=true;sequence.invalidate();status=checked.reason.data();
      printf("FRAMEINPUTS_REJECTED source_frame=%llu reason=%s\n",static_cast<unsigned long long>(inputs.frame.frameIndex),status);
      return false; // No NGX call and no image barrier was recorded.
    }
    const auto& f=inputs.frame;const auto& c=f.camera;
    const VkImage color=inputs.color.image,depth=inputs.depth.image,mv=inputs.motionVectors.image;
    const VkImageView colorView=inputs.color.view,depthView=inputs.depth.view,mvView=inputs.motionVectors.view;
    const VkFormat depthFormat=inputs.depth.format;
    barrier(cmd,color,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    barrier(cmd,depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT);
    barrier(cmd,mv,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    barrier(cmd,output,outputLayout,VK_IMAGE_LAYOUT_GENERAL);
    if(checking) {
      VkClearColorValue clear{};for(float& x:clear.float32)x=std::numeric_limits<float>::quiet_NaN();
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(cmd,output,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
      barrier(cmd,output,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    }
    auto back=resource(color,colorView,format,false),d=resource(depth,depthView,depthFormat,false,VK_IMAGE_ASPECT_DEPTH_BIT),v=resource(mv,mvView,VK_FORMAT_R16G16_SFLOAT,false),out=resource(output,outputView,format,true);
    NVSDK_NGX_VK_DLSSG_Eval_Params ep{};ep.pBackbuffer=&back;ep.pDepth=&d;ep.pMVecs=&v;ep.pOutputInterpFrame=&out;
    NVSDK_NGX_DLSSG_Opt_Eval_Params o{};o.multiFrameCount=o.multiFrameIndex=1;o.reset=f.resetHistory!=fi::Reset::none;o.cameraMotionIncluded=c.cameraMotionIncluded;
    memcpy(o.cameraViewToClip,c.viewToClip.data(),64);memcpy(o.clipToCameraView,c.clipToView.data(),64);memcpy(o.clipToLensClip,c.clipToLens.data(),64);memcpy(o.clipToPrevClip,c.clipToPrevious.data(),64);memcpy(o.prevClipToClip,c.previousToClip.data(),64);
    memcpy(o.cameraPos,c.position.data(),12);memcpy(o.cameraUp,c.up.data(),12);memcpy(o.cameraRight,c.right.data(),12);memcpy(o.cameraFwd,c.forward.data(),12);
    o.cameraNear=c.nearPlane;o.cameraFar=c.farPlane;o.cameraFOV=c.fovRadians;o.cameraAspectRatio=c.aspect;
    o.mvecScale[0]=c.motionVectorScale[0];o.mvecScale[1]=c.motionVectorScale[1];o.jitterOffset[0]=c.jitterPixels[0];o.jitterOffset[1]=c.jitterPixels[1];
    o.depthInverted=c.depthInverted;o.orthoProjection=c.orthographic;o.motionVectorsInvalidValue=1e30f;
    o.mvecsSubrectSize=o.depthSubrectSize=o.backbufferSubrectSize=o.outputInterpSubrectSize={extent.width,extent.height};
    params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,static_cast<unsigned long long>(f.frameIndex+1));
    auto r=NGX_VK_EVALUATE_DLSSG(cmd,feature,params,&ep,&o);
    if(!ok(r,"NGX Evaluate failed")){generated=false;sequence.invalidate();}
    else {
      ++evaluations;generated=!o.reset;sequence.accept(f);inFlightInputs=f;pendingSourceFrame=f.frameIndex;readbackPending=checking;status="Ready: FrameInputs, FG 2X";
      if(checking)printf("FRAMEINPUTS_CONSUMED source_frame=%llu generation=%llu reset=%d leases_retained=1\n",static_cast<unsigned long long>(f.frameIndex),static_cast<unsigned long long>(f.generation),int(o.reset));
    }
    reset=false;
    if(checking&&NVSDK_NGX_SUCCEED(r)) {
      for(unsigned i=0;i<2;++i){VkImage im=i?color:output;barrier(cmd,im,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};copy.bufferOffset=bytes*i;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={extent.width,extent.height,1};
        vkCmdCopyImageToBuffer(cmd,im,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);barrier(cmd,im,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);}
      VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    }
    barrier(cmd,color,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(cmd,output,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);outputLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return generated;
  }

  // Caller invokes this only after the submitting queue/fence has actually completed.
  void onGpuCompleted() {
    inspectCompleted();
    if(inFlightInputs&&checking)printf("FRAMEINPUTS_GPU_COMPLETE source_frame=%llu leases_released=1\n",static_cast<unsigned long long>(pendingSourceFrame));
    inFlightInputs.reset();
  }
  bool hasInFlightInputs() const {return inFlightInputs.has_value();}

  void inspectCompleted() {
    if(!readbackPending||!checking||!mapped||errors||!evaluations)return;
    readbackPending=false;
    const auto* out=static_cast<const uint16_t*>(mapped);const auto* cur=out+bytes/2;
    size_t nonfinite=0,different=0,differentPrev=0;const size_t pixels=size_t(extent.width)*extent.height;
    for(size_t p=0;p<pixels;++p){for(int c=0;c<4;++c)nonfinite+=(out[p*4+c]&0x7c00)==0x7c00;
      different+=memcmp(out+p*4,cur+p*4,8)!=0;differentPrev+=previous.size()==bytes&&memcmp(out+p*4,previous.data()+p*8,8)!=0;}
    printf("DIRECT_READBACK frame=%u generated=%d nonfinite=%zu different_current=%zu different_previous=%zu\n",evaluations-1,int(generated),nonfinite,different,differentPrev);
    if(nonfinite||(generated&&(!different||!differentPrev)))fail("FG output verification failed");
    else if(generated)++verified;
    if(evaluations==6) {
      for(unsigned i=0;i<2;++i) {
        const auto path=logDirectory / (i ? L"input-5.rgba16f" : L"generated-5.rgba16f");
        FILE* file=_wfopen(path.c_str(),L"wb");
        if(file){fwrite(i?cur:out,1,size_t(bytes),file);fclose(file);}
      }
    }
    previous.assign(reinterpret_cast<const unsigned char*>(cur),reinterpret_cast<const unsigned char*>(cur)+bytes);
  }

  void releaseImages() {
    if(feature){NVSDK_NGX_VULKAN_ReleaseFeature(feature);feature=nullptr;}
    if(mapped){vkUnmapMemory(dev,bufferMemory);mapped=nullptr;}
    if(buffer)vkDestroyBuffer(dev,buffer,nullptr);if(bufferMemory)vkFreeMemory(dev,bufferMemory,nullptr);
    if(outputView)vkDestroyImageView(dev,outputView,nullptr);if(output)vkDestroyImage(dev,output,nullptr);if(outputMemory)vkFreeMemory(dev,outputMemory,nullptr);
    buffer={};bufferMemory={};output={};outputView={};outputMemory={};previous.clear();generated=false;reset=true;readbackPending=false;inFlightInputs.reset();sequence.invalidate();
  }
  void shutdown(){releaseImages();if(params){NVSDK_NGX_VULKAN_DestroyParameters(params);params=nullptr;}if(initialized){NVSDK_NGX_VULKAN_Shutdown1(dev);initialized=false;}if(logFile){fclose(logFile);logFile=nullptr;}}
private:
  std::filesystem::path logDirectory;
  static inline FILE* logFile=nullptr; // The SDK logging callback has no user-data parameter.
  frame_inputs::Sequence sequence;
  std::optional<frame_inputs::FrameInputs> inFlightInputs;
  bool readbackPending=false;std::uint64_t pendingSourceFrame=0;
  VkQueue expectedQueue{};std::uint32_t expectedQueueFamily=VK_QUEUE_FAMILY_IGNORED;
  VkPhysicalDevice pd{};VkDevice dev{};VkExtent2D extent{};VkDeviceMemory outputMemory{},bufferMemory{};VkBuffer buffer{};void* mapped{};VkDeviceSize bytes{};
  VkImageLayout outputLayout=VK_IMAGE_LAYOUT_UNDEFINED;static constexpr VkFormat format=VK_FORMAT_R16G16B16A16_SFLOAT;
  bool initialized=false,checking=false;NVSDK_NGX_Parameter* params{};NVSDK_NGX_Handle* feature{};std::vector<unsigned char> previous;
  static void NVSDK_CONV log(const char* msg,NVSDK_NGX_Logging_Level,NVSDK_NGX_Feature) {
    if(logFile){fputs(msg,logFile);fputc('\n',logFile);fflush(logFile);}
  }
  bool ok(NVSDK_NGX_Result r,const char* why){if(NVSDK_NGX_FAILED(r)){printf("DIRECT_NGX_ERROR=%#x %s\n",unsigned(r),why);fail(why);return false;}return true;}
  void fail(const char* why){++errors;enabled=false;generated=false;status=why;}
  static void vkcheck(VkResult r){if(r!=VK_SUCCESS)throw std::runtime_error("Direct NGX Vulkan allocation failed");}
  uint32_t memoryType(uint32_t bits,VkMemoryPropertyFlags flags){VkPhysicalDeviceMemoryProperties m{};vkGetPhysicalDeviceMemoryProperties(pd,&m);for(uint32_t i=0;i<m.memoryTypeCount;++i)if((bits&(1u<<i))&&(m.memoryTypes[i].propertyFlags&flags)==flags)return i;throw std::runtime_error("No compatible FG memory type");}
  NVSDK_NGX_Resource_VK resource(VkImage image,VkImageView view,VkFormat fmt,bool rw,VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT){return NVSDK_NGX_Create_ImageView_Resource_VK(view,image,{aspect,0,1,0,1},fmt,extent.width,extent.height,rw);}
  static void barrier(VkCommandBuffer cmd,VkImage image,VkImageLayout from,VkImageLayout to,VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=from;b.newLayout=to;b.image=image;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={aspect,0,1,0,1};
    b.srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
  }
};
