// SPDX-License-Identifier: MIT
#pragma once

#include <framelab/framelab.h>
#include "FrameInputs.hpp"

namespace framelab::detail {
// Borrowed until the prepared packet is cancelled or retired.
std::shared_ptr<const void> retainLease(const FrameLabLease* lease) noexcept;
const frame_inputs::FrameInputs* preparedInputs(const FrameLabFgStream* stream) noexcept;
}
