// SPDX-License-Identifier: MIT
#pragma once

#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <string_view>

namespace frame_inputs {

// Adjacent samples let the verifier compare an output to both neighboring source images.
inline uint32_t diagnosticReadbackInterval()
{
    const char* value=std::getenv("FRAMELAB_FG_READBACK_EVERY");
    if(!value)return 1;
    const std::string_view text(value);
    uint32_t interval{};
    const auto result=std::from_chars(text.data(),text.data()+text.size(),interval);
    return result.ec==std::errc{}&&result.ptr==text.data()+text.size()&&interval>=3?interval:1;
}

}
