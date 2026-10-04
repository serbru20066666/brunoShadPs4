// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string_view>

#include "common/types.h"

namespace VideoCore {

/// Shows what the emulator is doing while a game starts, before its first frame. `total` is zero
/// when the amount of work is not known.
using LoadingCallback = std::function<void(std::string_view stage, u32 done, u32 total)>;

inline LoadingCallback loading_callback;

inline void ReportLoading(std::string_view stage, u32 done = 0, u32 total = 0) {
    if (loading_callback) {
        loading_callback(stage, done, total);
    }
}

} // namespace VideoCore
