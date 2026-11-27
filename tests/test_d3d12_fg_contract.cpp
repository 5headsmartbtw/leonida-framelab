// SPDX-License-Identifier: MIT
// Portable checks of the D3D12 FG configuration rules. No Windows, D3D12 or NGX headers.
#include "backends/d3d12/fg_contract.hpp"

#include <cstdio>
#include <initializer_list>

using namespace framelab::d3d12::fg;

#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #x); return 1; } } while (false)

int main()
{
    unsigned checks = 0;
    static_assert(FRAMELAB_FORMAT_RGB10A2_UNORM == 9, "public RGB10A2 value");

    // The x64 FrameLabD3D12FgConfig: v0.4.0 fields end at 104, render_extent at 112, and
    // color_format/flags make it 120 bytes.
    constexpr ConfigLayout x64{104, 112, 120, 8};
    CHECK(copiedBytes(x64, 104) == 104);
    CHECK(copiedBytes(x64, 112) == 112);
    CHECK(copiedBytes(x64, 120) == 120);
    CHECK(copiedBytes(x64, 121) == 120); // A later, larger layout: only the known fields.
    CHECK(copiedBytes(x64, 0) == 0);
    CHECK(copiedBytes(x64, 103) == 0);
    for (uint32_t size = 105; size < 112; ++size) CHECK(copiedBytes(x64, size) == 0);
    for (uint32_t size = 113; size < 120; ++size) CHECK(copiedBytes(x64, size) == 0);

    // Fields that end before the alignment boundary: the caller's sizeof includes tail padding,
    // which is never copied, and the unpadded field end is not a valid size.
    constexpr ConfigLayout padded{100, 108, 120, 8};
    CHECK(copiedBytes(padded, 104) == 100);
    CHECK(copiedBytes(padded, 112) == 108);
    CHECK(copiedBytes(padded, 100) == 0);
    CHECK(copiedBytes(padded, 108) == 0);
    CHECK(copiedBytes(padded, 116) == 0);
    CHECK(copiedBytes(padded, 120) == 120);

    // Zero defaults keep today's behaviour: RGBA8, SDR.
    Color color;
    CHECK(selectColor(0, 0, color) == FRAMELAB_OK);
    CHECK(color.format == FRAMELAB_FORMAT_RGBA8_UNORM && color.dxgi == dxgi::rgba8_unorm && !color.hdr10);
    CHECK(selectColor(FRAMELAB_FORMAT_RGBA8_UNORM, 0, color) == FRAMELAB_OK && color.dxgi == dxgi::rgba8_unorm);
    CHECK(selectColor(FRAMELAB_FORMAT_BGRA8_UNORM, 0, color) == FRAMELAB_OK);
    CHECK(color.format == FRAMELAB_FORMAT_BGRA8_UNORM && color.dxgi == dxgi::bgra8_unorm && !color.hdr10);
    CHECK(selectColor(FRAMELAB_FORMAT_RGB10A2_UNORM, 0, color) == FRAMELAB_OK);
    CHECK(color.format == FRAMELAB_FORMAT_RGB10A2_UNORM && color.dxgi == dxgi::rgb10a2_unorm && !color.hdr10);
    CHECK(selectColor(FRAMELAB_FORMAT_RGB10A2_UNORM, flag::hdr10, color) == FRAMELAB_OK);
    CHECK(color.format == FRAMELAB_FORMAT_RGB10A2_UNORM && color.dxgi == dxgi::rgb10a2_unorm && color.hdr10);

    // A rejected choice leaves the previous one untouched.
    const Color before = color;
    const auto unchanged = [&] { return color.format == before.format && color.dxgi == before.dxgi && color.hdr10 == before.hdr10; };
    // FP16 is a color format that this FG profile does not support.
    CHECK(selectColor(FRAMELAB_FORMAT_RGBA16_FLOAT, 0, color) == FRAMELAB_UNSUPPORTED && unchanged());
    // HDR10 means PQ RGB10A2 content; with any other format it is an invalid combination.
    for (const uint32_t format : {0u, uint32_t(FRAMELAB_FORMAT_RGBA8_UNORM), uint32_t(FRAMELAB_FORMAT_BGRA8_UNORM),
                                  uint32_t(FRAMELAB_FORMAT_RGBA16_FLOAT)})
        CHECK(selectColor(format, flag::hdr10, color) == FRAMELAB_INVALID_ARGUMENT && unchanged());
    // Depth and motion formats are not color formats; 10 and above are unknown.
    for (const uint32_t format : {uint32_t(FRAMELAB_FORMAT_RG16_FLOAT), uint32_t(FRAMELAB_FORMAT_RG32_FLOAT),
                                  uint32_t(FRAMELAB_FORMAT_R32_FLOAT), uint32_t(FRAMELAB_FORMAT_D24S8),
                                  uint32_t(FRAMELAB_FORMAT_D32_FLOAT), 10u, 0xffffffffu})
        CHECK(selectColor(format, 0, color) == FRAMELAB_INVALID_ARGUMENT && unchanged());
    // Unknown flag bits are rejected for every format, including with HDR10.
    for (const uint32_t flags : {2u, 3u, 0x80000000u, 0xffffffffu}) {
        CHECK(selectColor(0, flags, color) == FRAMELAB_INVALID_ARGUMENT && unchanged());
        CHECK(selectColor(FRAMELAB_FORMAT_RGB10A2_UNORM, flags, color) == FRAMELAB_INVALID_ARGUMENT && unchanged());
    }

    // Device format support: load, sample, 2D texture and typed UAV store are all required.
    constexpr uint32_t all1 = support1::texture2d | support1::shader_load | support1::shader_sample |
        support1::typed_unordered_access_view;
    CHECK(deviceSupportsColor(all1, support2::uav_typed_store));
    CHECK(deviceSupportsColor(0xffffffffu, 0xffffffffu));
    CHECK(!deviceSupportsColor(all1, 0));
    CHECK(!deviceSupportsColor(0, support2::uav_typed_store));
    for (const uint32_t missing : {support1::texture2d, support1::shader_load, support1::shader_sample,
                                   support1::typed_unordered_access_view})
        CHECK(!deviceSupportsColor(all1 & ~missing, support2::uav_typed_store));

    std::printf("PASS D3D12 FG contract checks=%u\n", checks);
    return 0;
}
