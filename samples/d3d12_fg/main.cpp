// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <framelab/d3d12_fg.h>

#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
constexpr FrameLabExtent extent{1280, 720};
constexpr unsigned frames = 6;
constexpr uint32_t sentinel = 0xffff00ff;

void check(HRESULT result, const char* step)
{
    if (SUCCEEDED(result)) return;
    std::fprintf(stderr, "%s: HRESULT 0x%08lx\n", step, result);
    throw std::runtime_error(step);
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

void FRAMELAB_CALL reportInit(void*, const char* operation, uint32_t result)
{
    std::printf("SDK_FG_INIT operation=%s ngx=%#x\n", operation, result);
}

struct Texture {
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
};

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t signal = 0;

    ~Gpu() { if (event) CloseHandle(event); }

    void initialize()
    {
        expect(event != nullptr, "create fence event");
        ComPtr<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create DXGI factory");
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            const auto enumerated = factory->EnumAdapters1(i, &adapter);
            if (enumerated == DXGI_ERROR_NOT_FOUND) break;
            check(enumerated, "enumerate DXGI adapter");
            DXGI_ADAPTER_DESC1 desc{};
            check(adapter->GetDesc1(&desc), "read adapter description");
            if (desc.VendorId != 0x10de) continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
                std::printf("GPU %ls\n", desc.Description);
                break;
            }
        }
        expect(device.Get() != nullptr, "NVIDIA D3D12 device unavailable");
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "create queue");
        check(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator)), "create allocator");
        check(device->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "create list");
        check(list->Close(), "close initial list");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create completion fence");
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
        sdk(framelab_d3d12_fg_mark_submitted(fg, signal), "acknowledge submission");
    }

    void wait()
    {
        check(fence->SetEventOnCompletion(signal, event), "watch fence");
        expect(WaitForSingleObject(event, 30'000) == WAIT_OBJECT_0, "GPU fence timed out");
        check(device->GetDeviceRemovedReason(), "check device after fence");
    }

    Texture texture(DXGI_FORMAT format)
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
        constexpr size_t rowBytes = extent.width * sizeof(uint32_t);
        for (unsigned y = 0; y < extent.height; ++y)
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

FrameLabCamera camera()
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

struct Gate {
    ComPtr<ID3D12Fence> fence;
    ~Gate() { if (fence) fence->Signal(1); }
};

void writeReadback(const std::filesystem::path& path, const std::vector<uint32_t>& image)
{
    FILE* file = _wfopen(path.c_str(), L"wb");
    expect(file != nullptr, "open synthetic image output");
    const auto written = std::fwrite(image.data(), sizeof(uint32_t), image.size(), file);
    const auto closed = std::fclose(file);
    expect(written == image.size() && closed == 0, "write synthetic image output");
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "usage: d3d12_fg <NGX runtime directory> <output directory> <app-local proxy or ->\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const auto runtime = std::filesystem::absolute(argv[1]);
        const auto logs = std::filesystem::absolute(argv[2]);
        std::filesystem::create_directories(logs);
        expect(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS) != 0,
               "set app-local DLL search policy");
        expect(AddDllDirectory(runtime.c_str()) != nullptr, "add external NGX runtime directory");
        if (std::wcscmp(argv[3], L"-") != 0) {
            const auto proxy = std::filesystem::absolute(argv[3]);
            expect(LoadLibraryW(proxy.c_str()) != nullptr, "load configured app-local proxy");
        }
        Gpu gpu;
        gpu.initialize();
        FrameLabD3D12FgConfig config{sizeof config, FRAMELAB_ABI_VERSION, 1, 1,
            gpu.device.Get(), gpu.queue.Get(), gpu.fence.Get(), extent,
            "31246941-7a24-484c-a4a7-e436c26c6f95", "phase2.1", runtime.c_str(), logs.c_str(), reportInit, nullptr};
        FrameLabD3D12Fg* raw{};
        sdk(framelab_d3d12_fg_create(&config, &raw), "initialize NGX and query native FG support");
        const std::unique_ptr<FrameLabD3D12Fg, decltype(&framelab_d3d12_fg_destroy)>
            fg(raw, &framelab_d3d12_fg_destroy);
        const auto recorded = [&](FrameLabStatus result, const char* step) {
            if (result == FRAMELAB_OK) return;
            // Discard the list. A native recording error can leave partial commands.
            gpu.list->Close();
            framelab_d3d12_fg_cancel(raw);
            sdk(result, step);
        };
        gpu.begin();
        recorded(framelab_d3d12_fg_record_initialize(raw, gpu.list.Get()), "record native FG create");
        gpu.enqueue(raw);
        gpu.wait();
        sdk(framelab_d3d12_fg_retire(raw), "retire native FG create");

        auto color = gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM);
        auto depth = gpu.texture(DXGI_FORMAT_R32_FLOAT);
        auto motion = gpu.texture(DXGI_FORMAT_R16G16_FLOAT);
        auto generated = gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM);
        unsigned releases = 0;
        auto colorLease = pin(color, releases);
        auto depthLease = pin(depth, releases);
        auto motionLease = pin(motion, releases);
        constexpr size_t count = extent.width * extent.height;
        std::vector<uint32_t> pixels(count), previous, output(count), clear(count, sentinel);
        std::vector<float> depths(count);
        std::vector<uint16_t> vectors(count * 2);
        const auto view = camera();
        auto time = std::chrono::steady_clock::now();
        unsigned verified = 0;
        for (unsigned i = 0; i < frames; ++i) {
            const auto now = std::chrono::steady_clock::now();
            const unsigned left = 160 + i * 32;
            for (unsigned y = 0; y < extent.height; ++y) {
                for (unsigned x = 0; x < extent.width; ++x) {
                    const size_t p = y * extent.width + x;
                    const bool object = x >= left && x < left + 160 && y >= 240 && y < 480;
                    pixels[p] = object ? 0xff2080f0 : (((x / 32 + y / 32) % 2) ? 0xff303030 : 0xff181818);
                    depths[p] = object ? 0.3f : 0.9f;
                    vectors[2 * p] = DirectX::PackedVector::XMConvertFloatToHalf(object ? -32.0f / extent.width : 0);
                    vectors[2 * p + 1] = 0;
                }
            }
            gpu.begin();
            constexpr auto shader = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            gpu.upload(color, pixels.data(), shader);
            gpu.upload(depth, depths.data(), shader);
            gpu.upload(motion, vectors.data(), shader);
            gpu.upload(generated, clear.data(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            FrameLabFgFrame frame{};
            frame.struct_size = sizeof frame;
            frame.abi_version = FRAMELAB_ABI_VERSION;
            frame.stream_id = frame.device_id = frame.generation = 1;
            frame.frame_index = i;
            frame.render_extent = frame.output_extent = extent;
            frame.delta_seconds = i ? std::chrono::duration<double>(now - time).count() : 1.0 / 60;
            time = now;
            frame.reset_history = i ? 0 : FRAMELAB_RESET_INITIAL;
            frame.camera = view;
            frame.producer = {1, i + 1u, 0, 0, FRAMELAB_COMPLETION_SAME_COMMAND_STREAM, nullptr};
            frame.color = {11, i, 1, extent, FRAMELAB_FORMAT_RGBA8_UNORM,
                           FRAMELAB_STATE_SHADER_READ, FRAMELAB_PROVENANCE_SYNTHETIC, colorLease.get()};
            frame.depth = {12, i, 1, extent, FRAMELAB_FORMAT_R32_FLOAT,
                           FRAMELAB_STATE_SHADER_READ, FRAMELAB_PROVENANCE_SYNTHETIC, depthLease.get()};
            frame.motion_vectors = {13, i, 1, extent, FRAMELAB_FORMAT_RG16_FLOAT,
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
            }
            if (i == 2) {
                auto stale = frame;
                --stale.depth.frame_index;
                expect(framelab_d3d12_fg_record(raw, &stale, &bindings) == FRAMELAB_INVALID_SEQUENCE,
                       "stale depth was not rejected");
                std::puts("ABI_CHECK stale_depth_rejected=1");
            }
            recorded(framelab_d3d12_fg_record(raw, &frame, &bindings), "record native FG evaluate");
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
            auto readback = gpu.readback(generated, layout);
            Gate gate;
            if (i + 1 == frames) {
                check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)), "create test execution gate");
                check(gpu.queue->Wait(gate.fence.Get(), 1), "block queue until retention is verified");
            }
            gpu.enqueue(raw);
            if (gate.fence) {
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
            }
            gpu.wait();
            sdk(framelab_d3d12_fg_retire(raw), "retire completed frame");
            void* mapped{};
            check(readback->Map(0, nullptr, &mapped), "map generated texture readback");
            constexpr size_t rowBytes = extent.width * sizeof(uint32_t);
            for (unsigned y = 0; y < extent.height; ++y)
                std::memcpy(output.data() + y * extent.width,
                    static_cast<char*>(mapped) + layout.Offset + y * layout.Footprint.RowPitch, rowBytes);
            readback->Unmap(0, nullptr);
            bool written = true;
            for (const auto pixel : output) written &= pixel != sentinel;
            if (i && written && output != pixels && output != previous) ++verified;
            previous = pixels;
            std::printf("SDK_FG_FRAME frame=%u fence=%llu ngx=%#x\n", i,
                        static_cast<unsigned long long>(gpu.signal), framelab_d3d12_fg_last_ngx_result(raw));
        }
        writeReadback(logs / L"generated-5.rgba", output);
        std::printf("SDK_FG_SUMMARY real_frames=%u verified=%u final_lease_releases=%u\n", frames, verified, releases);
        return verified == frames - 1 && releases == 3 ? 0 : 1;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "D3D12 public SDK test: %s\n", ex.what());
        return 1;
    }
}
