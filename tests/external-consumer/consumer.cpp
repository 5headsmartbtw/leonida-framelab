// SPDX-License-Identifier: MIT
#include <framelab/framelab.h>

#include <memory>
#include <string_view>
#include <type_traits>

static_assert(std::is_standard_layout_v<FrameLabFgFrame>);
static_assert(std::is_trivially_copyable_v<FrameLabFgFrame>);

int main()
{
    FrameLabFgStream* raw{};
    if (framelab_fg_stream_create(framelab_abi_version(), &raw) != FRAMELAB_OK || !raw)
        return 1;
    const std::unique_ptr<FrameLabFgStream, decltype(&framelab_fg_stream_destroy)>
        stream(raw, &framelab_fg_stream_destroy);
    FrameLabFgFrame bad{};
    bad.struct_size = sizeof bad;
    bad.abi_version = FRAMELAB_ABI_VERSION;
    const auto result = framelab_fg_prepare(stream.get(), &bad);
    return result == FRAMELAB_INVALID_IDENTITY &&
        !std::string_view(framelab_status_string(result)).empty() ? 0 : 2;
}
