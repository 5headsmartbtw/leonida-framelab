#pragma once
#include <sl.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <psapi.h>
// A test presenter for directly computed NGX frames. No DLSS-G Streamline plugin is loaded.
// PCL/Reflex bracket real-frame work; this is not a validated input-latency measurement.
struct TestPresentation {
    HMODULE slModule{};HWND window{};Microsoft::WRL::ComPtr<IDXGISwapChain3> swap;
    PFun_slShutdown* stop{};PFun_slSetD3DDevice* setDevice{};PFun_slGetFeatureFunction* getFunction{};PFun_slGetNewFrameToken* newToken{};
    PFun_slPCLSetMarker* markerFn{};PFun_slReflexSleep* sleepFn{};PFun_slReflexSetOptions* reflexOptions{};
    sl::FrameToken* token{};unsigned acceptedReal=0,acceptedGenerated=0,occluded=0;
    static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
    void initSL(const std::wstring& sdk,const std::wstring& logs){
        slModule=LoadLibraryExW((sdk+L"\\sl.interposer.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);if(!slModule)throw std::runtime_error("Reflex interposer load");
        auto init=reinterpret_cast<PFun_slInit*>(GetProcAddress(slModule,"slInit"));stop=reinterpret_cast<PFun_slShutdown*>(GetProcAddress(slModule,"slShutdown"));setDevice=reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(slModule,"slSetD3DDevice"));getFunction=reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(slModule,"slGetFeatureFunction"));newToken=reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(slModule,"slGetNewFrameToken"));
        if(!init||!stop||!setDevice||!getFunction||!newToken)throw std::runtime_error("SL API exports");
        const sl::Feature f[]={sl::kFeatureReflex,sl::kFeaturePCL};const wchar_t* paths[]={sdk.c_str()};sl::Preferences p{};p.flags=sl::PreferenceFlags::eUseManualHooking|sl::PreferenceFlags::eDisableCLStateTracking;p.featuresToLoad=f;p.numFeaturesToLoad=2;p.pathsToPlugins=paths;p.numPathsToPlugins=1;p.renderAPI=sl::RenderAPI::eD3D12;p.applicationId=0;p.projectId="31246941-7a24-484c-a4a7-e436c26c6f95";p.engine=sl::EngineType::eCustom;p.engineVersion="phase2.present";p.pathToLogsAndData=logs.c_str();
        auto r=init(p,sl::kSDKVersion);printf("PRESENT_SL_INIT=%d\n",int(r));if(r!=sl::Result::eOk)throw std::runtime_error("Reflex/PCL init");
    }
    void create(ID3D12Device* device,ID3D12CommandQueue* queue,UINT width,UINT height){
        auto r=setDevice(device);printf("PRESENT_SL_DEVICE=%d\n",int(r));if(r!=sl::Result::eOk)throw std::runtime_error("Reflex/PCL device");
        void* f=nullptr;if(getFunction(sl::kFeaturePCL,"slPCLSetMarker",f)!=sl::Result::eOk)throw std::runtime_error("PCL marker");markerFn=reinterpret_cast<PFun_slPCLSetMarker*>(f);
        f=nullptr;if(getFunction(sl::kFeatureReflex,"slReflexSleep",f)!=sl::Result::eOk)throw std::runtime_error("Reflex sleep");sleepFn=reinterpret_cast<PFun_slReflexSleep*>(f);
        f=nullptr;if(getFunction(sl::kFeatureReflex,"slReflexSetOptions",f)!=sl::Result::eOk)throw std::runtime_error("Reflex options");reflexOptions=reinterpret_cast<PFun_slReflexSetOptions*>(f);sl::ReflexOptions o{};o.mode=sl::ReflexMode::eLowLatency;o.useMarkersToOptimize=false;r=reflexOptions(o);printf("PRESENT_REFLEX_OPTIONS=%d\n",int(r));
        WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"GTA6SyntheticFGCheck";RegisterClassW(&wc);
        window=CreateWindowW(wc.lpszClassName,L"RTX 2050 NVIDIA FG 2X synthetic check",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,656,399,nullptr,nullptr,wc.hInstance,nullptr);if(!window)throw std::runtime_error("test window");ShowWindow(window,SW_SHOWNOACTIVATE);
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));DXGI_SWAP_CHAIN_DESC1 d{};d.Width=width;d.Height=height;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=2;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;Microsoft::WRL::ComPtr<IDXGISwapChain1> first;HR(factory->CreateSwapChainForHwnd(queue,window,&d,nullptr,nullptr,&first));HR(first.As(&swap));
    }
    void begin(unsigned frame){uint32_t id=frame;auto r=newToken(token,&id);if(r!=sl::Result::eOk)throw std::runtime_error("Frame token");r=sleepFn(*token);if(r!=sl::Result::eOk)printf("REFLEX_SLEEP_ERROR=%d\n",int(r));}
    void mark(sl::PCLMarker m){auto r=markerFn(m,*token);if(r!=sl::Result::eOk)printf("PCL_ERROR=%d\n",int(r));}
    template<class Context,class Texture> void present(Context& c,Texture& image,bool generated){
        MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        Microsoft::WRL::ComPtr<ID3D12Resource> target;HR(swap->GetBuffer(swap->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&target)));c.begin();c.transition(image,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={target.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST};c.list->ResourceBarrier(1,&b);c.list->CopyResource(target.Get(),image.resource.Get());std::swap(b.Transition.StateBefore,b.Transition.StateAfter);c.list->ResourceBarrier(1,&b);c.submit();HRESULT hr=swap->Present(1,0);HR(hr);if(hr==DXGI_STATUS_OCCLUDED)++occluded;else if(generated)++acceptedGenerated;else ++acceptedReal;
    }
    void shutdown(){swap.Reset();if(window){DestroyWindow(window);window=nullptr;}if(stop){stop();stop=nullptr;}}
    ~TestPresentation(){shutdown();}
};
