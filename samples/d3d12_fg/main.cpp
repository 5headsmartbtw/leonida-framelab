// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <framelab/d3d12_fg.h>

#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
constexpr FrameLabExtent outputExtent{1280, 720};
constexpr unsigned frames = 6;
constexpr unsigned recreatedFrames = 3;
constexpr uint32_t sentinel = 0xffff00ff;

bool operator==(FrameLabExtent a, FrameLabExtent b) { return a.width == b.width && a.height == b.height; }

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

    // Every texture in this sample uses a four-byte texel format.
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

FrameLabCamera camera()
{
    FrameLabCamera result{};
    result.flags = FRAMELAB_CAMERA_VERIFIED | FRAMELAB_CAMERA_MOTION_INCLUDED;
    result.projection_space = result.reprojection_space = FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE;
    result.motion_convention = FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV;
    result.aspect = float(outputExtent.width) / outputExtent.height;
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

// A moving rectangle over a checkerboard. Color is drawn at the output extent; depth and motion
// vectors sample the same scene at the render extent. With equal extents this is the original
// synthetic input. Motion is in UV units, so its value does not depend on the extent.
struct Scene {
    FrameLabExtent render;
    std::vector<uint32_t> pixels;
    std::vector<float> depths;
    std::vector<uint16_t> vectors;

    explicit Scene(FrameLabExtent renderExtent)
        : render(renderExtent), pixels(size_t(outputExtent.width) * outputExtent.height),
          depths(size_t(renderExtent.width) * renderExtent.height), vectors(depths.size() * 2) {}

    static unsigned left(unsigned frame) { return 160 + frame * 32; }
    static bool object(unsigned x, unsigned y, unsigned frame)
    {
        return x >= left(frame) && x < left(frame) + 160 && y >= 240 && y < 480;
    }

    void draw(unsigned frame)
    {
        for (unsigned y = 0; y < outputExtent.height; ++y)
            for (unsigned x = 0; x < outputExtent.width; ++x)
                pixels[size_t(y) * outputExtent.width + x] = object(x, y, frame) ? 0xff2080f0 :
                    (((x / 32 + y / 32) % 2) ? 0xff303030 : 0xff181818);
        const auto motion = DirectX::PackedVector::XMConvertFloatToHalf(-32.0f / outputExtent.width);
        for (unsigned y = 0; y < render.height; ++y) {
            const auto oy = unsigned((y + 0.5) * outputExtent.height / render.height);
            for (unsigned x = 0; x < render.width; ++x) {
                const auto ox = unsigned((x + 0.5) * outputExtent.width / render.width);
                const size_t p = size_t(y) * render.width + x;
                const bool inside = object(ox, oy, frame);
                depths[p] = inside ? 0.3f : 0.9f;
                vectors[2 * p] = inside ? motion : 0;
                vectors[2 * p + 1] = 0;
            }
        }
    }
};

// Left and right edges of the rectangle on the middle row of a readback, or -1 if absent. The
// rectangle is orange; the checkerboard is grey.
void edges(const std::vector<uint32_t>& image, int& left, int& right)
{
    left = right = -1;
    const auto* row = image.data() + size_t(outputExtent.height / 2) * outputExtent.width;
    for (unsigned x = 0; x < outputExtent.width; ++x) {
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

struct RunResult {
    unsigned verified = 0;
    unsigned releases = 0;
    int maxEdgeError = 0;
    std::vector<uint32_t> output;
};

// Initializes the backend, then records, submits and retires `count` frames. The full run adds the
// rejection, retention and destruction checks; a recreated backend checks that it needs a reset.
RunResult run(Gpu& gpu, FrameLabD3D12Fg* raw, FrameLabExtent render, unsigned count, bool full)
{
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

    RunResult result;
    auto color = gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM, outputExtent);
    auto depth = gpu.texture(DXGI_FORMAT_R32_FLOAT, render);
    auto motion = gpu.texture(DXGI_FORMAT_R16G16_FLOAT, render);
    auto generated = gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM, outputExtent);
    auto colorLease = pin(color, result.releases);
    auto depthLease = pin(depth, result.releases);
    auto motionLease = pin(motion, result.releases);
    const size_t count2d = size_t(outputExtent.width) * outputExtent.height;
    std::vector<uint32_t> previous, clear(count2d, sentinel);
    result.output.resize(count2d);
    Scene scene(render);
    const auto view = camera();
    auto time = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < count; ++i) {
        const auto now = std::chrono::steady_clock::now();
        scene.draw(i);
        gpu.begin();
        constexpr auto shader = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        gpu.upload(color, scene.pixels.data(), shader);
        gpu.upload(depth, scene.depths.data(), shader);
        gpu.upload(motion, scene.vectors.data(), shader);
        gpu.upload(generated, clear.data(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        FrameLabFgFrame frame{};
        frame.struct_size = sizeof frame;
        frame.abi_version = FRAMELAB_ABI_VERSION;
        frame.stream_id = frame.device_id = frame.generation = 1;
        frame.frame_index = i;
        frame.render_extent = render;
        frame.output_extent = outputExtent;
        frame.delta_seconds = i ? std::chrono::duration<double>(now - time).count() : 1.0 / 60;
        time = now;
        frame.reset_history = i ? 0 : FRAMELAB_RESET_INITIAL;
        frame.camera = view;
        frame.producer = {1, i + 1u, 0, 0, FRAMELAB_COMPLETION_SAME_COMMAND_STREAM, nullptr};
        frame.color = {11, i, 1, outputExtent, FRAMELAB_FORMAT_RGBA8_UNORM,
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
        // Frame 0 carries the initial reset, so the rejections below (each of which invalidates
        // history) leave the later frames' continuity unchanged.
        if (full && i == 0) {
            auto bad = bindings;
            bad.color.state = D3D12_RESOURCE_STATE_COPY_DEST;
            expect(framelab_d3d12_fg_record(raw, &frame, &bad) == FRAMELAB_INVALID_RESOURCE_STATE,
                   "wrong native resource state was not rejected");
            std::puts("ABI_CHECK invalid_native_state_rejected=1");

            // Self-consistent metadata at a render extent the backend was not created with.
            auto other = frame;
            other.render_extent = render == outputExtent ?
                FrameLabExtent{outputExtent.width / 2, outputExtent.height / 2} : outputExtent;
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
        if (!full && i == 0) {
            auto unreset = frame;
            unreset.reset_history = 0;
            expect(framelab_d3d12_fg_record(raw, &unreset, &bindings) == FRAMELAB_RESET_REQUIRED,
                   "recreated backend accepted a frame without a reset");
            std::puts("ABI_CHECK recreated_requires_reset=1");
        }
        if (full && i == 2) {
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
        if (full && i + 1 == count) {
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
            expect(framelab_d3d12_fg_retire(raw) == FRAMELAB_NOT_READY && result.releases == 0,
                   "early retirement released pending resources");
            expect(framelab_d3d12_fg_destroy(raw) == FRAMELAB_BUSY, "pending backend was destructible");
            check(gate.fence->Signal(1), "release test execution gate");
            std::puts("ABI_CHECK early_retire_rejected=1 pending_destroy_rejected=1");
        }
        gpu.wait();
        sdk(framelab_d3d12_fg_retire(raw), "retire completed frame");
        void* mapped{};
        check(readback->Map(0, nullptr, &mapped), "map generated texture readback");
        const size_t rowBytes = size_t(outputExtent.width) * sizeof(uint32_t);
        for (unsigned y = 0; y < outputExtent.height; ++y)
            std::memcpy(result.output.data() + size_t(y) * outputExtent.width,
                static_cast<char*>(mapped) + layout.Offset + y * layout.Footprint.RowPitch, rowBytes);
        readback->Unmap(0, nullptr);
        bool written = true;
        for (const auto pixel : result.output) written &= pixel != sentinel;
        const bool interpolated = i && written && result.output != scene.pixels && result.output != previous;
        if (interpolated) ++result.verified;
        previous = scene.pixels;
        // The generated frame lies between frames i - 1 and i, so the rectangle is expected
        // halfway, 16 output pixels left of its position in frame i.
        int left = -1, right = -1;
        edges(result.output, left, right);
        const int expectedLeft = int(Scene::left(i)) - 16;
        if (interpolated) {
            const int error = left < 0 ? int(outputExtent.width) :
                std::max(std::abs(left - expectedLeft), std::abs(right - (expectedLeft + 160)));
            result.maxEdgeError = std::max(result.maxEdgeError, error);
        }
        std::printf("SDK_FG_FRAME frame=%u fence=%llu ngx=%#x left=%d right=%d expected_left=%d\n", i,
                    static_cast<unsigned long long>(gpu.signal), framelab_d3d12_fg_last_ngx_result(raw),
                    left, right, i ? expectedLeft : -1);
    }
    // FrameLab released its references at retirement; dropping ours runs the lease callbacks.
    colorLease.reset();
    depthLease.reset();
    motionLease.reset();
    return result;
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
    return extent.width && extent.height && extent.width <= outputExtent.width &&
        extent.height <= outputExtent.height;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    FrameLabExtent render = outputExtent;
    const bool renderArgument = argc == 6 && std::wcscmp(argv[4], L"--render") == 0;
    if ((argc != 4 && !renderArgument) || (renderArgument && !parseExtent(argv[5], render))) {
        std::fprintf(stderr, "usage: d3d12_fg <NGX runtime directory> <output directory> <app-local proxy or -> "
                             "[--render <width>x<height>, at most 1280x720]\n");
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
        std::printf("SDK_FG_EXTENTS output=%ux%u render=%ux%u\n", outputExtent.width, outputExtent.height,
                    render.width, render.height);
        FrameLabD3D12FgConfig config{sizeof config, FRAMELAB_ABI_VERSION, 1, 1,
            gpu.device.Get(), gpu.queue.Get(), gpu.fence.Get(), outputExtent,
            "31246941-7a24-484c-a4a7-e436c26c6f95", "phase2.1", runtime.c_str(), logs.c_str(), reportInit, nullptr,
            renderArgument ? render : FrameLabExtent{}};
        descriptorChecks(config);

        FrameLabD3D12Fg* raw{};
        sdk(framelab_d3d12_fg_create(&config, &raw), "initialize NGX and query native FG support");
        std::unique_ptr<FrameLabD3D12Fg, decltype(&framelab_d3d12_fg_destroy)> fg(raw, &framelab_d3d12_fg_destroy);
        const auto first = run(gpu, raw, render, frames, true);
        writeReadback(logs / L"generated-5.rgba", first.output);
        std::printf("SDK_FG_SUMMARY real_frames=%u verified=%u final_lease_releases=%u max_edge_error=%d\n",
                    frames, first.verified, first.releases, first.maxEdgeError);

        // A different render extent needs a new backend: destroy the idle one, create another and
        // record its initialization. Its history starts over.
        sdk(framelab_d3d12_fg_destroy(fg.release()), "destroy idle backend");
        config.render_extent = render == outputExtent ?
            FrameLabExtent{outputExtent.width * 3 / 4, outputExtent.height * 3 / 4} : outputExtent;
        sdk(framelab_d3d12_fg_create(&config, &raw), "recreate backend at another render extent");
        fg.reset(raw);
        const auto second = run(gpu, raw, config.render_extent, recreatedFrames, false);
        std::printf("SDK_FG_RECREATED render=%ux%u real_frames=%u verified=%u final_lease_releases=%u max_edge_error=%d\n",
                    config.render_extent.width, config.render_extent.height, recreatedFrames, second.verified,
                    second.releases, second.maxEdgeError);
        return first.verified == frames - 1 && first.releases == 3 &&
            second.verified == recreatedFrames - 1 && second.releases == 3 ? 0 : 1;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "D3D12 public SDK test: %s\n", ex.what());
        return 1;
    }
}
