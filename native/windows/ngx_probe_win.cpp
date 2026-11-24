// Windows experimental port of ../probe/ngx_probe.cpp; original MIT license applies.
// ngx_probe: headless NGX DLSS Frame Generation check on native Linux Vulkan.
// Usage: ngx_probe <runtime dir> [eval]
// Reports what NGX says about DLSS-G on this GPU, creates the feature, and with "eval" runs one evaluation.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <psapi.h>
#include <chrono>
#include <atomic>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_helpers_dlssg_vk.h"

static std::atomic<unsigned> validationErrors{0},validationWarnings{0};
static VKAPI_ATTR VkBool32 VKAPI_CALL validationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)++validationErrors;else if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)++validationWarnings;
    printf("VALIDATION severity=%u %s\n",unsigned(severity),data&&data->pMessage?data->pMessage:"(no message)");return VK_FALSE;
}
static const char* kProjectId = "7d4c2f0e-5b1a-4c38-9e61-2f8a6b3d9c10";

static void NVSDK_CONV ngx_log(const char* msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    fputs("ngx: ", stdout), fputs(msg, stdout);
    size_t n = strlen(msg);
    if (!n || msg[n - 1] != '\n') fputc('\n', stdout);
}

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d\n", #x, r_); return 1; } } while (0)

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    NVSDK_NGX_Resource_VK res{};
};

static uint32_t mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

static bool make_image(VkPhysicalDevice pd, VkDevice dev, VkFormat fmt, uint32_t w, uint32_t h, bool rw, Image& out) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = fmt, ci.extent = {w, h, 1}, ci.mipLevels = 1, ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(dev, &ci, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, out.image, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size, ai.memoryTypeIndex = mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &ai, nullptr, &out.mem) != VK_SUCCESS) return false;
    vkBindImageMemory(dev, out.image, out.mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &vi, nullptr, &out.view) != VK_SUCCESS) return false;
    out.res = NVSDK_NGX_Create_ImageView_Resource_VK(out.view, out.image, vi.subresourceRange, fmt, w, h, rw);
    return true;
}


struct HostBuffer { VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{}; };
static bool host_buffer(VkPhysicalDevice pd,VkDevice dev,size_t bytes,VkBufferUsageFlags usage,HostBuffer& out){
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=bytes;ci.usage=usage;
    if(vkCreateBuffer(dev,&ci,nullptr,&out.buffer)!=VK_SUCCESS)return false;
    VkMemoryRequirements mr{};vkGetBufferMemoryRequirements(dev,out.buffer,&mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=mem_type(pd,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(vkAllocateMemory(dev,&ai,nullptr,&out.memory)!=VK_SUCCESS)return false;
    if(vkBindBufferMemory(dev,out.buffer,out.memory,0)!=VK_SUCCESS)return false;
    return vkMapMemory(dev,out.memory,0,bytes,0,&out.mapped)==VK_SUCCESS;
}
static void save_rgba(const char* dir,const char* kind,int frame,const void* data,size_t bytes){
    std::string path=std::string(dir)+"/"+kind+"-"+std::to_string(frame)+".rgba";
    FILE*f=fopen(path.c_str(),"wb");if(!f){printf("FAIL output file %s\n",path.c_str());return;}fwrite(data,1,bytes,f);fclose(f);
}
#include "winvk_presenter.h"
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if(argc>3){HMODULE proxy=LoadLibraryA(argv[3]);printf("Windows proxy=%p error=%lu\n",proxy,proxy?0:GetLastError());if(!proxy)return 2;}
    if (argc < 2) return printf("usage: %s <runtime dir> [eval]\n", argv[0]), 2;
    const bool presentation=getenv("GTA6_VK_PRESENT")!=nullptr;
    const unsigned frameCount=getenv("GTA6_FG_FRAMES")?unsigned(atoi(getenv("GTA6_FG_FRAMES"))):6;
    const bool eval = argc > 2 && !strcmp(argv[2], "eval");
    const uint32_t W = 1280, H = 720;

    std::wstring wdir(argv[1], argv[1] + strlen(argv[1]));
    const wchar_t* paths[] = {wdir.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths, info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = ngx_log;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    printf("SDK_API=%x\n",unsigned(NVSDK_NGX_Version_API));
    NVSDK_NGX_FeatureDiscoveryInfo d{};
    d.SDKVersion = NVSDK_NGX_Version_API;
    d.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    d.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    d.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    d.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    d.Identifier.v.ProjectDesc.EngineVersion = "1.0";
    d.ApplicationDataPath = wdir.c_str();
    d.FeatureInfo = &info;

    // instance
    std::vector<const char*> iext;
    {
        uint32_t n = 0;
        VkExtensionProperties* p = nullptr;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&d, &n, &p);
        printf("probe: instance extension requirements: %#x, %u\n", (unsigned)r, n);
        for (uint32_t i = 0; i < n && p; i++) printf("probe:   %s\n", p[i].extensionName), iext.push_back(p[i].extensionName);
    }
    if(presentation){iext.push_back(VK_KHR_SURFACE_EXTENSION_NAME);iext.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);}
    const bool validationRequested=getenv("VK_INSTANCE_LAYERS")&&strstr(getenv("VK_INSTANCE_LAYERS"),"VK_LAYER_KHRONOS_validation");
    if(validationRequested)iext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ngx_probe", app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app, ici.enabledExtensionCount = (uint32_t)iext.size(), ici.ppEnabledExtensionNames = iext.data();
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, nullptr, &inst));
    VkDebugUtilsMessengerEXT messenger{};
    if(validationRequested){
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;debug.pfnUserCallback=validationMessage;
        auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(inst,"vkCreateDebugUtilsMessengerEXT"));
        if(!create)return printf("FAIL validation callback unavailable\n"),1;CHECK(create(inst,&debug,nullptr,&messenger));printf("VALIDATION_MESSENGER_CREATED\n");
    }


    uint32_t npd = 8;
    VkPhysicalDevice pds[8], pd = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(inst, &npd, pds);
    for (uint32_t i = 0; i < npd; i++) {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(pds[i], &pp);
        if (pp.vendorID == 0x10de && !pd) pd = pds[i], printf("probe: GPU %s (device %#x)\n", pp.deviceName, pp.deviceID);
    }
    if (!pd) return printf("FAIL no NVIDIA GPU\n"), 1;

    // device
    uint32_t nall = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &nall, nullptr);
    std::vector<VkExtensionProperties> all(nall);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &nall, all.data());
    auto has = [&](const char* name) { for (auto& e : all) if (!strcmp(e.extensionName, name)) return true; return false; };
    std::vector<std::string> dext_s = {"VK_NVX_binary_import", "VK_NVX_image_view_handle", "VK_KHR_push_descriptor"};
    {
        uint32_t n = 0;
        VkExtensionProperties* p = nullptr;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(inst, pd, &d, &n, &p);
        printf("probe: device extension requirements: %#x, %u\n", (unsigned)r, n);
        for (uint32_t i = 0; i < n && p; i++) {
            printf("probe:   %s%s\n", p[i].extensionName, has(p[i].extensionName) ? "" : " (device lacks it)");
            bool listed = false;
            for (auto& s : dext_s) listed |= s == p[i].extensionName;
            if (!listed && has(p[i].extensionName)) dext_s.push_back(p[i].extensionName);
        }
    }
    printf("Windows VK_NVX_binary_import=%d VK_NVX_image_view_handle=%d\n",has("VK_NVX_binary_import"),has("VK_NVX_image_view_handle"));
    if(presentation)dext_s.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    const bool budgetAvailable=has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if(budgetAvailable)dext_s.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    std::vector<const char*> dext;
    for (auto& s : dext_s) if (has(s.c_str())) dext.push_back(s.c_str());
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    uint32_t nq = 0, qf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qs(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qs.data());
    for (uint32_t i = 0; i < nq; i++)
        if ((qs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { qf = i; break; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qf, qci.queueCount = 1, qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)dext.size(), dci.ppEnabledExtensionNames = dext.data();
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qf, 0, &queue);

    // NGX
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", wdir.c_str(), inst, pd, dev,
                                                              vkGetInstanceProcAddr, vkGetDeviceProcAddr, &info, NVSDK_NGX_Version_API);
    printf("probe: NGX init: %#x\n", (unsigned)r);
    if (NVSDK_NGX_FAILED(r)) return 1;
    NVSDK_NGX_Parameter* caps = nullptr;
    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_FAILED(r) || !caps) return printf("FAIL capability parameters %#x\n", (unsigned)r), 1;
    int sr = 0, fg = 0, fg_drv = 0, fg_init = 0;
    unsigned most = 0;
    NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &sr);
    NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_FrameGeneration_Available, &fg);
    NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &fg_drv);
    NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &fg_init);
    NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &most);
    printf("probe: SuperSampling_Available=%d FrameGeneration_Available=%d NeedsUpdatedDriver=%d FeatureInitResult=%#x MultiFrameCountMax=%u\n",
           sr, fg, fg_drv, (unsigned)fg_init, most);

    // the feature
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, pci.queueFamilyIndex = qf;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    auto submit = [&]() -> VkResult {
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1, si.pCommandBuffers = &cmd;
        VkResult s = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        if (s != VK_SUCCESS) return s;
        return vkQueueWaitIdle(queue);
    };

    NVSDK_NGX_Parameter* params = nullptr;
    r = NVSDK_NGX_VULKAN_AllocateParameters(&params);
    if (NVSDK_NGX_FAILED(r) || !params) return printf("FAIL allocate parameters %#x\n", (unsigned)r), 1;
    NVSDK_NGX_DLSSG_Create_Params cp{};
    cp.Width = W, cp.Height = H, cp.NativeBackbufferFormat = VK_FORMAT_R8G8B8A8_UNORM;
    cp.RenderWidth = W, cp.RenderHeight = H;
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_DLSSG_Parameter_Width, W);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_DLSSG_Parameter_Height, H);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_DLSSG_Parameter_InternalWidth, W);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_DLSSG_Parameter_InternalHeight, H);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
    NVSDK_NGX_Parameter_SetUI(params,"DLSSG.EnableInterp",1u);
    NVSDK_NGX_Parameter_SetUI(params,"DLSSG.SynchronousInit",1u);
    NVSDK_NGX_Handle* feature = nullptr;
    CHECK(vkBeginCommandBuffer(cmd, &bi));
    r = NGX_VK_CREATE_DLSSG(cmd, 1, 1, &feature, params, &cp);
    VkResult s = submit();
    printf("probe: create DLSS-G feature: %#x, handle %p, submit %d\n", (unsigned)r, (void*)feature, s);
    if (NVSDK_NGX_FAILED(r) || !feature) return 1;

    if (eval) {
        Image back, depth, mv, out;
        if (!make_image(pd, dev, VK_FORMAT_R8G8B8A8_UNORM, W, H, false, back) || !make_image(pd, dev, VK_FORMAT_R32_SFLOAT, W, H, false, depth) ||
            !make_image(pd, dev, VK_FORMAT_R16G16_SFLOAT, W, H, false, mv) || !make_image(pd, dev, VK_FORMAT_R8G8B8A8_UNORM, W, H, true, out))
            return printf("FAIL images\n"), 1;
        const char* outdir=getenv("GTA6_VK_OUTPUT_DIR");if(!outdir)outdir="E:/Dev/GTA6/logs/fg/vulkan-frames";
        CreateDirectoryA(outdir,nullptr);
        HostBuffer upload[4],readback;
        for(auto& b:upload)if(!host_buffer(pd,dev,W*H*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,b))return printf("FAIL upload buffer\n"),1;
        if(!host_buffer(pd,dev,W*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,readback))return printf("FAIL readback buffer\n"),1;
        VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(pd,&properties);VkQueryPoolCreateInfo qciTime{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qciTime.queryType=VK_QUERY_TYPE_TIMESTAMP;qciTime.queryCount=2;VkQueryPool times{};CHECK(vkCreateQueryPool(dev,&qciTime,nullptr,&times));
        TestVkPresenter presenter;if(presentation&&!presenter.init(inst,pd,dev,queue,qf,cmd))return printf("FAIL presentation init\n"),1;
        auto seriesBegin=std::chrono::steady_clock::now();
        std::vector<uint32_t> pixels(W*H),previous;unsigned verified=0;
        static float ident[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
        for (int frame = 0; frame < int(frameCount); frame++) {
            auto frameBegin=std::chrono::steady_clock::now();
            const unsigned left=160+(frame%18)*32;
            auto* colorData=static_cast<uint32_t*>(upload[0].mapped);auto* depthData=static_cast<float*>(upload[1].mapped);auto* mvData=static_cast<uint16_t*>(upload[2].mapped);auto* sentinelData=static_cast<uint32_t*>(upload[3].mapped);
            for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){unsigned i=y*W+x;bool obj=x>=left&&x<left+160&&y>=240&&y<480;bool grid=((x/32+y/32)%2)!=0;pixels[i]=colorData[i]=obj?0xff2080f0:(grid?0xff303030:0xff181818);depthData[i]=obj?0.3f:0.9f;mvData[2*i]=DirectX::PackedVector::XMConvertFloatToHalf(obj?-32.0f/W:0.0f);mvData[2*i+1]=0;sentinelData[i]=0xffff00ff;}
            CHECK(vkBeginCommandBuffer(cmd, &bi));
            Image* images[]={&back,&depth,&mv,&out};
            for(unsigned i=0;i<4;++i){
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.oldLayout=frame?(i==3?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL):VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
                b.image=images[i]->image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};b.srcAccessMask=frame?VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT:0;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
                VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={W,H,1};
                vkCmdCopyBufferToImage(cmd,upload[i].buffer,images[i]->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
                b.oldLayout=b.newLayout;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
            }
            NVSDK_NGX_VK_DLSSG_Eval_Params ep{};
            ep.pBackbuffer = &back.res, ep.pDepth = &depth.res, ep.pMVecs = &mv.res, ep.pOutputInterpFrame = &out.res;
            NVSDK_NGX_DLSSG_Opt_Eval_Params o{};
            o.multiFrameCount = 1, o.multiFrameIndex = 1;
            memcpy(o.cameraViewToClip, ident, sizeof ident), memcpy(o.clipToCameraView, ident, sizeof ident);
            memcpy(o.clipToLensClip, ident, sizeof ident), memcpy(o.clipToPrevClip, ident, sizeof ident), memcpy(o.prevClipToClip, ident, sizeof ident);
            o.mvecScale[0] = o.mvecScale[1] = 1.0f;
            DirectX::XMFLOAT4X4 proj,inv;auto P=DirectX::XMMatrixPerspectiveFovLH(1.0f,float(W)/H,0.1f,1000.0f);DirectX::XMStoreFloat4x4(&proj,P);DirectX::XMStoreFloat4x4(&inv,DirectX::XMMatrixInverse(nullptr,P));memcpy(o.cameraViewToClip,&proj,64);memcpy(o.clipToCameraView,&inv,64);o.cameraMotionIncluded=true;o.motionVectorsInvalidValue=1e30f;
            o.cameraUp[1] = 1, o.cameraRight[0] = 1, o.cameraFwd[2] = 1;
            o.cameraNear = 0.1f, o.cameraFar = 1000.0f, o.cameraFOV = 1.0f, o.cameraAspectRatio = (float)W / H;
            o.reset = frame%18 == 0;
            o.mvecsSubrectSize = {W, H}, o.depthSubrectSize = {W, H}, o.backbufferSubrectSize = {W, H}, o.outputInterpSubrectSize = {W, H};
            NVSDK_NGX_Parameter_SetULL(params, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, (unsigned long long)frame + 1);
            vkCmdResetQueryPool(cmd,times,0,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,times,0);
            auto evaluateBegin=std::chrono::steady_clock::now();
            r = NGX_VK_EVALUATE_DLSSG(cmd, feature, params, &ep, &o);
            vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,times,1);
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=VK_IMAGE_LAYOUT_GENERAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=out.image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
            VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={W,H,1};vkCmdCopyImageToBuffer(cmd,out.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&copy);
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
            s = submit();
            printf("probe: evaluate frame %d: %#x, submit %d\n", frame, (unsigned)r, s);
            if (NVSDK_NGX_FAILED(r) || s != VK_SUCCESS) return 1;
            auto* data=static_cast<uint32_t*>(readback.mapped);size_t sentinel=0,current=0,prev=0,redCount=0;double redX=0;
            for(size_t i=0;i<pixels.size();++i){sentinel+=data[i]==0xffff00ff;current+=data[i]==pixels[i];prev+=!previous.empty()&&data[i]==previous[i];unsigned rr=data[i]&255,gg=(data[i]>>8)&255,bb=(data[i]>>16)&255;if(rr>150&&rr>gg*1.3&&gg>bb*1.5){redX+=i%W;++redCount;}}
            printf("READBACK frame=%d sentinel=%zu equal_current=%zu equal_previous=%zu centroid_x=%.4f expected_midpoint_x=%.1f\n",frame,sentinel,current,prev,redCount?redX/redCount:-1,223.5+32*frame);
            if(frame<6||frame+1==int(frameCount)) save_rgba(outdir,"input",frame,pixels.data(),pixels.size()*4);if(frame<6||frame+1==int(frameCount)) save_rgba(outdir,"generated",frame,data,pixels.size()*4);
            if(frame%18!=0&&sentinel==0&&current<pixels.size()&&prev<pixels.size())++verified;previous=pixels;
            uint64_t stamps[2]{};CHECK(vkGetQueryPoolResults(dev,times,0,2,sizeof(stamps),stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));double gpuMs=double(stamps[1]-stamps[0])*properties.limits.timestampPeriod/1000000.0;double cpuMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-evaluateBegin).count();
            uint64_t heapUsage=0;VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};VkPhysicalDeviceMemoryProperties2 memory{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};memory.pNext=budgetAvailable?&budget:nullptr;vkGetPhysicalDeviceMemoryProperties2(pd,&memory);for(uint32_t i=0;i<memory.memoryProperties.memoryHeapCount;++i)if(memory.memoryProperties.memoryHeaps[i].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)heapUsage+=budget.heapUsage[i];PROCESS_MEMORY_COUNTERS_EX pm{};pm.cb=sizeof(pm);GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm),sizeof(pm));printf("METRIC frame=%d gpu_ms=%.6f cpu_wait_ms=%.6f vram_bytes=%llu ram_working_bytes=%llu reset=%d\n",frame,gpuMs,cpuMs,static_cast<unsigned long long>(heapUsage),static_cast<unsigned long long>(pm.WorkingSetSize),int(o.reset));
            if(presentation){if(!o.reset)CHECK(presenter.present(out.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,true));CHECK(presenter.present(back.image,VK_IMAGE_LAYOUT_GENERAL,false));}
            printf("FRAME_TIME frame=%d total_ms=%.6f\n",frame,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-frameBegin).count());

        }
        printf("SERIES seconds=%.6f real_frames=%u accepted_real_presents=%u accepted_generated_presents=%u\n",std::chrono::duration<double>(std::chrono::steady_clock::now()-seriesBegin).count(),frameCount,presenter.realPresents,presenter.generatedPresents);
        printf("VERIFIED_NONTRIVIAL_OUTPUTS=%u\n",verified);
        if(!verified)return 1;
        CHECK(vkDeviceWaitIdle(dev));
        NVSDK_NGX_VULKAN_ReleaseFeature(feature);feature=nullptr;
        for(Image* im:{&back,&depth,&mv,&out}){vkDestroyImageView(dev,im->view,nullptr);vkDestroyImage(dev,im->image,nullptr);vkFreeMemory(dev,im->mem,nullptr);}
        for(HostBuffer* b:{&upload[0],&upload[1],&upload[2],&upload[3],&readback}){vkUnmapMemory(dev,b->memory);vkDestroyBuffer(dev,b->buffer,nullptr);vkFreeMemory(dev,b->memory,nullptr);}
        vkDestroyQueryPool(dev,times,nullptr);
    }
    vkDeviceWaitIdle(dev);
    if(feature)NVSDK_NGX_VULKAN_ReleaseFeature(feature);
    NVSDK_NGX_VULKAN_DestroyParameters(params);
    NVSDK_NGX_VULKAN_DestroyParameters(caps);
    NVSDK_NGX_VULKAN_Shutdown1(dev);
    vkDestroyCommandPool(dev,pool,nullptr);vkDestroyDevice(dev,nullptr);
    if(messenger){auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(inst,"vkDestroyDebugUtilsMessengerEXT"));if(destroy)destroy(inst,messenger,nullptr);}
    vkDestroyInstance(inst,nullptr);
    if(validationRequested)printf("VALIDATION_SUMMARY errors=%u warnings=%u\n",validationErrors.load(),validationWarnings.load());
    printf("probe: done\n");
    return validationErrors.load()?1:0;
}
