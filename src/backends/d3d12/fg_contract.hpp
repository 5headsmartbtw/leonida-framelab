// SPDX-License-Identifier: MIT
#pragma once
// D3D12 Frame Generation configuration rules without Windows or NGX headers. The native adapter
// reads the caller's descriptor, queries the device and calls NGX. Portable tests exercise these
// decisions.
#include <framelab/framelab.h>

#include <cstddef>
#include <cstdint>

namespace framelab::d3d12::fg {

// Numeric Windows SDK and public-header values. The native adapter static_asserts each one.
namespace dxgi {
constexpr uint32_t rgb10a2_unorm = 24, rgba8_unorm = 28, bgra8_unorm = 87;
}
namespace support1 {
constexpr uint32_t texture2d = 0x20, shader_load = 0x100, shader_sample = 0x200,
    typed_unordered_access_view = 0x2000000;
}
namespace support2 {
constexpr uint32_t uav_typed_store = 0x80;
}
namespace flag {
constexpr uint32_t hdr10 = 1, known = hdr10;
}

constexpr size_t roundUp(size_t size, size_t align) { return (size + align - 1) / align * align; }

// The published FrameLabD3D12FgConfig layouts. Each older layout ends where the next appended
// group starts, and its sizeof is that offset rounded up to the struct alignment.
struct ConfigLayout {
    size_t renderExtent; // offsetof(render_extent): the v0.4.0 fields end here.
    size_t colorFormat;  // offsetof(color_format): the render_extent layout ends here.
    size_t size;         // sizeof the current struct.
    size_t align;
};

// Bytes to copy from the caller's descriptor, or 0 for FRAMELAB_ABI_MISMATCH. Only the fields of
// the caller's layout are copied: never its tail padding, and nothing past struct_size.
constexpr size_t copiedBytes(const ConfigLayout& layout, uint32_t structSize)
{
    if (structSize == roundUp(layout.renderExtent, layout.align)) return layout.renderExtent;
    if (structSize == roundUp(layout.colorFormat, layout.align)) return layout.colorFormat;
    return structSize >= layout.size ? layout.size : 0;
}

// The color and generated-image format a backend is created with.
struct Color {
    uint32_t format = FRAMELAB_FORMAT_RGBA8_UNORM; // FRAMELAB_FORMAT_*
    uint32_t dxgi = dxgi::rgba8_unorm;
    bool hdr10 = false;
};

// Create-time choice from FrameLabD3D12FgConfig.color_format and flags. `out` changes only on
// FRAMELAB_OK.
//  - Unknown flag bits, a value that is not a color format, or HDR10 with any format other than
//    RGB10A2: FRAMELAB_INVALID_ARGUMENT.
//  - RGBA16_FLOAT (scRGB/FP16): FRAMELAB_UNSUPPORTED. FG has no FP16 profile; D3D12 SR keeps it.
constexpr FrameLabStatus selectColor(uint32_t format, uint32_t flags, Color& out)
{
    if (flags & ~flag::known) return FRAMELAB_INVALID_ARGUMENT;
    const bool hdr10 = (flags & flag::hdr10) != 0;
    Color color;
    switch (format) {
    case 0:
    case FRAMELAB_FORMAT_RGBA8_UNORM: color = {FRAMELAB_FORMAT_RGBA8_UNORM, dxgi::rgba8_unorm, false}; break;
    case FRAMELAB_FORMAT_BGRA8_UNORM: color = {FRAMELAB_FORMAT_BGRA8_UNORM, dxgi::bgra8_unorm, false}; break;
    case FRAMELAB_FORMAT_RGB10A2_UNORM: color = {FRAMELAB_FORMAT_RGB10A2_UNORM, dxgi::rgb10a2_unorm, hdr10}; break;
    case FRAMELAB_FORMAT_RGBA16_FLOAT: return hdr10 ? FRAMELAB_INVALID_ARGUMENT : FRAMELAB_UNSUPPORTED;
    default: return FRAMELAB_INVALID_ARGUMENT;
    }
    if (hdr10 && color.format != FRAMELAB_FORMAT_RGB10A2_UNORM) return FRAMELAB_INVALID_ARGUMENT;
    out = color;
    return FRAMELAB_OK;
}

// The device must load and sample the color input as a 2D texture and store the generated image
// through a typed UAV (D3D12_FEATURE_FORMAT_SUPPORT for the configured format).
constexpr bool deviceSupportsColor(uint32_t formatSupport1, uint32_t formatSupport2)
{
    constexpr uint32_t required1 = support1::texture2d | support1::shader_load | support1::shader_sample |
        support1::typed_unordered_access_view;
    return (formatSupport1 & required1) == required1 && (formatSupport2 & support2::uav_typed_store) != 0;
}
} // namespace framelab::d3d12::fg
