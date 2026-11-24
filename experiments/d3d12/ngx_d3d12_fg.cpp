// Independent synthetic NGX D3D12 check, informed by dlssg-open's MIT probe.
// NVIDIA SDK, runtime and kernels are external dependencies; none is embedded here.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_dlssg_d3d.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <stdexcept>
#include <chrono>
using Microsoft::WRL::ComPtr;
constexpr UINT W=1280,H=720;
static FILE* ngxLogFile=nullptr;
static void NVSDK_CONV ngxLog(const char* s,NVSDK_NGX_Logging_Level,NVSDK_NGX_Feature){if(ngxLogFile){fputs(s,ngxLogFile);fputc('\n',ngxLogFile);fflush(ngxLogFile);}}
static void hrcheck(HRESULT hr,const char* name){if(FAILED(hr)){printf("FAIL %s hr=%08lx\n",name,hr);throw std::runtime_error(name);}}
#define HR(x) hrcheck((x),#x)
#include "d3d12_presentation.h"
#include "../../src/backends/d3d12/frame_bindings.hpp"

namespace fi=frame_inputs;
namespace bindings=framelab::d3d12;

static std::shared_ptr<ID3D12Resource> pin(ID3D12Resource* resource) {
    resource->AddRef();
    return {resource,[](ID3D12Resource* owned){ owned->Release(); }};
}

static fi::Camera syntheticCamera() {
    fi::Camera camera;
    camera.verified=true;camera.cameraMotionIncluded=true;
    camera.nearPlane=0.1f;camera.farPlane=1000;camera.fovRadians=1;camera.aspect=float(W)/H;
    camera.up[1]=camera.right[0]=camera.forward[2]=1;
    camera.projectionSpace=camera.reprojectionSpace=fi::ClipSpace::y_up_zero_to_one;
    camera.motion=fi::MotionConvention::previous_minus_current_uv;
    DirectX::XMFLOAT4X4 proj,inv,ident;
    auto projection=DirectX::XMMatrixPerspectiveFovLH(camera.fovRadians,camera.aspect,camera.nearPlane,camera.farPlane);
    DirectX::XMStoreFloat4x4(&proj,projection);
    DirectX::XMStoreFloat4x4(&inv,DirectX::XMMatrixInverse(nullptr,projection));
    DirectX::XMStoreFloat4x4(&ident,DirectX::XMMatrixIdentity());
    memcpy(camera.viewToClip.data(),&proj,sizeof(proj));
    memcpy(camera.clipToView.data(),&inv,sizeof(inv));
    for(auto* matrix:{&camera.clipToLens,&camera.clipToPrevious,&camera.previousToClip})
        memcpy(matrix->data(),&ident,sizeof(ident));
    return camera;
}

static NVSDK_NGX_DLSSG_Opt_Eval_Params evaluateOptions(const fi::FrameInputs& frame) {
    const auto& c=frame.camera;
    NVSDK_NGX_DLSSG_Opt_Eval_Params options{};
    options.multiFrameCount=options.multiFrameIndex=1;
    options.cameraNear=c.nearPlane;options.cameraFar=c.farPlane;
    options.cameraFOV=c.fovRadians;options.cameraAspectRatio=c.aspect;
    memcpy(options.cameraPos,c.position.data(),sizeof(options.cameraPos));
    memcpy(options.cameraUp,c.up.data(),sizeof(options.cameraUp));
    memcpy(options.cameraRight,c.right.data(),sizeof(options.cameraRight));
    memcpy(options.cameraFwd,c.forward.data(),sizeof(options.cameraFwd));
    memcpy(options.mvecScale,c.motionVectorScale.data(),sizeof(options.mvecScale));
    memcpy(options.cameraViewToClip,c.viewToClip.data(),sizeof(options.cameraViewToClip));
    memcpy(options.clipToCameraView,c.clipToView.data(),sizeof(options.clipToCameraView));
    memcpy(options.clipToLensClip,c.clipToLens.data(),sizeof(options.clipToLensClip));
    memcpy(options.clipToPrevClip,c.clipToPrevious.data(),sizeof(options.clipToPrevClip));
    memcpy(options.prevClipToClip,c.previousToClip.data(),sizeof(options.prevClipToClip));
    options.reset=frame.resetHistory!=fi::Reset::none;
    options.depthInverted=c.depthInverted;options.cameraMotionIncluded=c.cameraMotionIncluded;
    options.orthoProjection=c.orthographic;options.motionVectorsInvalidValue=1e30f;
    options.mvecsSubrectSize={frame.motionVectors.extent.width,frame.motionVectors.extent.height};
    options.depthSubrectSize={frame.depth.extent.width,frame.depth.extent.height};
    options.backbufferSubrectSize={frame.color.extent.width,frame.color.extent.height};
    options.outputInterpSubrectSize={frame.outputExtent.width,frame.outputExtent.height};
    return options;
}
struct Texture {ComPtr<ID3D12Resource> resource;D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COPY_DEST;};
struct Context {
    ComPtr<IDXGIAdapter3> adapter3;ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 value=0;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    void init(){
        ComPtr<IDXGIFactory1> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        for(UINT i=0;;++i){ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId!=0x10de)continue;HR(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));HR(a.As(&adapter3));printf("GPU %ls\n",d.Description);break;}
        if(!device)throw std::runtime_error("NVIDIA device absent");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;HR(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        HR(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&alloc)));HR(device->CreateCommandList(0,q.Type,alloc.Get(),nullptr,IID_PPV_ARGS(&list)));HR(list->Close());HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    }
    void begin(){uploads.clear();HR(alloc->Reset());HR(list->Reset(alloc.Get(),nullptr));}
    void submit(){HR(list->Close());ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);HR(queue->Signal(fence.Get(),++value));HR(fence->SetEventOnCompletion(value,event));if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU fence timeout");HR(device->GetDeviceRemovedReason());}
    Texture texture(DXGI_FORMAT format){Texture t;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=W;d.Height=H;d.DepthOrArraySize=1;d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,t.state,nullptr,IID_PPV_ARGS(&t.resource)));return t;}
    ComPtr<ID3D12Resource> buffer(UINT64 bytes,D3D12_HEAP_TYPE type){ComPtr<ID3D12Resource>b;D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;auto state=type==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_GENERIC_READ;HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&b)));return b;}
    void transition(Texture& t,D3D12_RESOURCE_STATES state){if(t.state==state)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={t.resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,t.state,state};list->ResourceBarrier(1,&b);t.state=state;}
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint(Texture& t,UINT64& size){auto d=t.resource->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&size);return fp;}
    void upload(Texture& t,const void* bytes,UINT rowBytes,D3D12_RESOURCE_STATES after){UINT64 size;auto fp=footprint(t,size);auto up=buffer(size,D3D12_HEAP_TYPE_UPLOAD);void* mapped=nullptr;HR(up->Map(0,nullptr,&mapped));for(UINT y=0;y<H;++y)memcpy((char*)mapped+fp.Offset+y*fp.Footprint.RowPitch,(const char*)bytes+y*rowBytes,rowBytes);up->Unmap(0,nullptr);transition(t,D3D12_RESOURCE_STATE_COPY_DEST);D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=t.resource.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=up.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);transition(t,after);uploads.push_back(up);}
    ComPtr<ID3D12Resource> readback(Texture& t,D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp){UINT64 size;fp=footprint(t,size);auto rb=buffer(size,D3D12_HEAP_TYPE_READBACK);transition(t,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=t.resource.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);return rb;}
    ~Context(){if(event)CloseHandle(event);}
};
static void writeBytes(const std::wstring& file,const void* data,size_t bytes){FILE*f=nullptr;_wfopen_s(&f,file.c_str(),L"wb");if(!f)throw std::runtime_error("open output");fwrite(data,1,bytes,f);fclose(f);}
int wmain(int argc,wchar_t**argv){
    setvbuf(stdout,nullptr,_IONBF,0);if(argc<4){printf("usage: ngx_d3d12_fg <SDK runtime dir> <output dir> <proxy path or ->\n");return 2;}
    try{
        std::wstring sdk=argv[1],outdir=argv[2];CreateDirectoryW(outdir.c_str(),nullptr);
        _wfopen_s(&ngxLogFile,(outdir+L"\\ngx-detail.log").c_str(),L"wb");
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS|LOAD_LIBRARY_SEARCH_USER_DIRS);AddDllDirectory(sdk.c_str());
        if(wcscmp(argv[3],L"-")!=0){auto m=LoadLibraryW(argv[3]);printf("proxy=%p error=%lu\n",m,m?0:GetLastError());if(!m)return 2;}
        unsigned frameCount=argc>4?unsigned(_wtoi(argv[4])):6;
        bool presentation=argc>5&&wcscmp(argv[5],L"present")==0;
        TestPresentation presenter;if(presentation)presenter.initSL(sdk,outdir);
        Context c;c.init();if(presentation)presenter.create(c.device.Get(),c.queue.Get(),W,H);
        const wchar_t*paths[]={sdk.c_str()};NVSDK_NGX_FeatureCommonInfo info{};info.PathListInfo.Path=paths;info.PathListInfo.Length=1;info.LoggingInfo.LoggingCallback=ngxLog;info.LoggingInfo.MinimumLoggingLevel=NVSDK_NGX_LOGGING_LEVEL_VERBOSE;info.LoggingInfo.DisableOtherLoggingSinks=true;
        printf("ENTER NGX_D3D12_Init\n");
        auto r=NVSDK_NGX_D3D12_Init_with_ProjectID("31246941-7a24-484c-a4a7-e436c26c6f95",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"phase2.1",outdir.c_str(),c.device.Get(),&info,NVSDK_NGX_Version_API);
        printf("NGX init=%08x\n",unsigned(r));if(NVSDK_NGX_FAILED(r))return 1;
        NVSDK_NGX_Parameter*caps=nullptr;r=NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);printf("NGX capabilities=%08x\n",unsigned(r));if(NVSDK_NGX_FAILED(r)||!caps)return 1;
        int available=0;caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available,&available);printf("FrameGeneration.Available=%d\n",available);
        if(available!=1){
            NVSDK_NGX_D3D12_DestroyParameters(caps);
            NVSDK_NGX_D3D12_Shutdown1(c.device.Get());
            if(presentation)presenter.shutdown();
            if(ngxLogFile)fclose(ngxLogFile);
            return 3;
        }
        NVSDK_NGX_D3D12_DestroyParameters(caps);
        NVSDK_NGX_Parameter*params=nullptr;r=NVSDK_NGX_D3D12_AllocateParameters(&params);if(NVSDK_NGX_FAILED(r))return 1;
        NVSDK_NGX_DLSSG_Create_Params cp{};cp.Width=W;cp.Height=H;cp.RenderWidth=W;cp.RenderHeight=H;cp.NativeBackbufferFormat=DXGI_FORMAT_R8G8B8A8_UNORM;
        params->Set("DLSSG.SynchronousInit",1u);params->Set("DLSSG.EnableInterp",1u);
        NVSDK_NGX_Handle*feature=nullptr;c.begin();printf("ENTER CreateFeature\n");r=NGX_D3D12_CREATE_DLSSG(c.list.Get(),1,1,&feature,params,&cp);printf("CreateFeature=%08x handle=%p\n",unsigned(r),feature);c.submit();if(NVSDK_NGX_FAILED(r)||!feature)return 1;
        auto color=c.texture(DXGI_FORMAT_R8G8B8A8_UNORM),depth=c.texture(DXGI_FORMAT_R32_FLOAT),mv=c.texture(DXGI_FORMAT_R16G16_FLOAT),output=c.texture(DXGI_FORMAT_R8G8B8A8_UNORM);
        std::vector<uint32_t> pixels(W*H),sentinel(W*H,0xffff00ff),previous;std::vector<float> depths(W*H);std::vector<uint16_t> vectors(W*H*2);
        ComPtr<ID3D12QueryHeap> queries;D3D12_QUERY_HEAP_DESC qh{};qh.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qh.Count=2;HR(c.device->CreateQueryHeap(&qh,IID_PPV_ARGS(&queries)));auto timeReadback=c.buffer(16,D3D12_HEAP_TYPE_READBACK);UINT64 frequency=0;HR(c.queue->GetTimestampFrequency(&frequency));
        using bindings::ImageBinding;
        ImageBinding colorBinding{11,pin(color.resource.Get()),color.state};
        ImageBinding depthBinding{12,pin(depth.resource.Get()),depth.state};
        ImageBinding motionBinding{13,pin(mv.resource.Get()),mv.state};
        ImageBinding outputBinding{14,pin(output.resource.Get()),output.state};
        const bindings::RecordContext recording{1,1,1,*c.device.Get(),*c.queue.Get(),*c.list.Get()};
        const auto camera=syntheticCamera();
        fi::Sequence history;
        auto previousFrameTime=std::chrono::steady_clock::time_point{};
        unsigned verified=0;auto seriesBegin=std::chrono::steady_clock::now();
        for(unsigned frame=0;frame<frameCount;++frame){
            auto frameBegin=std::chrono::steady_clock::now();
            const unsigned left=160+(frame%18)*32;
            if(presentation){presenter.begin(frame);presenter.mark(sl::PCLMarker::eSimulationStart);}
            for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){unsigned p=y*W+x;bool obj=x>=left&&x<left+160&&y>=240&&y<480;bool grid=((x/32+y/32)%2)!=0;pixels[p]=obj?0xff2080f0:(grid?0xff303030:0xff181818);depths[p]=obj?0.3f:0.9f;vectors[2*p]=DirectX::PackedVector::XMConvertFloatToHalf(obj?-32.0f/W:0.0f);vectors[2*p+1]=0;}
            if(presentation){presenter.mark(sl::PCLMarker::eSimulationEnd);presenter.mark(sl::PCLMarker::eRenderSubmitStart);}
            c.begin();c.upload(color,pixels.data(),W*4,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);c.upload(depth,depths.data(),W*4,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);c.upload(mv,vectors.data(),W*4,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);c.upload(output,sentinel.data(),W*4,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            colorBinding.state=color.state;depthBinding.state=depth.state;
            motionBinding.state=mv.state;outputBinding.state=output.state;
            fi::FrameInputs inputs;
            inputs.stream=1;inputs.device=recording.deviceId;inputs.generation=1;inputs.frameIndex=frame;
            inputs.renderExtent=inputs.outputExtent={W,H};
            inputs.deltaSeconds=frame ? std::chrono::duration<double>(frameBegin-previousFrameTime).count() : 1.0/60.0;
            previousFrameTime=frameBegin;
            const auto describe=[&](const ImageBinding& binding,fi::Format format){
                return fi::Image{binding.id,frame,recording.queueId,format,{W,H},fi::State::shader_read,
                    fi::Provenance::synthetic_renderer,binding.resource};
            };
            inputs.color=describe(colorBinding,fi::Format::rgba8_unorm);
            inputs.depth=describe(depthBinding,fi::Format::r32_float);
            inputs.motionVectors=describe(motionBinding,fi::Format::rg16_float);
            inputs.camera=camera;
            inputs.sync={fi::Completion::same_command_stream,recording.queueId,recording.commandStreamId};
            if(frame==0)inputs.resetHistory=fi::Reset::initial;
            else if(frame%18==0)inputs.resetHistory=fi::Reset::discontinuity;
            if(frame==2){
                auto stale=inputs;
                --stale.depth.frameIndex;
                if(bindings::check(stale,recording,colorBinding,depthBinding,motionBinding,outputBinding).error!=fi::Error::sequence)
                    throw std::runtime_error("stale D3D12 depth was not rejected");
                printf("D3D12_STALE_DEPTH_REJECTED_BEFORE_EVALUATE\n");
            }
            if(const auto result=history.check(inputs);!result)throw std::runtime_error(std::string(result.reason));
            if(const auto result=bindings::check(inputs,recording,colorBinding,depthBinding,motionBinding,outputBinding);!result)
                throw std::runtime_error(std::string(result.reason));
            NVSDK_NGX_D3D12_DLSSG_Eval_Params ep{};
            ep.pBackbuffer=colorBinding.resource.get();ep.pDepth=depthBinding.resource.get();
            ep.pMVecs=motionBinding.resource.get();ep.pOutputInterpFrame=outputBinding.resource.get();
            auto o=evaluateOptions(inputs);
            params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,static_cast<unsigned long long>(frame+1));params->Set("DLSSG.EnableInterp",1u);params->Set("DLSSG.IsRecording",0u);params->Set("DLSSG.FlushRequired",0u);
            printf("ENTER Evaluate frame=%u\n",frame);auto begin=std::chrono::steady_clock::now();c.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);r=NGX_D3D12_EVALUATE_DLSSG(c.list.Get(),feature,params,&ep,&o);c.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);c.list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timeReadback.Get(),0);printf("Evaluate frame=%u result=%08x\n",frame,unsigned(r));
            D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;uav.UAV.pResource=output.resource.Get();c.list->ResourceBarrier(1,&uav);D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};auto rb=c.readback(output,fp);c.submit();
            double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();if(NVSDK_NGX_FAILED(r))return 1;
            // submit() waited on the actual queue fence; the packet and its owners are still alive here.
            history.accept(inputs);
            printf("D3D12_FRAME_COMPLETED frame=%u fence=%llu\n",frame,c.value);
            void*mapped=nullptr;HR(rb->Map(0,nullptr,&mapped));std::vector<uint32_t>read(W*H);for(unsigned y=0;y<H;++y)memcpy(read.data()+y*W,(char*)mapped+fp.Offset+y*fp.Footprint.RowPitch,W*4);rb->Unmap(0,nullptr);
            size_t unchanged=0,sameCurrent=0,samePrevious=0;for(size_t i=0;i<read.size();++i){unchanged+=read[i]==sentinel[i];sameCurrent+=read[i]==pixels[i];samePrevious+=!previous.empty()&&read[i]==previous[i];}
            printf("READBACK frame=%u sentinel=%zu/%zu equal_current=%zu equal_previous=%zu cpu_wait_ms=%.3f\n",frame,unchanged,read.size(),sameCurrent,samePrevious,elapsed);
            if(frame<6||frame+1==frameCount) writeBytes(outdir+L"\\input-"+std::to_wstring(frame)+L".rgba",pixels.data(),pixels.size()*4);if(frame<6||frame+1==frameCount) writeBytes(outdir+L"\\generated-"+std::to_wstring(frame)+L".rgba",read.data(),read.size()*4);
            if(frame%18!=0&&unchanged==0&&sameCurrent<read.size()&&samePrevious<read.size())++verified;previous=pixels;
            UINT64* stamps=nullptr;HR(timeReadback->Map(0,nullptr,reinterpret_cast<void**>(&stamps)));double gpuMs=double(stamps[1]-stamps[0])*1000.0/frequency;timeReadback->Unmap(0,nullptr);DXGI_QUERY_VIDEO_MEMORY_INFO memory{};HR(c.adapter3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory));PROCESS_MEMORY_COUNTERS_EX pm{};pm.cb=sizeof(pm);GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm),sizeof(pm));printf("METRIC frame=%u gpu_ms=%.6f cpu_wait_ms=%.6f vram_bytes=%llu ram_working_bytes=%llu reset=%d\n",frame,gpuMs,elapsed,static_cast<unsigned long long>(memory.CurrentUsage),static_cast<unsigned long long>(pm.WorkingSetSize),int(o.reset));
            if(presentation){presenter.mark(sl::PCLMarker::eRenderSubmitEnd);presenter.mark(sl::PCLMarker::ePresentStart);if(!o.reset)presenter.present(c,output,true);presenter.present(c,color,false);presenter.mark(sl::PCLMarker::ePresentEnd);}
            printf("FRAME_TIME frame=%u total_ms=%.6f\n",frame,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-frameBegin).count());

        }
        printf("SERIES seconds=%.6f real_frames=%u accepted_real_presents=%u accepted_generated_presents=%u occluded=%u\n",std::chrono::duration<double>(std::chrono::steady_clock::now()-seriesBegin).count(),frameCount,presenter.acceptedReal,presenter.acceptedGenerated,presenter.occluded);
        printf("VERIFIED_NONTRIVIAL_OUTPUTS=%u\n",verified);NVSDK_NGX_D3D12_ReleaseFeature(feature);NVSDK_NGX_D3D12_DestroyParameters(params);NVSDK_NGX_D3D12_Shutdown1(c.device.Get());if(presentation)presenter.shutdown();if(ngxLogFile)fclose(ngxLogFile);return verified?0:1;
    }catch(const std::exception&e){printf("EXCEPTION %s\n",e.what());return 1;}
}
