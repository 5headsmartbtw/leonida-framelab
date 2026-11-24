// SPDX-License-Identifier: MIT
#pragma once

#include <framelab/framelab.h>
#include "FrameInputs.hpp"

namespace framelab::detail {
// Borrowed until the prepared packet is cancelled or retired.
const frame_inputs::FrameInputs* preparedInputs(const FrameLabFgStream* stream) noexcept;
}
