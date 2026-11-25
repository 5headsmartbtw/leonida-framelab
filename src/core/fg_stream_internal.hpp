// SPDX-License-Identifier: MIT
#pragma once

#include <framelab/framelab.h>
#include "FrameInputs.hpp"

#include <memory>

namespace framelab::detail {
std::shared_ptr<const void> retainLease(const FrameLabLease* lease) noexcept;
// Borrowed until the prepared packet is cancelled or retired.
const frame_inputs::FrameInputs* preparedInputs(const FrameLabFgStream* stream) noexcept;
}
