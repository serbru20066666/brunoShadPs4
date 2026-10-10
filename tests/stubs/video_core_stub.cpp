// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/amdgpu/pixel_format.h"

namespace AmdGpu {

// The table of formats the host can show lives with the Vulkan renderer, which the tests do not
// build: for them every format a texture descriptor names is a known one.
bool IsKnownSurfaceFormat(DataFormat, NumberFormat) {
    return true;
}

} // namespace AmdGpu
