#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <cstdio>
#include <string>
using Microsoft::WRL::ComPtr;
static void logMessage(sl::LogType type, const char* msg) { std::printf("SL[%d] %s\n", int(type), msg); }
static void modules() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    MODULEENTRY32W e{sizeof(e)};
    if (snap != INVALID_HANDLE_VALUE && Module32FirstW(snap, &e)) do {
        std::wstring n=e.szModule;
        if (n.find(L"sl.")!=n.npos || n.find(L"nvngx")!=n.npos || n.find(L"sm86")!=n.npos || n.find(L"kernel")!=n.npos || n==L"version.dll")
            std::printf("MODULE %ls\n", e.szExePath);
    } while (Module32NextW(snap,&e));
    if(snap!=INVALID_HANDLE_VALUE) CloseHandle(snap);
}
int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc<3) return 2;
    const wchar_t* sdk=argv[1]; const wchar_t* logdir=argv[2];
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS|LOAD_LIBRARY_SEARCH_USER_DIRS);
    AddDllDirectory(sdk);
    if(argc>3) {
        std::printf("ENTER proxy %ls\n",argv[3]);
        HMODULE proxy=LoadLibraryW(argv[3]);
        std::printf("LEAVE proxy=%p error=%lu\n",proxy,proxy?0:GetLastError());
        if(!proxy)return 2;
    }
    const auto interposer=LoadLibraryExW((std::wstring(sdk)+L"\\sl.interposer.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    std::printf("interposer=%p\n",interposer); if(!interposer)return 2;
    const auto init=reinterpret_cast<PFun_slInit*>(GetProcAddress(interposer,"slInit"));
    const auto stop=reinterpret_cast<PFun_slShutdown*>(GetProcAddress(interposer,"slShutdown"));
    const auto setDevice=reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(interposer,"slSetD3DDevice"));
    const auto supported=reinterpret_cast<PFun_slIsFeatureSupported*>(GetProcAddress(interposer,"slIsFeatureSupported"));
    const auto requirements=reinterpret_cast<PFun_slGetFeatureRequirements*>(GetProcAddress(interposer,"slGetFeatureRequirements"));
    const auto getFunction=reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(interposer,"slGetFeatureFunction"));
    if(!init||!stop||!setDevice||!supported||!requirements||!getFunction)return 2;
    sl::Feature features[]={sl::kFeatureDLSS_G,sl::kFeatureReflex,sl::kFeaturePCL};
    const wchar_t* paths[]={sdk};
    sl::Preferences prefs{};
    prefs.flags=sl::PreferenceFlags::eUseManualHooking|sl::PreferenceFlags::eDisableCLStateTracking|sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    if(argc>4&&std::wcscmp(argv[4],L"--ota")==0){prefs.flags|=sl::PreferenceFlags::eAllowOTA|sl::PreferenceFlags::eLoadDownloadedPlugins;std::printf("CONTROL_OTA_ENABLED\n");}
    prefs.featuresToLoad=features; prefs.numFeaturesToLoad=3;
    prefs.pathsToPlugins=paths; prefs.numPathsToPlugins=1;
    prefs.renderAPI=sl::RenderAPI::eD3D12;
    prefs.applicationId=0; prefs.projectId="31246941-7a24-484c-a4a7-e436c26c6f95";
    prefs.engine=sl::EngineType::eCustom; prefs.engineVersion="phase2.1";
    prefs.logLevel=sl::LogLevel::eVerbose; prefs.logMessageCallback=logMessage; prefs.pathToLogsAndData=logdir;
    std::printf("ENTER slInit no-OTA flags=%llu\n",static_cast<unsigned long long>(prefs.flags));
    auto r=init(prefs,sl::kSDKVersion); std::printf("LEAVE slInit=%d\n",int(r));
    if(r!=sl::Result::eOk){modules();return 1;}
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    std::printf("CreateDXGIFactory1=%08lx\n",hr); if(FAILED(hr))return 1;
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){
        ComPtr<IDXGIAdapter1> a;
        if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);
        if(d.VendorId==0x10de){adapter=a;std::printf("GPU %ls device=%04x\n",d.Description,d.DeviceId);break;}
    }
    if(!adapter)return 1;
    ComPtr<ID3D12Device> device;
    hr=D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device));
    std::printf("D3D12CreateDevice=%08lx device=%p\n",hr,device.Get()); if(FAILED(hr))return 1;
    auto luid=device->GetAdapterLuid();
    sl::AdapterInfo ai{};ai.deviceLUID=reinterpret_cast<uint8_t*>(&luid);ai.deviceLUIDSizeInBytes=sizeof(luid);
    std::printf("SUPPORT before-set=%d LUID=%08lx.%08lx bytes=%u\n",int(supported(sl::kFeatureDLSS_G,ai)),luid.HighPart,luid.LowPart,unsigned(sizeof(luid)));
    r=setDevice(device.Get());std::printf("slSetD3DDevice=%d\n",int(r));
    std::printf("SUPPORT after-set=%d\n",int(supported(sl::kFeatureDLSS_G,ai)));
    sl::FeatureRequirements req{};r=requirements(sl::kFeatureDLSS_G,req);
    std::printf("requirements=%d flags=%llu driver=%u.%u.%u\n",int(r),static_cast<unsigned long long>(req.flags),req.driverVersionDetected.major,req.driverVersionDetected.minor,req.driverVersionDetected.build);
    void* fn=nullptr;r=getFunction(sl::kFeatureDLSS_G,"slDLSSGSetOptions",fn);std::printf("resolve slDLSSGSetOptions=%d pointer=%p\n",int(r),fn);
    modules();
    stop();device.Reset();
    std::printf("DONE: no frame generation attempted\n");
    return 0;
}
