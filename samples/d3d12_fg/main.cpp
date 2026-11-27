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
#include <cmath>
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

// The color and generated-image format of a run. Every format here has four-byte texels.
struct ColorMode {
    const wchar_t* name;
    uint32_t format; // FRAMELAB_FORMAT_*
    DXGI_FORMAT dxgi;
    bool hdr10;
};
constexpr ColorMode colorModes[] = {
    {L"rgba8", FRAMELAB_FORMAT_RGBA8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false},
    {L"bgra8", FRAMELAB_FORMAT_BGRA8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, false},
    {L"rgb10a2", FRAMELAB_FORMAT_RGB10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, false},
    {L"hdr10", FRAMELAB_FORMAT_RGB10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, true},
};

// HDR10: sRGB-encoded SDR colors shown at the BT.2408 reference white of 203 cd/m², converted
// from BT.709 to BT.2020 primaries (BT.2087) and PQ-encoded (SMPTE ST 2084), 10-bit full range.
float pq(float nits)
{
    constexpr float m1 = 2610.f / 16384, m2 = 2523.f / 4096 * 128, c1 = 3424.f / 4096,
        c2 = 2413.f / 4096 * 32, c3 = 2392.f / 4096 * 32;
    const float y = std::pow(std::clamp(nits / 10000.f, 0.f, 1.f), m1);
    return std::pow((c1 + c2 * y) / (1 + c3 * y), m2);
}

float linear(uint32_t srgb8)
{
    const float c = srgb8 / 255.f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

uint32_t ten(float value) { return uint32_t(std::lround(std::clamp(value, 0.f, 1.f) * 1023)); }

// Encodes an 8-bit sRGB color (r, g, b) as one opaque texel of the mode's format.
uint32_t encode(const ColorMode& mode, uint32_t r, uint32_t g, uint32_t b)
{
    if (mode.dxgi == DXGI_FORMAT_R8G8B8A8_UNORM) return 0xff000000u | b << 16 | g << 8 | r;
    if (mode.dxgi == DXGI_FORMAT_B8G8R8A8_UNORM) return 0xff000000u | r << 16 | g << 8 | b;
    float rgb[3]{r / 255.f, g / 255.f, b / 255.f};
    if (mode.hdr10) {
        constexpr float toBt2020[3][3]{{0.6274f, 0.3293f, 0.0433f}, {0.0691f, 0.9195f, 0.0114f},
                                       {0.0164f, 0.0880f, 0.8956f}};
        const float in[3]{linear(r) * 203, linear(g) * 203, linear(b) * 203};
        for (unsigned i = 0; i < 3; ++i)
            rgb[i] = pq(toBt2020[i][0] * in[0] + toBt2020[i][1] * in[1] + toBt2020[i][2] * in[2]);
    }
    return 3u << 30 | ten(rgb[2]) << 20 | ten(rgb[1]) << 10 | ten(rgb[0]);
}

// Red and blue channel codes of a texel; `max` is the channel's largest code.
void redBlue(const ColorMode& mode, uint32_t texel, int& red, int& blue)
{
    if (mode.dxgi == DXGI_FORMAT_R8G8B8A8_UNORM) {
        red = texel & 0xff;
        blue = (texel >> 16) & 0xff;
    } else if (mode.dxgi == DXGI_FORMAT_B8G8R8A8_UNORM) {
        red = (texel >> 16) & 0xff;
        blue = texel & 0xff;
    } else {
        red = texel & 0x3ff;
        blue = (texel >> 20) & 0x3ff;
    }
}

const ColorMode* parseColor(const wchar_t* text)
{
    for (const auto& mode : colorModes)
        if (std::wcscmp(text, mode.name) == 0) return &mode;
    return nullptr;
}

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
    ComPtr<ID3D12InfoQueue> messages; // Set when the debug layer is enabled.
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t signal = 0;

    ~Gpu() { if (event) CloseHandle(event); }

    // With `validate`, the D3D12 debug layer and GPU-based validation are enabled before the
    // device is created, and every message is kept for writeMessages().
    void initialize(bool validate)
    {
        expect(event != nullptr, "create fence event");
        if (validate) {
            ComPtr<ID3D12Debug1> layer;
            check(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)), "enable the D3D12 debug layer (Graphics Tools)");
            layer->EnableDebugLayer();
            layer->SetEnableGPUBasedValidation(TRUE);
            layer->SetEnableSynchronizedCommandQueueValidation(TRUE);
        }
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
        if (validate) {
            check(device.As(&messages), "query the D3D12 info queue");
            check(messages->SetMessageCountLimit(UINT64(-1)), "keep every D3D12 debug message");
        }
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "create queue");
        check(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator)), "create allocator");
        check(device->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "create list");
        check(list->Close(), "close initial list");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create completion fence");
    }

    // Writes the stored debug-layer messages to `path`, one per line (severity, ID, text), and
    // prints a count per severity. FrameLab and NGX share the device, so a message may come from
    // either.
    void writeMessages(const std::filesystem::path& path)
    {
        if (!messages) return;
        FILE* file = _wfopen(path.c_str(), L"w");
        expect(file != nullptr, "open D3D12 debug message log");
        unsigned counts[5]{};
        const auto stored = messages->GetNumStoredMessages();
        for (UINT64 i = 0; i < stored; ++i) {
            SIZE_T length = 0;
            if (FAILED(messages->GetMessage(i, nullptr, &length)) || !length) continue;
            std::vector<char> storage(length);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(messages->GetMessage(i, message, &length))) continue;
            if (unsigned(message->Severity) < 5) ++counts[message->Severity];
            std::fprintf(file, "%d %d %s\n", int(message->Severity), int(message->ID), message->pDescription);
        }
        std::fclose(file);
        std::printf("SDK_FG_DEBUG corruption=%u error=%u warning=%u info=%u message=%u discarded=%llu\n",
                    counts[D3D12_MESSAGE_SEVERITY_CORRUPTION], counts[D3D12_MESSAGE_SEVERITY_ERROR],
                    counts[D3D12_MESSAGE_SEVERITY_WARNING], counts[D3D12_MESSAGE_SEVERITY_INFO],
                    counts[D3D12_MESSAGE_SEVERITY_MESSAGE],
                    static_cast<unsigned long long>(messages->GetNumMessagesDiscardedByMessageCountLimit()));
        messages->ClearStoredMessages();
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

// A moving orange rectangle over a grey checkerboard. Color is drawn at the output extent in the
// run's format; depth and motion vectors sample the same scene at the render extent. With equal
// extents and RGBA8 this is the original synthetic input. Motion is in UV units, so its value does
// not depend on the extent.
struct Scene {
    FrameLabExtent render;
    uint32_t rectangle, light, dark;
    std::vector<uint32_t> pixels;
    std::vector<float> depths;
    std::vector<uint16_t> vectors;

    Scene(FrameLabExtent renderExtent, const ColorMode& mode)
        : render(renderExtent), rectangle(encode(mode, 0xf0, 0x80, 0x20)), light(encode(mode, 0x30, 0x30, 0x30)),
          dark(encode(mode, 0x18, 0x18, 0x18)), pixels(size_t(outputExtent.width) * outputExtent.height),
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
                pixels[size_t(y) * outputExtent.width + x] = object(x, y, frame) ? rectangle :
                    (((x / 32 + y / 32) % 2) ? light : dark);
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
// rectangle is orange; the checkerboard is grey. A texel counts as rectangle when red exceeds blue
// by 0x60/0xd0 of the rectangle's own difference: 0x60 in 8-bit RGBA8, as before.
void edges(const ColorMode& mode, uint32_t rectangle, const std::vector<uint32_t>& image, int& left, int& right)
{
    left = right = -1;
    int rectangleRed = 0, rectangleBlue = 0;
    redBlue(mode, rectangle, rectangleRed, rectangleBlue);
    const int threshold = (rectangleRed - rectangleBlue) * 0x60 / 0xd0;
    const auto* row = image.data() + size_t(outputExtent.height / 2) * outputExtent.width;
    for (unsigned x = 0; x < outputExtent.width; ++x) {
        int red = 0, blue = 0;
        redBlue(mode, row[x], red, blue);
        if (red - blue < threshold) continue;
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

// sizeof an older FrameLabD3D12FgConfig layout: its fields rounded up to the struct alignment.
constexpr uint32_t layoutSize(size_t fields)
{
    constexpr size_t align = alignof(FrameLabD3D12FgConfig);
    return uint32_t((fields + align - 1) / align * align);
}

// Descriptor sizes, render extents and color configurations, checked against the real device
// before the run.
void descriptorChecks(const FrameLabD3D12FgConfig& base)
{
    constexpr uint32_t v1 = layoutSize(offsetof(FrameLabD3D12FgConfig, render_extent));
    constexpr uint32_t extentSize = layoutSize(offsetof(FrameLabD3D12FgConfig, color_format));
    auto config = base;
    config.struct_size = v1;
    // A v0.4.0 client passes the v1 size. The bytes after it are not part of its struct, so the
    // invalid extent and color values placed there must not be read.
    config.render_extent = {1, 0};
    config.color_format = FRAMELAB_FORMAT_RGBA16_FLOAT;
    config.flags = ~0u;
    expect(tryCreate(config) == FRAMELAB_OK, "v0.4.0-size config was not accepted");
    std::printf("ABI_CHECK v1_size=%u created=1\n", v1);
    // A client of the render_extent header passes that size; its render extent is read, the color
    // fields after it are not.
    config.struct_size = extentSize;
    config.render_extent = base.render_extent;
    expect(tryCreate(config) == FRAMELAB_OK, "render_extent-size config was not accepted");
    std::printf("ABI_CHECK extent_size=%u created=1\n", extentSize);
    for (uint32_t size = v1 - 1; size < sizeof config; ++size) {
        if (size == v1 || size == extentSize) continue;
        config.struct_size = size;
        expect(tryCreate(config) == FRAMELAB_ABI_MISMATCH, "partial config size was not rejected");
    }
    std::puts("ABI_CHECK partial_sizes_rejected=1");
    // Color format and flag combinations that create rejects before initializing NGX.
    struct Combination { uint32_t format, flags; FrameLabStatus expected; };
    constexpr Combination invalid[]{
        {FRAMELAB_FORMAT_RGBA16_FLOAT, 0, FRAMELAB_UNSUPPORTED},
        {0, FRAMELAB_D3D12_FG_HDR10, FRAMELAB_INVALID_ARGUMENT},
        {FRAMELAB_FORMAT_RGBA8_UNORM, FRAMELAB_D3D12_FG_HDR10, FRAMELAB_INVALID_ARGUMENT},
        {FRAMELAB_FORMAT_BGRA8_UNORM, FRAMELAB_D3D12_FG_HDR10, FRAMELAB_INVALID_ARGUMENT},
        {FRAMELAB_FORMAT_RGBA16_FLOAT, FRAMELAB_D3D12_FG_HDR10, FRAMELAB_INVALID_ARGUMENT},
        {FRAMELAB_FORMAT_R32_FLOAT, 0, FRAMELAB_INVALID_ARGUMENT},
        {FRAMELAB_FORMAT_D24S8, 0, FRAMELAB_INVALID_ARGUMENT},
        {FRAMELAB_FORMAT_RGB10A2_UNORM + 1, 0, FRAMELAB_INVALID_ARGUMENT},
        {FRAMELAB_FORMAT_RGB10A2_UNORM, 2, FRAMELAB_INVALID_ARGUMENT},
        {0, 0x80000000u, FRAMELAB_INVALID_ARGUMENT},
    };
    config = base;
    for (const auto& combination : invalid) {
        config.color_format = combination.format;
        config.flags = combination.flags;
        expect(tryCreate(config) == combination.expected, "invalid color configuration was not rejected");
    }
    std::puts("ABI_CHECK invalid_color_configs_rejected=1");
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
RunResult run(Gpu& gpu, FrameLabD3D12Fg* raw, const ColorMode& mode, FrameLabExtent render, unsigned count, bool full)
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
    auto color = gpu.texture(mode.dxgi, outputExtent);
    auto depth = gpu.texture(DXGI_FORMAT_R32_FLOAT, render);
    auto motion = gpu.texture(DXGI_FORMAT_R16G16_FLOAT, render);
    auto generated = gpu.texture(mode.dxgi, outputExtent);
    auto colorLease = pin(color, result.releases);
    auto depthLease = pin(depth, result.releases);
    auto motionLease = pin(motion, result.releases);
    const size_t count2d = size_t(outputExtent.width) * outputExtent.height;
    std::vector<uint32_t> previous, clear(count2d, sentinel);
    result.output.resize(count2d);
    Scene scene(render, mode);
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
        frame.color = {11, i, 1, outputExtent, mode.format,
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

            // Frame metadata in a color format other than the configured one, including FP16.
            auto otherFormat = frame;
            otherFormat.color.format = mode.format == FRAMELAB_FORMAT_RGBA8_UNORM ?
                FRAMELAB_FORMAT_BGRA8_UNORM : FRAMELAB_FORMAT_RGBA8_UNORM;
            expect(framelab_d3d12_fg_record(raw, &otherFormat, &bindings) == FRAMELAB_INVALID_IMAGE_DESCRIPTION,
                   "frame in another color format was not rejected");
            std::puts("ABI_CHECK color_format_mismatch_rejected=1");
            otherFormat.color.format = FRAMELAB_FORMAT_RGBA16_FLOAT;
            expect(framelab_d3d12_fg_record(raw, &otherFormat, &bindings) == FRAMELAB_INVALID_IMAGE_DESCRIPTION,
                   "FP16 color frame was not rejected");
            std::puts("ABI_CHECK fp16_frame_rejected=1");

            // Correct metadata, but the native color input or the generated image has another
            // format.
            const auto otherDxgi = mode.dxgi == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R10G10B10A2_UNORM :
                                                                            DXGI_FORMAT_R8G8B8A8_UNORM;
            std::vector<uint32_t> zeros4(size_t(outputExtent.width) * outputExtent.height);
            unsigned otherReleases = 0;
            auto otherColor = gpu.texture(otherDxgi, outputExtent);
            gpu.upload(otherColor, zeros4.data(), shader);
            auto otherLease = pin(otherColor, otherReleases);
            auto otherColorFrame = frame;
            otherColorFrame.color.lease = otherLease.get();
            auto otherColorBindings = bindings;
            otherColorBindings.color = {11, otherColor.resource.Get(), static_cast<uint32_t>(otherColor.state)};
            expect(framelab_d3d12_fg_record(raw, &otherColorFrame, &otherColorBindings) == FRAMELAB_INVALID_RESOURCE_STATE,
                   "native color in another format was not rejected");
            std::puts("ABI_CHECK native_color_format_mismatch_rejected=1");
            auto otherOutput = gpu.texture(otherDxgi, outputExtent);
            gpu.upload(otherOutput, zeros4.data(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            auto otherOutputBindings = bindings;
            otherOutputBindings.generated = {14, otherOutput.resource.Get(), static_cast<uint32_t>(otherOutput.state)};
            expect(framelab_d3d12_fg_record(raw, &frame, &otherOutputBindings) == FRAMELAB_INVALID_RESOURCE_STATE,
                   "generated image in another format was not rejected");
            std::puts("ABI_CHECK native_output_format_mismatch_rejected=1");
            gpu.uploads.push_back(otherColor.resource);
            gpu.uploads.push_back(otherOutput.resource);
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
        edges(mode, scene.rectangle, result.output, left, right);
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
    const ColorMode* mode = &colorModes[0];
    bool validate = false;
    bool usage = argc < 4;
    for (int i = 4; !usage && i < argc; i += 2) {
        if (std::wcscmp(argv[i], L"--debug-layer") == 0) {
            validate = true;
            --i; // No value.
        } else if (i + 1 >= argc) {
            usage = true;
        } else if (std::wcscmp(argv[i], L"--render") == 0) {
            usage = !parseExtent(argv[i + 1], render);
        } else if (std::wcscmp(argv[i], L"--color") == 0) {
            usage = !(mode = parseColor(argv[i + 1]));
        } else {
            usage = true;
        }
    }
    if (usage) {
        std::fprintf(stderr, "usage: d3d12_fg <NGX runtime directory> <output directory> <app-local proxy or -> "
                             "[--render <width>x<height>, at most 1280x720] [--color rgba8|bgra8|rgb10a2|hdr10] "
                             "[--debug-layer]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Gpu gpu;
    std::filesystem::path logs;
    try {
        const auto runtime = std::filesystem::absolute(argv[1]);
        logs = std::filesystem::absolute(argv[2]);
        std::filesystem::create_directories(logs);
        expect(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS) != 0,
               "set app-local DLL search policy");
        expect(AddDllDirectory(runtime.c_str()) != nullptr, "add external NGX runtime directory");
        if (std::wcscmp(argv[3], L"-") != 0) {
            const auto proxy = std::filesystem::absolute(argv[3]);
            expect(LoadLibraryW(proxy.c_str()) != nullptr, "load configured app-local proxy");
        }
        gpu.initialize(validate);
        std::printf("SDK_FG_EXTENTS output=%ux%u render=%ux%u\n", outputExtent.width, outputExtent.height,
                    render.width, render.height);
        std::printf("SDK_FG_VALIDATION debug_layer=%d gpu_based=%d\n", int(validate), int(validate));
        // FrameLab needs the color format to be loadable, sampleable and a typed UAV store target.
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{mode->dxgi};
        check(gpu.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support),
              "query color format support");
        constexpr auto required1 = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
            D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
        const bool supported = (support.Support1 & required1) == required1 &&
            (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
        std::printf("SDK_FG_COLOR mode=%ls format=%u dxgi=%u hdr10=%d device_support=%d\n", mode->name,
                    mode->format, unsigned(mode->dxgi), int(mode->hdr10), int(supported));
        FrameLabD3D12FgConfig config{sizeof config, FRAMELAB_ABI_VERSION, 1, 1,
            gpu.device.Get(), gpu.queue.Get(), gpu.fence.Get(), outputExtent,
            "31246941-7a24-484c-a4a7-e436c26c6f95", "phase2.1", runtime.c_str(), logs.c_str(), reportInit, nullptr,
            render, mode->format, mode->hdr10 ? uint32_t(FRAMELAB_D3D12_FG_HDR10) : 0u};
        if (!supported) {
            // The device cannot run this format: create must refuse it, and the host falls back.
            expect(tryCreate(config) == FRAMELAB_UNSUPPORTED, "unsupported color format was not rejected");
            std::puts("ABI_CHECK unsupported_color_rejected=1");
            gpu.writeMessages(logs / L"d3d12-debug.log");
            return 3;
        }
        descriptorChecks(config);

        FrameLabD3D12Fg* raw{};
        sdk(framelab_d3d12_fg_create(&config, &raw), "initialize NGX and query native FG support");
        std::unique_ptr<FrameLabD3D12Fg, decltype(&framelab_d3d12_fg_destroy)> fg(raw, &framelab_d3d12_fg_destroy);
        const auto first = run(gpu, raw, *mode, render, frames, true);
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
        const auto second = run(gpu, raw, *mode, config.render_extent, recreatedFrames, false);
        std::printf("SDK_FG_RECREATED render=%ux%u real_frames=%u verified=%u final_lease_releases=%u max_edge_error=%d\n",
                    config.render_extent.width, config.render_extent.height, recreatedFrames, second.verified,
                    second.releases, second.maxEdgeError);
        // Destroy the backend first, so the log also covers NGX feature release and shutdown.
        fg.reset();
        gpu.writeMessages(logs / L"d3d12-debug.log");
        return first.verified == frames - 1 && first.releases == 3 &&
            second.verified == recreatedFrames - 1 && second.releases == 3 ? 0 : 1;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "D3D12 public SDK test: %s\n", ex.what());
        try {
            if (!logs.empty()) gpu.writeMessages(logs / L"d3d12-debug.log");
        } catch (const std::exception&) {
        }
        return 1;
    }
}
