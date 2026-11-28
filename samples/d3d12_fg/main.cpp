// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <framelab/d3d12_fg.h>

#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
using Clock = std::chrono::steady_clock;
constexpr FrameLabExtent initialExtent{1280, 720};
constexpr FrameLabExtent resizedExtent{960, 540};
constexpr unsigned readbackFrames = 6;
constexpr unsigned recreatedFrames = 3;
constexpr uint32_t sentinel = 0xffff00ff;
constexpr HRESULT occluded = DXGI_STATUS_OCCLUDED;

bool operator==(FrameLabExtent a, FrameLabExtent b) { return a.width == b.width && a.height == b.height; }

struct GpuError : std::runtime_error {
    HRESULT result;
    GpuError(HRESULT value, const char* step) : std::runtime_error(step), result(value) {}
};

void check(HRESULT result, const char* step)
{
    if (SUCCEEDED(result)) return;
    throw GpuError(result, step);
}

void sdk(FrameLabStatus result, const char* step)
{
    if (result == FRAMELAB_OK) return;
    std::fprintf(stderr, "%s: %s\n", step, framelab_status_string(result));
    throw std::runtime_error(step);
}

void expect(bool condition, const char* detail)
{
    if (!condition) throw std::runtime_error(detail);
}

bool environment(const wchar_t* name) { return GetEnvironmentVariableW(name, nullptr, 0) != 0; }

void FRAMELAB_CALL reportInit(void*, const char* operation, uint32_t result)
{
    std::printf("SDK_FG_INIT operation=%s ngx=%#x\n", operation, result);
}

// Command line. The first three positional arguments are unchanged from the readback client.
struct Options {
    std::filesystem::path runtime, logs;
    const wchar_t* proxy = L"-";
    bool presenting = false, vsync = false, noFg = false, resize = false, reject = false, debug = false;
    bool timing = false, pace = true, gpuValidation = false;
    unsigned frames = readbackFrames;
    FrameLabExtent render{};
    int cutAt = -1;
};

struct Texture {
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
};

struct Validation {
    ComPtr<ID3D12InfoQueue> queue;
    unsigned errors = 0, warnings = 0;

    void drain()
    {
        if (!queue) return;
        const auto count = queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            if (FAILED(queue->GetMessage(i, nullptr, &size)) || !size) continue;
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(queue->GetMessage(i, message, &size))) continue;
            const bool error = message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                message->Severity == D3D12_MESSAGE_SEVERITY_ERROR;
            const bool warning = message->Severity == D3D12_MESSAGE_SEVERITY_WARNING;
            errors += error;
            warnings += warning;
            if (error || warning)
                std::printf("D3D12_DEBUG severity=%d id=%d %s\n", int(message->Severity), int(message->ID),
                            message->pDescription);
        }
        queue->ClearStoredMessages();
    }
};

struct Memory {
    uint64_t start = 0, warm = 0, end = 0, cleanup = 0, growth = 0, peak = 0, budget = UINT64_MAX;
    std::vector<uint64_t> steady;
};

struct Gpu {
    FrameLabExtent extent = initialExtent;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    Validation validation;
    Memory memory;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t signal = 0;
    UINT64 timestampFrequency = 0;
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestampBuffer;

    void initializeTiming()
    {
        D3D12_QUERY_HEAP_DESC desc{};
        desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        desc.Count = 2;
        check(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&timestamps)), "create timestamp queries");
        timestampBuffer = buffer(2 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK);
        check(queue->GetTimestampFrequency(&timestampFrequency), "query timestamp frequency");
    }

    void startTiming()
    {
        if (timestamps) list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    }

    void stopTiming()
    {
        if (!timestamps) return;
        list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timestampBuffer.Get(), 0);
    }

    double elapsedGpuMs()
    {
        void* data{};
        const D3D12_RANGE range{0, 2 * sizeof(UINT64)};
        check(timestampBuffer->Map(0, &range, &data), "map timestamp readback");
        const auto* values = static_cast<const UINT64*>(data);
        const double elapsed = 1000.0 * double(values[1] - values[0]) / double(timestampFrequency);
        const D3D12_RANGE written{0, 0};
        timestampBuffer->Unmap(0, &written);
        return elapsed;
    }

    ~Gpu() { if (event) CloseHandle(event); }

    void initialize(bool debugLayer, bool gpuValidation)
    {
        expect(event != nullptr, "create fence event");
        if (debugLayer) {
            ComPtr<ID3D12Debug> debug;
            check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "get D3D12 debug layer");
            debug->EnableDebugLayer();
            if (gpuValidation) {
                ComPtr<ID3D12Debug1> debug1;
                check(debug.As(&debug1), "get GPU-based validation interface");
                debug1->SetEnableGPUBasedValidation(TRUE);
            }
        }
        ComPtr<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create DXGI factory");
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            const auto enumerated = factory->EnumAdapters1(i, &candidate);
            if (enumerated == DXGI_ERROR_NOT_FOUND) break;
            check(enumerated, "enumerate DXGI adapter");
            DXGI_ADAPTER_DESC1 desc{};
            check(candidate->GetDesc1(&desc), "read adapter description");
            if (desc.VendorId != 0x10de) continue;
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
                adapter = candidate;
                LARGE_INTEGER umd{};
                const bool version = SUCCEEDED(candidate->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
                std::printf("GPU %ls\n", desc.Description);
                std::printf("CAPS device_id=%#x dedicated_mb=%llu umd=%u.%u.%u.%u\n", desc.DeviceId,
                            static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20),
                            version ? unsigned(HIWORD(umd.HighPart)) : 0, version ? unsigned(LOWORD(umd.HighPart)) : 0,
                            version ? unsigned(HIWORD(umd.LowPart)) : 0, version ? unsigned(LOWORD(umd.LowPart)) : 0);
                break;
            }
        }
        expect(device.Get() != nullptr, "NVIDIA D3D12 device unavailable");
        if (debugLayer) {
            check(device.As(&validation.queue), "query D3D12 info queue");
            validation.queue->SetMessageCountLimit(16384);
        }
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "create queue");
        check(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator)), "create allocator");
        check(device->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "create list");
        check(list->Close(), "close initial list");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create completion fence");
        memory.start = sampleMemory();
    }

    // Returns the process's current local-segment usage and records the peak and current budget.
    uint64_t sampleMemory()
    {
        ComPtr<IDXGIAdapter3> adapter3;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (!adapter || FAILED(adapter.As(&adapter3)) ||
            FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return 0;
        memory.peak = std::max<uint64_t>(memory.peak, info.CurrentUsage);
        memory.budget = std::min<uint64_t>(memory.budget, info.Budget);
        return info.CurrentUsage;
    }

    void begin()
    {
        uploads.clear();
        check(allocator->Reset(), "reset allocator");
        check(list->Reset(allocator.Get(), nullptr), "reset list");
    }

    void enqueue(FrameLabD3D12Fg* fg)
    {
        check(list->Close(), "close submitted list");
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), ++signal), "signal actual consumer fence");
        if (fg) sdk(framelab_d3d12_fg_mark_submitted(fg, signal), "acknowledge submission");
    }

    void wait()
    {
        check(fence->SetEventOnCompletion(signal, event), "watch fence");
        expect(WaitForSingleObject(event, 30'000) == WAIT_OBJECT_0, "GPU fence timed out");
        check(device->GetDeviceRemovedReason(), "check device after fence");
    }

    void drain()
    {
        check(queue->Signal(fence.Get(), ++signal), "signal presentation drain");
        wait();
    }

    Texture texture(DXGI_FORMAT format, FrameLabExtent extent)
    {
        Texture output;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = extent.width;
        desc.Height = extent.height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, output.state,
                                               nullptr, IID_PPV_ARGS(&output.resource)), "create texture");
        return output;
    }

    ComPtr<ID3D12Resource> buffer(uint64_t bytes, D3D12_HEAP_TYPE type)
    {
        ComPtr<ID3D12Resource> output;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto state = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ :
                                                           D3D12_RESOURCE_STATE_COPY_DEST;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                               nullptr, IID_PPV_ARGS(&output)), "create transfer buffer");
        return output;
    }

    void transition(Texture& texture, D3D12_RESOURCE_STATES state)
    {
        if (texture.state == state) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {texture.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, texture.state, state};
        list->ResourceBarrier(1, &barrier);
        texture.state = state;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint(Texture& texture, uint64_t& size)
    {
        const auto desc = texture.resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT result{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &result, nullptr, nullptr, &size);
        return result;
    }

    void upload(Texture& texture, const void* data, D3D12_RESOURCE_STATES finalState)
    {
        uint64_t size{};
        const auto layout = footprint(texture, size);
        auto input = buffer(size, D3D12_HEAP_TYPE_UPLOAD);
        void* mapped{};
        check(input->Map(0, nullptr, &mapped), "map input buffer");
        const size_t rowBytes = size_t(layout.Footprint.Width) * sizeof(uint32_t);
        for (unsigned y = 0; y < layout.Footprint.Height; ++y)
            std::memcpy(static_cast<char*>(mapped) + layout.Offset + y * layout.Footprint.RowPitch,
                        static_cast<const char*>(data) + y * rowBytes, rowBytes);
        input->Unmap(0, nullptr);
        transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION src{input.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = layout;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(texture, finalState);
        uploads.push_back(std::move(input));
    }

    ComPtr<ID3D12Resource> readback(Texture& texture, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout)
    {
        uint64_t size{};
        layout = footprint(texture, size);
        auto result = buffer(size, D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = texture.resource.Get();
        list->ResourceBarrier(1, &uav);
        transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst{result.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = layout;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        return result;
    }
};

double percentile(std::vector<double> values, double p)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>(p * double(values.size() - 1) + 0.5);
    return values[std::min(index, values.size() - 1)];
}

struct Intervals {
    std::vector<double> realToGenerated, generatedToReal, realToReal, copyGpu;
};

enum class Kind { real, generated };

// Application-owned DXGI flip-model presentation of real frames and Direct NGX generated frames.
// FrameLab does not create, own or present the swapchain.
struct Gate {
    ComPtr<ID3D12Fence> fence;
    ~Gate() { if (fence) fence->Signal(1); }
};

struct Presenter {
    Gpu* gpuOwner = nullptr;
    HWND window{};
    ComPtr<IDXGISwapChain3> swapchain;
    bool vsync = false, tearing = false, pace = true;
    unsigned realAttempts{}, generatedAttempts{}, realPresents{}, generatedPresents{}, occludedPresents{};
    unsigned refreshHz{}, displayWidth{}, displayHeight{}, pendingResizeChecks{};
    Clock::time_point lastPresent{};
    Kind lastKind = Kind::real;
    Intervals intervals;
    DXGI_FRAME_STATISTICS firstStats{}, lastStats{};
    bool firstStatsValid = false, lastStatsValid = false;
    HRESULT statsResult = S_OK;

    ~Presenter()
    {
        if (swapchain) {
            // On an error, still drain queued Presents if the device remains usable.
            try { gpuOwner->drain(); } catch (const std::runtime_error&) {}
        }
        teardown();
    }

    void teardown()
    {
        swapchain.Reset();
        if (window) DestroyWindow(window);
        window = nullptr;
    }

    void initialize(Gpu& gpu, bool sync, bool paced)
    {
        gpuOwner = &gpu;
        vsync = sync;
        pace = paced;
        const auto extent = gpu.extent;
        RECT rect{0, 0, LONG(extent.width), LONG(extent.height)};
        constexpr auto style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        expect(AdjustWindowRect(&rect, style, FALSE) != 0, "size presentation window");
        window = CreateWindowExW(0, L"STATIC", L"FrameLab D3D12 FG", style | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        expect(window != nullptr, "create presentation window");
        ComPtr<IDXGIFactory5> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create swapchain factory");
        BOOL supported = FALSE;
        const bool tearingSupported = SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &supported, sizeof supported)) && supported;
        tearing = !vsync && tearingSupported;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = extent.width;
        desc.Height = extent.height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> chain;
        check(factory->CreateSwapChainForHwnd(gpu.queue.Get(), window, &desc, nullptr, nullptr, &chain),
              "create DXGI flip swapchain");
        check(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "disable exclusive fullscreen");
        check(chain.As(&swapchain), "query swapchain backbuffer index");
        // The display mode of the monitor that holds the window. On hybrid laptops this can be an output of the
        // integrated adapter; DXGI then copies across adapters, which the API cannot report.
        MONITORINFOEXW monitor{};
        monitor.cbSize = sizeof monitor;
        DEVMODEW mode{};
        mode.dmSize = sizeof mode;
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
            EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
            refreshHz = mode.dmDisplayFrequency;
            displayWidth = mode.dmPelsWidth;
            displayHeight = mode.dmPelsHeight;
        }
        std::printf("DXGI_CONFIG swap_effect=flip_discard buffers=2 format=rgba8 sync_interval=%u tearing=%u "
                    "tearing_supported=%u pacing=%s display=%ux%u refresh_hz=%u present_mode=unmeasured\n",
                    unsigned(vsync), unsigned(tearing), unsigned(tearingSupported), pace ? "half_interval" : "none",
                    displayWidth, displayHeight, refreshHz);
    }

    // Resizes only after observing GPU completion of all queued work, including the last Present.
    // The pending-work check holds the queue behind an unsignalled fence first and confirms that the
    // application does not resize while that work is outstanding.
    void resize(Gpu& gpu, FrameLabExtent extent)
    {
        expect(gpu.fence->GetCompletedValue() == gpu.signal, "drain presentation before resizing");
        check(swapchain->ResizeBuffers(0, extent.width, extent.height, DXGI_FORMAT_UNKNOWN,
              tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0), "resize swapchain buffers");
        RECT rect{0, 0, LONG(extent.width), LONG(extent.height)};
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        expect(AdjustWindowRect(&rect, style, FALSE) != 0, "adjust resized window rectangle");
        expect(SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0, "resize window");
        gpu.extent = extent;
        lastPresent = {};
        firstStatsValid = lastStatsValid = false;
        std::printf("DXGI_RESIZE width=%u height=%u\n", extent.width, extent.height);
    }

    bool pump()
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT || message.message == WM_CLOSE) return false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return IsWindow(window) != 0;
    }

    // Holds a real frame that follows a generated frame until the generated image has been shown for as
    // long as the preceding real frame, which splits each real-frame period evenly. Interpolation needs
    // frame N before the N-1/N midpoint can be shown, so this adds about half a period of latency to
    // every real frame.
    void hold(Kind kind)
    {
        if (!pace || kind != Kind::real || lastKind != Kind::generated || intervals.realToGenerated.empty()) return;
        const double shown = std::min(intervals.realToGenerated.back(), 250.0);
        const auto target = lastPresent + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(shown));
        while (Clock::now() < target) {
            if (target - Clock::now() > std::chrono::milliseconds(2))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            else
                std::this_thread::yield();
        }
    }

    void present(Gpu& gpu, Texture& input, unsigned frame, Kind kind, bool measured, bool pendingResize = false)
    {
        const bool generated = kind == Kind::generated;
        const auto index = swapchain->GetCurrentBackBufferIndex();
        {
            Texture backbuffer;
            backbuffer.state = D3D12_RESOURCE_STATE_PRESENT;
            check(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer.resource)), "get current backbuffer");
            gpu.begin();
            gpu.startTiming();
            gpu.transition(input, D3D12_RESOURCE_STATE_COPY_SOURCE);
            gpu.transition(backbuffer, D3D12_RESOURCE_STATE_COPY_DEST);
            gpu.list->CopyResource(backbuffer.resource.Get(), input.resource.Get());
            gpu.transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT);
            gpu.stopTiming();
            Gate gate;
            if (pendingResize) {
                check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)), "create resize gate");
                check(gpu.queue->Wait(gate.fence.Get(), 1), "hold actual backbuffer copy before resize");
            }
            gpu.enqueue(nullptr);
            if (gate.fence) {
                const auto completed = gpu.fence->GetCompletedValue();
                expect(completed < gpu.signal, "resize gate did not retain the backbuffer copy");
                std::printf("DXGI_RESIZE_PENDING outstanding=1 completed=%llu signal=%llu backbuffer=%u\n",
                            static_cast<unsigned long long>(completed), static_cast<unsigned long long>(gpu.signal), index);
                ++pendingResizeChecks;
                check(gate.fence->Signal(1), "release resize gate");
            }
            // The single allocator is reset by the next begin(). Wait so the copy has completed first.
            gpu.wait();
            if (gpu.timestamps && measured) intervals.copyGpu.push_back(gpu.elapsedGpuMs());
        } // The backbuffer reference is released before Present and before any ResizeBuffers.
        hold(kind);
        ++(generated ? generatedAttempts : realAttempts);
        const auto now = Clock::now();
        const bool previous = lastPresent.time_since_epoch().count() != 0;
        const double interval = previous ? std::chrono::duration<double, std::milli>(now - lastPresent).count() : 0;
        if (previous) {
            if (generated) intervals.realToGenerated.push_back(interval);
            else if (lastKind == Kind::generated) intervals.generatedToReal.push_back(interval);
            else intervals.realToReal.push_back(interval);
        }
        lastPresent = now;
        lastKind = kind;
        const HRESULT result = swapchain->Present(vsync ? 1 : 0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (result == S_OK) ++(generated ? generatedPresents : realPresents);
        if (result == occluded) ++occludedPresents;
        std::printf("DXGI_PRESENT source=%u previous=%u kind=%s backbuffer=%u fence=%llu interval_ms=%.3f hr=%#lx\n",
                    frame, generated ? frame - 1 : frame, generated ? "generated" : "real", index,
                    static_cast<unsigned long long>(gpu.signal), interval, static_cast<unsigned long>(result));
        check(result, "present backbuffer");
        DXGI_FRAME_STATISTICS stats{};
        statsResult = swapchain->GetFrameStatistics(&stats);
        if (SUCCEEDED(statsResult)) {
            if (!firstStatsValid) { firstStats = stats; firstStatsValid = true; }
            lastStats = stats;
            lastStatsValid = true;
        } else if (statsResult == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
            firstStatsValid = lastStatsValid = false;
        }
    }
};

FrameLabCamera camera(FrameLabExtent extent)
{
    FrameLabCamera result{};
    result.flags = FRAMELAB_CAMERA_VERIFIED | FRAMELAB_CAMERA_MOTION_INCLUDED;
    result.projection_space = result.reprojection_space = FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE;
    result.motion_convention = FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV;
    result.aspect = float(extent.width) / extent.height;
    result.fov_radians = 1;
    result.near_plane = 0.1f;
    result.far_plane = 1000;
    result.up[1] = result.right[0] = result.forward[2] = 1;
    result.motion_vector_scale[0] = result.motion_vector_scale[1] = 1;
    DirectX::XMFLOAT4X4 proj, inverse, identity;
    auto p = DirectX::XMMatrixPerspectiveFovLH(1, result.aspect, result.near_plane, result.far_plane);
    DirectX::XMStoreFloat4x4(&proj, p);
    DirectX::XMStoreFloat4x4(&inverse, DirectX::XMMatrixInverse(nullptr, p));
    DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
    std::memcpy(result.view_to_clip, &proj, sizeof proj);
    std::memcpy(result.clip_to_view, &inverse, sizeof inverse);
    for (auto* matrix : {result.clip_to_lens, result.clip_to_previous, result.previous_to_clip})
        std::memcpy(matrix, &identity, sizeof identity);
    return result;
}

struct LeasePayload {
    ComPtr<ID3D12Resource> resource;
    unsigned& releases;
};

void FRAMELAB_CALL release(void* ptr)
{
    std::unique_ptr<LeasePayload> payload(static_cast<LeasePayload*>(ptr));
    ++payload->releases;
}

using Lease = std::unique_ptr<FrameLabLease, decltype(&framelab_lease_destroy)>;
Lease pin(const Texture& texture, unsigned& releases)
{
    FrameLabLease* handle{};
    auto payload = std::make_unique<LeasePayload>(LeasePayload{texture.resource, releases});
    sdk(framelab_lease_create(payload.get(), release, &handle), "create allocation lease");
    payload.release();
    return {handle, &framelab_lease_destroy};
}

void writeReadback(const std::filesystem::path& path, const std::vector<uint32_t>& image)
{
    FILE* file = _wfopen(path.c_str(), L"wb");
    expect(file != nullptr, "open synthetic image output");
    const auto written = std::fwrite(image.data(), sizeof(uint32_t), image.size(), file);
    const auto closed = std::fclose(file);
    expect(written == image.size() && closed == 0, "write synthetic image output");
}

// Horizontal object position. The object wraps before it leaves the image; the wrap and an optional
// forced cut are discontinuities that the producer reports with a history reset.
struct Scene {
    unsigned span;
    int cutAt;
    explicit Scene(FrameLabExtent extent, int cut) : span((extent.width - 480) / 32 * 32), cutAt(cut) {}
    unsigned step(unsigned i) const { return i * 32 + (cutAt >= 0 && int(i) >= cutAt ? 13 * 32 : 0); }
    unsigned left(unsigned i) const { return 160 + step(i) % span; }
    bool discontinuity(unsigned i) const { return i && left(i) != left(i - 1) + 32; }
};

// Left and right edges of the rectangle on the middle row of a readback, or -1 if absent. The
// rectangle is orange; the checkerboard is grey.
void edges(const std::vector<uint32_t>& image, int& left, int& right)
{
    left = right = -1;
    const auto* row = image.data() + size_t(initialExtent.height / 2) * initialExtent.width;
    for (unsigned x = 0; x < initialExtent.width; ++x) {
        const int red = row[x] & 0xff, blue = (row[x] >> 16) & 0xff;
        if (red - blue < 0x60) continue;
        if (left < 0) left = int(x);
        right = int(x) + 1;
    }
}

FrameLabStatus tryCreate(FrameLabD3D12FgConfig config)
{
    FrameLabD3D12Fg* fg{};
    const auto status = framelab_d3d12_fg_create(&config, &fg);
    if (fg) sdk(framelab_d3d12_fg_destroy(fg), "destroy probe backend");
    return status;
}

// Descriptor sizes and render extents, checked against the real device before the run.
void descriptorChecks(const FrameLabD3D12FgConfig& base)
{
    constexpr uint32_t align = alignof(FrameLabD3D12FgConfig);
    constexpr uint32_t v1 = (uint32_t(offsetof(FrameLabD3D12FgConfig, render_extent)) + align - 1) / align * align;
    auto config = base;
    config.struct_size = v1;
    // A v0.4.0 client passes the v1 size. The bytes after it are not part of its struct, so the
    // invalid extent placed there must not be read.
    config.render_extent = {1, 0};
    sdk(tryCreate(config), "create with v0.4.0-size config");
    std::printf("ABI_CHECK v1_size=%u created=1\n", v1);
    for (uint32_t size = v1 - 1; size < sizeof config; ++size) {
        if (size == v1) continue;
        config.struct_size = size;
        expect(tryCreate(config) == FRAMELAB_ABI_MISMATCH, "partial config size was not rejected");
    }
    std::puts("ABI_CHECK partial_sizes_rejected=1");
    config = base;
    const auto w = base.extent.width, h = base.extent.height;
    for (const FrameLabExtent bad : {FrameLabExtent{w, 0}, FrameLabExtent{0, h},
                                     FrameLabExtent{w + 1, h}, FrameLabExtent{w, h + 1}}) {
        config.render_extent = bad;
        expect(tryCreate(config) == FRAMELAB_INVALID_DIMENSIONS, "invalid render extent was not rejected");
    }
    std::puts("ABI_CHECK invalid_render_extents_rejected=1");
}

bool parseExtent(const wchar_t* text, FrameLabExtent& extent)
{
    wchar_t* end{};
    const auto width = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'x') return false;
    const auto* height = end + 1;
    const auto value = std::wcstoul(height, &end, 10);
    if (end == height || *end) return false;
    extent = {uint32_t(width), uint32_t(value)};
    return extent.width && extent.height && extent.width <= initialExtent.width &&
        extent.height <= initialExtent.height;
}
struct Totals {
    unsigned rendered{}, evaluated{}, verified{}, unverifiedPresented{}, dropped{}, resets{}, segments{};
    unsigned expectedInterpolations{}, leasesCreated{}, leaseReleases{}, contractChecks{};
    bool historyContractVerified = false;
    std::vector<double> sourceIntervals, sourceGpu;
};

bool parse(int argc, wchar_t** argv, Options& options)
{
    if (argc < 4) return false;
    options.runtime = std::filesystem::absolute(argv[1]);
    options.logs = std::filesystem::absolute(argv[2]);
    options.proxy = argv[3];
    bool framesGiven = false;
    for (int i = 4; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--vsync" || arg == L"--no-vsync") {
            options.presenting = true;
            options.vsync = arg == L"--vsync";
        } else if (arg == L"--no-fg") options.noFg = true;
        else if (arg == L"--resize") options.resize = true;
        else if (arg == L"--reject-frame") options.reject = true;
        else if (arg == L"--debug" || arg == L"--gpu-validation") {
            options.debug = true;
            options.gpuValidation |= arg == L"--gpu-validation";
        }
        else if (arg == L"--timing") options.timing = true;
        else if (arg == L"--no-pacing") options.pace = false;
        else if (arg == L"--render" && i + 1 < argc) {
            if (!parseExtent(argv[++i], options.render)) return false;
        }
        else if (arg == L"--frames" && i + 1 < argc) {
            options.frames = unsigned(std::wcstoul(argv[++i], nullptr, 10));
            framesGiven = true;
        } else if (arg == L"--cut-at" && i + 1 < argc) options.cutAt = int(std::wcstol(argv[++i], nullptr, 10));
        else return false;
    }
    // Earlier published command lines enable these through the environment.
    options.debug |= environment(L"FRAMELAB_D3D12_DEBUG");
    options.reject |= environment(L"FRAMELAB_D3D12_REJECT_FRAME");
    options.resize |= environment(L"FRAMELAB_D3D12_RESIZE");
    if (!options.presenting)
        return !options.noFg && !options.resize && !options.reject && !options.timing && !framesGiven &&
               options.cutAt < 0 && options.pace;
    return options.frames >= 3 && options.frames <= 100000 && !(options.timing && options.debug);
}

void writeSummary(const Options& options, const Gpu& gpu, const Presenter& presenter, const Totals& totals,
                  bool pass)
{
    const auto path = options.logs / L"d3d12-fg-summary.json";
    FILE* file = _wfopen(path.c_str(), L"w");
    expect(file != nullptr, "open summary");
    const auto& in = presenter.intervals;
    const auto stat = [&](const char* name, const std::vector<double>& v, bool last) {
        std::fprintf(file, "    \"%s\": {\"count\": %zu, \"p50\": %.3f, \"p95\": %.3f, \"max\": %.3f}%s\n", name,
                     v.size(), percentile(v, 0.5), percentile(v, 0.95), percentile(v, 1.0), last ? "" : ",");
    };
    std::fprintf(file, "{\n  \"result\": \"%s\",\n", pass ? "pass" : "fail");
    std::fprintf(file, "  \"mode\": {\"fg\": %s, \"sync_interval\": %u, \"tearing\": %s, \"pacing\": \"%s\", "
                 "\"timing\": %s, \"debug_layer\": %s, \"frames_per_segment\": %u, \"segments\": %u, \"resize\": %s, "
                 "\"reject_frame\": %s, \"cut_at\": %d},\n",
                 options.noFg ? "false" : "true", unsigned(presenter.vsync), presenter.tearing ? "true" : "false",
                 presenter.pace ? "half_interval" : "none", options.timing ? "true" : "false",
                 options.debug ? "true" : "false", options.frames, totals.segments, options.resize ? "true" : "false",
                 options.reject ? "true" : "false", options.cutAt);
    std::fprintf(file, "  \"display\": {\"width\": %u, \"height\": %u, \"refresh_hz\": %u, \"present_mode\": "
                 "\"unmeasured\", \"scanout\": \"unmeasured\"},\n",
                 presenter.displayWidth, presenter.displayHeight, presenter.refreshHz);
    std::fprintf(file, "  \"counters\": {\"rendered\": %u, \"native_evaluations\": %u, \"verified_generated\": %u, "
                 "\"unverified_generated_presented\": %u, \"expected_interpolations\": %u, \"dropped\": %u, "
                 "\"history_resets\": %u, \"real_attempts\": %u, \"generated_attempts\": %u, \"real_presents\": %u, "
                 "\"generated_presents\": %u, \"occluded_presents\": %u, \"pending_resize_checks\": %u, "
                 "\"contract_checks\": %u, \"history_contract_verified\": %s},\n",
                 totals.rendered, totals.evaluated, totals.verified, totals.unverifiedPresented,
                 totals.expectedInterpolations, totals.dropped, totals.resets, presenter.realAttempts,
                 presenter.generatedAttempts, presenter.realPresents, presenter.generatedPresents,
                 presenter.occludedPresents, presenter.pendingResizeChecks, totals.contractChecks,
                 totals.historyContractVerified ? "true" : "false");
    std::fprintf(file, "  \"leases\": {\"created\": %u, \"released\": %u},\n", totals.leasesCreated,
                 totals.leaseReleases);
    std::fprintf(file, "  \"fence\": {\"signalled\": %llu, \"completed\": %llu},\n",
                 static_cast<unsigned long long>(gpu.signal),
                 static_cast<unsigned long long>(gpu.fence->GetCompletedValue()));
    std::fprintf(file, "  \"present_interval_ms\": {\n");
    stat("real_to_generated", in.realToGenerated, false);
    stat("generated_to_real", in.generatedToReal, false);
    stat("real_to_real", in.realToReal, false);
    stat("source", totals.sourceIntervals, true);
    std::fprintf(file, "  },\n  \"gpu_interval_ms\": {\n");
    stat("source_submission", totals.sourceGpu, false);
    stat("backbuffer_copy", in.copyGpu, true);
    std::fprintf(file, "  },\n");
    const double sourceMs = std::accumulate(totals.sourceIntervals.begin(), totals.sourceIntervals.end(), 0.0);
    const double sourceFps = sourceMs > 0 ? 1000.0 * totals.sourceIntervals.size() / sourceMs : 0;
    std::fprintf(file, "  \"source_fps_after_warmup\": %.3f,\n", sourceFps);
    const bool stats = presenter.firstStatsValid && presenter.lastStatsValid;
    std::fprintf(file, "  \"frame_statistics\": {\"available\": %s, \"last_hr\": \"%#lx\", \"present_count\": %u, "
                 "\"present_refresh_count\": %u, \"sync_refresh_count\": %u},\n",
                 stats ? "true" : "false", static_cast<unsigned long>(presenter.statsResult),
                 stats ? presenter.lastStats.PresentCount - presenter.firstStats.PresentCount : 0,
                 stats ? presenter.lastStats.PresentRefreshCount - presenter.firstStats.PresentRefreshCount : 0,
                 stats ? presenter.lastStats.SyncRefreshCount - presenter.firstStats.SyncRefreshCount : 0);
    auto steady = gpu.memory.steady;
    std::sort(steady.begin(), steady.end());
    const uint64_t medianUsage = steady.empty() ? 0 : steady[steady.size() / 2];
    std::fprintf(file, "  \"memory_bytes\": {\"start\": %llu, \"warm\": %llu, \"end\": %llu, \"cleanup\": %llu, "
                 "\"growth_after_warm\": %llu, \"peak_local\": %llu, \"steady_local\": %llu, \"budget\": %llu},\n",
                 static_cast<unsigned long long>(gpu.memory.start), static_cast<unsigned long long>(gpu.memory.warm),
                 static_cast<unsigned long long>(gpu.memory.end), static_cast<unsigned long long>(gpu.memory.cleanup),
                 static_cast<unsigned long long>(gpu.memory.growth),
                 static_cast<unsigned long long>(gpu.memory.peak), static_cast<unsigned long long>(medianUsage),
                 static_cast<unsigned long long>(gpu.memory.budget == UINT64_MAX ? 0 : gpu.memory.budget));
    std::fprintf(file, "  \"validation\": {\"debug_layer\": %s, \"gpu_based\": %s, \"errors\": %u, \"warnings\": %u}\n}\n",
                 options.debug ? "true" : "false", options.gpuValidation ? "true" : "false",
                 gpu.validation.errors, gpu.validation.warnings);
    expect(std::fclose(file) == 0, "write summary");
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    Options options;
    if (!parse(argc, argv, options)) {
        std::fprintf(stderr,
            "usage: d3d12_fg <NGX runtime directory> <output directory> <app-local proxy or ->\n"
            "       [--vsync | --no-vsync] [--no-fg] [--frames N] [--resize] [--reject-frame] [--cut-at N]\n"
            "       [--debug | --gpu-validation] [--timing] [--no-pacing] [--render <width>x<height>]\n"
            "Without --vsync or --no-vsync the client runs the original six-frame readback check.\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool presenting = options.presenting;
    const bool fallbackOnly = options.noFg;
    try {
        const auto& runtime = options.runtime;
        const auto& logs = options.logs;
        std::filesystem::create_directories(logs);
        expect(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS) != 0,
               "set app-local DLL search policy");
        if (!fallbackOnly)
            expect(AddDllDirectory(runtime.c_str()) != nullptr, "add external NGX runtime directory");
        if (!fallbackOnly && std::wcscmp(options.proxy, L"-") != 0) {
            const auto proxy = std::filesystem::absolute(options.proxy);
            expect(LoadLibraryW(proxy.c_str()) != nullptr, "load configured app-local proxy");
        }
        Gpu gpu;
        gpu.initialize(options.debug, options.gpuValidation);
        if (options.timing) gpu.initializeTiming();
        Presenter presenter;
        if (presenting) presenter.initialize(gpu, options.vsync, options.pace);
        Totals totals;
        const unsigned warmFrame = std::min(options.frames - 1, std::max(2u, options.frames / 4));
        // Compare live allocations at warm-up and segment end before destroying the backend.
        const auto sampleFrame = [&](unsigned i) {
            const auto usage = gpu.sampleMemory();
            if (i == warmFrame) gpu.memory.warm = usage;
            if (i > warmFrame && usage) gpu.memory.steady.push_back(usage);
        };
        const auto runFrames = [&](unsigned firstFrame, unsigned frames, FrameLabExtent configuredRender, bool recreated) {
            const bool checkingReadback = !presenting && !recreated;
            const bool resizing = presenting && options.resize && !firstFrame;
            ++totals.segments;
            const auto extent = gpu.extent;
            const auto render = configuredRender.width ?
                FrameLabExtent{std::min(configuredRender.width, extent.width), std::min(configuredRender.height, extent.height)} :
                extent;
            FrameLabD3D12FgConfig config{sizeof config, FRAMELAB_ABI_VERSION, 1, 1,
                gpu.device.Get(), gpu.queue.Get(), gpu.fence.Get(), extent,
                "31246941-7a24-484c-a4a7-e436c26c6f95", "phase2.1", runtime.c_str(), logs.c_str(), reportInit, nullptr,
                configuredRender.width ? render : FrameLabExtent{}};
            if (checkingReadback) descriptorChecks(config);
            FrameLabD3D12Fg* raw{};
            if (!fallbackOnly) {
                const auto status = framelab_d3d12_fg_create(&config, &raw);
                if (status != FRAMELAB_OK && presenting)
                    std::printf("SDK_FG_FALLBACK initialization=%s\n", framelab_status_string(status));
                else
                    sdk(status, "initialize NGX and query native FG support");
            }
            std::unique_ptr<FrameLabD3D12Fg, decltype(&framelab_d3d12_fg_destroy)>
                fg(raw, &framelab_d3d12_fg_destroy);
            const auto recorded = [&](FrameLabStatus result, const char* step) {
                if (result == FRAMELAB_OK) return;
                // Discard the list. A native recording error can leave partial commands.
                gpu.list->Close();
                framelab_d3d12_fg_cancel(raw);
                sdk(result, step);
            };
            if (raw) {
                gpu.begin();
                const auto status = framelab_d3d12_fg_record_initialize(raw, gpu.list.Get());
                if (presenting && status != FRAMELAB_OK) {
                    check(gpu.list->Close(), "discard failed FG initialization list");
                    if (status == FRAMELAB_NATIVE_FAILURE)
                        sdk(framelab_d3d12_fg_cancel(raw), "cancel failed FG initialization");
                    sdk(framelab_d3d12_fg_destroy(fg.release()), "destroy unavailable FG backend");
                    raw = nullptr;
                    std::printf("SDK_FG_FALLBACK native_create=%s\n", framelab_status_string(status));
                } else {
                    recorded(status, "record native FG create");
                    gpu.enqueue(raw);
                    gpu.wait();
                    sdk(framelab_d3d12_fg_retire(raw), "retire native FG create");
                }
            }

            auto color = gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM, extent);
            Texture depth, motion, generated;
            const size_t count = size_t(extent.width) * extent.height;
            const size_t renderCount = size_t(render.width) * render.height;
            std::vector<uint32_t> pixels(count), previous, output, clear;
            std::vector<float> depths;
            std::vector<uint16_t> vectors;
            const auto abandonFg = [&] {
                sdk(framelab_d3d12_fg_destroy(fg.release()), "destroy idle FG backend after allocation failure");
                raw = nullptr;
                depth = {};
                motion = {};
                generated = {};
                std::puts("SDK_FG_FALLBACK allocation=out_of_memory");
            };
            if (raw) {
                try {
                    depth = gpu.texture(DXGI_FORMAT_R32_FLOAT, render);
                    if (presenting && environment(L"FRAMELAB_D3D12_FAIL_FG_ALLOCATION"))
                        throw GpuError(E_OUTOFMEMORY, "inject optional FG allocation failure");
                    motion = gpu.texture(DXGI_FORMAT_R16G16_FLOAT, render);
                    generated = gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM, extent);
                    depths.resize(renderCount);
                    vectors.resize(renderCount * 2);
                    if (!options.timing) {
                        previous.resize(count);
                        output.resize(count);
                        clear.assign(count, sentinel);
                    }
                } catch (const GpuError& error) {
                    if (!presenting || error.result != E_OUTOFMEMORY) throw;
                    abandonFg();
                } catch (const std::bad_alloc&) {
                    if (!presenting) throw;
                    abandonFg();
                }
            }
            unsigned releases = 0;
            Lease colorLease(nullptr, &framelab_lease_destroy), depthLease(nullptr, &framelab_lease_destroy),
                motionLease(nullptr, &framelab_lease_destroy);
            const bool haveLeases = raw != nullptr;
            if (haveLeases) {
                colorLease = pin(color, releases);
                depthLease = pin(depth, releases);
                motionLease = pin(motion, releases);
            }
            totals.leasesCreated += raw ? 3 : 0;
            const auto view = camera(extent);
            const Scene scene(extent, presenting ? options.cutAt : -1);
            auto time = Clock::now();
            unsigned verified = 0, evaluated = 0, rendered = 0;
            int maxEdgeError = 0;
            const auto realBefore = presenter.realPresents;
            const auto generatedBefore = presenter.generatedPresents;
            const bool injectRejection = presenting && options.reject;
            bool resetHistory = true;
            uint32_t pendingReset = 0;
            unsigned dropped = 0, expectedInterpolations = 0, unverifiedPresented = 0;
            for (unsigned i = 0; i < frames; ++i) {
                if (presenting && !presenter.pump()) break;
                const auto now = Clock::now();
                const unsigned left = scene.left(i);
                const bool cut = presenting && scene.discontinuity(i);
                const bool pendingResize = resizing && i + 1 == frames;
                const double sourceSeconds = i ? std::chrono::duration<double>(now - time).count() : 1.0 / 60;
                time = now;
                if (i > warmFrame) totals.sourceIntervals.push_back(sourceSeconds * 1000);
                for (unsigned y = 0; y < extent.height; ++y) {
                    for (unsigned x = 0; x < extent.width; ++x) {
                        const size_t p = y * extent.width + x;
                        const bool object = x >= left && x < left + 160 && y >= 240 && y < 480;
                        pixels[p] = object ? 0xff2080f0 : (((x / 32 + y / 32) % 2) ? 0xff303030 : 0xff181818);

                    }
                }
                if (raw) {
                    const auto uvMotion = DirectX::PackedVector::XMConvertFloatToHalf(-32.0f / extent.width);
                    for (unsigned y = 0; y < render.height; ++y) {
                        const auto oy = unsigned((y + 0.5) * extent.height / render.height);
                        for (unsigned x = 0; x < render.width; ++x) {
                            const auto ox = unsigned((x + 0.5) * extent.width / render.width);
                            const bool object = ox >= left && ox < left + 160 && oy >= 240 && oy < 480;
                            const size_t p = size_t(y) * render.width + x;
                            depths[p] = object ? 0.3f : 0.9f;
                            vectors[2 * p] = object ? uvMotion : 0;
                            vectors[2 * p + 1] = 0;
                        }
                    }
                }
                if (cut && raw) {
                    // The object jumped. Drop history before the frame is recorded.
                    sdk(framelab_d3d12_fg_invalidate(raw), "invalidate FG history at discontinuity");
                    resetHistory = true;
                    pendingReset = FRAMELAB_RESET_CAMERA_CUT;
                    std::printf("SDK_FG_CUT source=%u left=%u\n", firstFrame + i, left);
                }
                const auto colorState = color.state;
                const auto depthState = depth.state;
                const auto motionState = motion.state;
                const auto generatedState = generated.state;
                gpu.begin();
                gpu.startTiming();
                constexpr auto shader = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                const auto presentOrdinary = [&] {
                    gpu.stopTiming();
                    gpu.sampleMemory();
                    gpu.enqueue(nullptr);
                    gpu.wait();
                    if (options.timing && i > warmFrame) totals.sourceGpu.push_back(gpu.elapsedGpuMs());
                    presenter.present(gpu, color, firstFrame + i, Kind::real, i > warmFrame, pendingResize);
                    sampleFrame(i);
                };
                const auto discardUploads = [&] {
                    check(gpu.list->Close(), "discard unsubmitted FG list");
                    color.state = colorState;
                    depth.state = depthState;
                    motion.state = motionState;
                    generated.state = generatedState;
                };
                gpu.upload(color, pixels.data(), shader);
                ++rendered;
                if (!raw) {
                    presentOrdinary();
                    continue;
                }
                try {
                    gpu.upload(depth, depths.data(), shader);
                    gpu.upload(motion, vectors.data(), shader);
                    // The sentinel clear detects outputs that NGX did not write. The timing mode skips it.
                    if (!options.timing) gpu.upload(generated, clear.data(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    else gpu.transition(generated, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                } catch (const GpuError& error) {
                    if (!presenting || error.result != E_OUTOFMEMORY) throw;
                    discardUploads();
                    abandonFg();
                    depthLease.reset();
                    motionLease.reset();
                    gpu.begin();
                    gpu.startTiming();
                    gpu.upload(color, pixels.data(), shader);
                    presentOrdinary();
                    continue;
                }

                FrameLabFgFrame frame{};
                frame.struct_size = sizeof frame;
                frame.abi_version = FRAMELAB_ABI_VERSION;
                frame.stream_id = frame.device_id = frame.generation = 1;
                frame.frame_index = i;
                frame.render_extent = render;
                frame.output_extent = extent;
                frame.delta_seconds = sourceSeconds;
                frame.reset_history = resetHistory ?
                    (i ? (pendingReset ? pendingReset : FRAMELAB_RESET_DISCONTINUITY) : FRAMELAB_RESET_INITIAL) : 0;
                frame.camera = view;
                frame.producer = {1, i + 1u, 0, 0, FRAMELAB_COMPLETION_SAME_COMMAND_STREAM, nullptr};
                frame.color = {11, i, 1, extent, FRAMELAB_FORMAT_RGBA8_UNORM,
                               FRAMELAB_STATE_SHADER_READ, FRAMELAB_PROVENANCE_SYNTHETIC, colorLease.get()};
                frame.depth = {12, i, 1, render, FRAMELAB_FORMAT_R32_FLOAT,
                               FRAMELAB_STATE_SHADER_READ, FRAMELAB_PROVENANCE_SYNTHETIC, depthLease.get()};
                frame.motion_vectors = {13, i, 1, render, FRAMELAB_FORMAT_RG16_FLOAT,
                               FRAMELAB_STATE_SHADER_READ, FRAMELAB_PROVENANCE_SYNTHETIC, motionLease.get()};
                FrameLabD3D12FgBindings bindings{sizeof bindings, FRAMELAB_ABI_VERSION, i + 1u, gpu.list.Get(),
                    {11, color.resource.Get(), static_cast<uint32_t>(color.state)},
                    {12, depth.resource.Get(), static_cast<uint32_t>(depth.state)},
                    {13, motion.resource.Get(), static_cast<uint32_t>(motion.state)},
                    {14, generated.resource.Get(), static_cast<uint32_t>(generated.state)}};
                if (i == 0) {
                    auto bad = bindings;
                    bad.color.state = D3D12_RESOURCE_STATE_COPY_DEST;
                    expect(framelab_d3d12_fg_record(raw, &frame, &bad) == FRAMELAB_INVALID_RESOURCE_STATE,
                           "wrong native resource state was not rejected");
                    std::puts("ABI_CHECK invalid_native_state_rejected=1");
                    ++totals.contractChecks;
                }
                if (checkingReadback && !i) {
                    // Self-consistent metadata at a render extent the backend was not created with.
                    auto other = frame;
                    other.render_extent = render == extent ?
                        FrameLabExtent{extent.width / 2, extent.height / 2} : extent;
                    other.depth.extent = other.motion_vectors.extent = other.render_extent;
                    expect(framelab_d3d12_fg_record(raw, &other, &bindings) == FRAMELAB_INVALID_DIMENSIONS,
                           "frame at another render extent was not rejected");
                    std::puts("ABI_CHECK render_extent_mismatch_rejected=1");

                    // Correct metadata, identity, lease and state, but the native depth resource has
                    // another size.
                    unsigned wrongReleases = 0;
                    auto wrongDepth = gpu.texture(DXGI_FORMAT_R32_FLOAT, {render.width, render.height + 1});
                    std::vector<float> zeros(size_t(render.width) * (render.height + 1));
                    gpu.upload(wrongDepth, zeros.data(), shader);
                    auto wrongLease = pin(wrongDepth, wrongReleases);
                    auto resized = frame;
                    resized.depth.lease = wrongLease.get();
                    auto resizedBindings = bindings;
                    resizedBindings.depth = {12, wrongDepth.resource.Get(), static_cast<uint32_t>(wrongDepth.state)};
                    expect(framelab_d3d12_fg_record(raw, &resized, &resizedBindings) == FRAMELAB_INVALID_RESOURCE_STATE,
                           "native depth of another size was not rejected");
                    std::puts("ABI_CHECK native_extent_mismatch_rejected=1");
                    gpu.uploads.push_back(wrongDepth.resource);
                }
                if ((recreated || (presenting && firstFrame)) && !i) {
                    auto unreset = frame;
                    unreset.reset_history = 0;
                    expect(framelab_d3d12_fg_record(raw, &unreset, &bindings) == FRAMELAB_RESET_REQUIRED,
                           "recreated backend accepted a frame without a reset");
                    std::puts(recreated ? "ABI_CHECK recreated_requires_reset=1" : "ABI_CHECK resize_requires_reset=1");
                }
                if (i == 2 && !recreated) {
                    auto stale = frame;
                    --stale.depth.frame_index;
                    expect(framelab_d3d12_fg_record(raw, &stale, &bindings) == FRAMELAB_INVALID_SEQUENCE,
                           "stale depth was not rejected");
                    std::puts("ABI_CHECK stale_depth_rejected=1");
                    ++totals.contractChecks;
                }
                if (cut) {
                    // After invalidation, a frame that does not declare a reset must be rejected.
                    auto continued = frame;
                    continued.reset_history = 0;
                    expect(framelab_d3d12_fg_record(raw, &continued, &bindings) == FRAMELAB_RESET_REQUIRED,
                           "frame without reset after invalidation was not rejected");
                    if (!totals.historyContractVerified) std::puts("ABI_CHECK reset_required_after_invalidate=1");
                    totals.historyContractVerified = true;
                    ++totals.contractChecks;
                }
                if (injectRejection && i == 2) --frame.depth.frame_index;
                const auto result = framelab_d3d12_fg_record(raw, &frame, &bindings);
                if (presenting && result != FRAMELAB_OK) {
                    // A failed native evaluation can leave commands in this list. Submit a fresh
                    // upload for ordinary presentation, starting from the last committed states.
                    discardUploads();
                    if (result == FRAMELAB_NATIVE_FAILURE)
                        sdk(framelab_d3d12_fg_cancel(raw), "cancel failed FG packet");
                    gpu.begin();
                    gpu.startTiming();
                    gpu.upload(color, pixels.data(), shader);
                    presentOrdinary();
                    std::printf("SDK_FG_FALLBACK source=%u record=%s\n", firstFrame + i, framelab_status_string(result));
                    ++dropped;
                    resetHistory = true;
                    pendingReset = 0;
                    continue;
                }
                recorded(result, "record native FG evaluate");
                if (resetHistory && i) ++totals.resets;
                const bool expectInterpolation = !resetHistory;
                if (expectInterpolation) ++expectedInterpolations;
                resetHistory = false;
                pendingReset = 0;
                ++evaluated;
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
                ComPtr<ID3D12Resource> readback;
                if (!options.timing) readback = gpu.readback(generated, layout);
                Gate gate;
                if ((checkingReadback && i + 1 == frames) || pendingResize) {
                    check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)), "create test execution gate");
                    check(gpu.queue->Wait(gate.fence.Get(), 1), "block queue until retention is verified");
                }
                gpu.stopTiming();
                gpu.sampleMemory();
                gpu.enqueue(raw);
                if (gate.fence && !presenting) {
                    colorLease.reset();
                    depthLease.reset();
                    motionLease.reset();
                    color.resource.Reset();
                    depth.resource.Reset();
                    motion.resource.Reset();
                    generated.resource.Reset();
                    expect(framelab_d3d12_fg_retire(raw) == FRAMELAB_NOT_READY && releases == 0,
                           "early retirement released pending resources");
                    expect(framelab_d3d12_fg_destroy(raw) == FRAMELAB_BUSY, "pending backend was destructible");
                    check(gate.fence->Signal(1), "release test execution gate");
                    std::puts("ABI_CHECK early_retire_rejected=1 pending_destroy_rejected=1");
                } else if (gate.fence) {
                    expect(framelab_d3d12_fg_retire(raw) == FRAMELAB_NOT_READY && releases == 0,
                           "resize released pending FG resources");
                    expect(framelab_d3d12_fg_destroy(raw) == FRAMELAB_BUSY, "resize destroyed pending FG backend");
                    std::puts("ABI_CHECK resize_fg_pending=1");
                    check(gate.fence->Signal(1), "release FG resize gate");
                }
                gpu.wait();
                if (options.timing && i > warmFrame) totals.sourceGpu.push_back(gpu.elapsedGpuMs());
                sdk(framelab_d3d12_fg_retire(raw), "retire completed frame");
                bool interpolated = false;
                if (readback) {
                    void* mapped{};
                    check(readback->Map(0, nullptr, &mapped), "map generated texture readback");
                    const size_t rowBytes = extent.width * sizeof(uint32_t);
                    for (unsigned y = 0; y < extent.height; ++y)
                        std::memcpy(output.data() + y * extent.width,
                            static_cast<char*>(mapped) + layout.Offset + y * layout.Footprint.RowPitch, rowBytes);
                    readback->Unmap(0, nullptr);
                    bool written = true;
                    for (const auto pixel : output) written &= pixel != sentinel;
                    interpolated = expectInterpolation && written && output != pixels && output != previous;
                    if (interpolated) {
                        ++verified;
                        if (!presenting) {
                            int leftEdge, rightEdge;
                            edges(output, leftEdge, rightEdge);
                            const int expected = int(left) - 16;
                            const int error = leftEdge < 0 ? int(extent.width) :
                                std::max(std::abs(leftEdge - expected), std::abs(rightEdge - (expected + 160)));
                            maxEdgeError = std::max(maxEdgeError, error);
                            std::printf("SDK_FG_EDGES frame=%u left=%d right=%d expected_left=%d\n",
                                        i, leftEdge, rightEdge, expected);
                        }
                    }
                }
                if (presenting) {
                    // In correctness modes the texture verified above is the source of the generated backbuffer
                    // copy. The timing mode presents evaluated output without reading it back.
                    if (interpolated) presenter.present(gpu, generated, firstFrame + i, Kind::generated, i > warmFrame);
                    else if (options.timing && expectInterpolation) {
                        presenter.present(gpu, generated, firstFrame + i, Kind::generated, i > warmFrame);
                        ++unverifiedPresented;
                    }
                    presenter.present(gpu, color, firstFrame + i, Kind::real, i > warmFrame, pendingResize);
                }
                if (!options.timing) previous = pixels;
                sampleFrame(i);
                if (!presenting || frames <= 32 || i % 60 == 0 || i + 1 == frames)
                    std::printf("SDK_FG_FRAME frame=%u fence=%llu ngx=%#x\n", firstFrame + i,
                                static_cast<unsigned long long>(gpu.signal), framelab_d3d12_fg_last_ngx_result(raw));
            }
            totals.rendered += rendered;
            totals.evaluated += evaluated;
            totals.verified += verified;
            totals.unverifiedPresented += unverifiedPresented;
            totals.dropped += dropped;
            totals.expectedInterpolations += expectedInterpolations;
            // Destroy only after the last packet retired. Check the result, then drop caller leases.
            if (presenting) {
                gpu.drain();
                gpu.memory.end = gpu.sampleMemory();
                if (gpu.memory.end > gpu.memory.warm)
                    gpu.memory.growth = std::max(gpu.memory.growth, gpu.memory.end - gpu.memory.warm);
            }
            if (fg) sdk(framelab_d3d12_fg_destroy(fg.release()), "destroy idle FG backend");
            colorLease.reset();
            depthLease.reset();
            motionLease.reset();
            totals.leaseReleases += releases;
            if (presenting) {
                gpu.validation.drain();
                std::printf("DXGI_SUMMARY first_source=%u rendered=%u evaluated=%u verified=%u unverified_presented=%u "
                            "dropped=%u real_attempts=%u generated_attempts=%u real_presents=%u generated_presents=%u "
                            "occluded=%u lease_releases=%u scanout=unmeasured\n",
                            firstFrame, rendered, evaluated, verified, unverifiedPresented, dropped,
                            presenter.realAttempts, presenter.generatedAttempts, presenter.realPresents,
                            presenter.generatedPresents, presenter.occludedPresents, releases);
                const unsigned shown = options.timing ? unverifiedPresented : verified;
                const bool complete = rendered == frames && presenter.realPresents - realBefore == rendered;
                const bool fgComplete = !raw || (evaluated + dropped == rendered && shown == expectedInterpolations &&
                                                 presenter.generatedPresents - generatedBefore == shown);
                const bool released = releases == (haveLeases ? 3u : 0u);
                return complete && fgComplete && released ? 0 : 1;
            }
            if (recreated) {
                std::printf("SDK_FG_RECREATED render=%ux%u real_frames=%u verified=%u final_lease_releases=%u max_edge_error=%d\n",
                            render.width, render.height, frames, verified, releases, maxEdgeError);
            } else {
                writeReadback(logs / L"generated-5.rgba", output);
                std::printf("SDK_FG_SUMMARY real_frames=%u verified=%u final_lease_releases=%u max_edge_error=%d\n",
                            frames, verified, releases, maxEdgeError);
            }
            return verified == frames - 1 && releases == 3 ? 0 : 1;
        };
        int result = runFrames(0, options.frames, options.render, false);
        if (presenting && !result && options.resize) {
            presenter.resize(gpu, resizedExtent);
            result = runFrames(options.frames, options.frames, options.render, false);
        }
        if (!presenting) {
            const auto nextRender = !options.render.width || options.render == initialExtent ? resizedExtent : initialExtent;
            if (!result) result = runFrames(readbackFrames, recreatedFrames, nextRender, true);
            gpu.validation.drain();
            return result || gpu.validation.errors || gpu.validation.warnings ? 1 : 0;
        }
        // Drain the final Present, release the swapchain and window before the device, and collect any
        // debug messages raised by that teardown.
        presenter.teardown();
        gpu.memory.cleanup = gpu.sampleMemory();
        std::puts("TEARDOWN final_present_drained=1 swapchain_released=1");
        gpu.validation.drain();
        // Extended runs must not keep growing after warm-up. 32 MiB allows for driver-side pooling noise.
        const bool stable = options.frames < 120 || (gpu.memory.warm != 0 && gpu.memory.end != 0 &&
                                                    gpu.memory.growth <= (32ull << 20));
        const bool clean = !gpu.validation.errors && !gpu.validation.warnings;
        const bool leases = totals.leaseReleases == totals.leasesCreated;
        std::printf("MEMORY start_mb=%.1f warm_mb=%.1f end_mb=%.1f peak_mb=%.1f budget_mb=%.1f growth_mb=%.1f\n",
                    gpu.memory.start / 1048576.0, gpu.memory.warm / 1048576.0, gpu.memory.end / 1048576.0,
                    gpu.memory.peak / 1048576.0, (gpu.memory.budget == UINT64_MAX ? 0 : gpu.memory.budget) / 1048576.0,
                    gpu.memory.growth / 1048576.0);
        if (options.debug)
            std::printf("D3D12_VALIDATION errors=%u warnings=%u\n", gpu.validation.errors, gpu.validation.warnings);
        const bool pass = !result && stable && clean && leases &&
            (!options.resize || presenter.pendingResizeChecks == 1) &&
            (options.cutAt < 0 || !totals.evaluated || totals.historyContractVerified);
        writeSummary(options, gpu, presenter, totals, pass);
        std::printf("FL15_RESULT %s\n", pass ? "pass" : "fail");
        return pass ? 0 : 1;
    } catch (const GpuError& error) {
        std::fprintf(stderr, "D3D12 public SDK test: %s: HRESULT 0x%08lx\n", error.what(), error.result);
        return 1;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "D3D12 public SDK test: %s\n", ex.what());
        return 1;
    }
}
