//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <imgui.h>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace Vulkan::HostPasses {

/// Frame generation with AMD FSR 3 (FidelityFX SDK): builds the frame halfway between the
/// previously presented frame and the current one, so twice as many frames reach the screen.
/// Games hand the emulator no motion vectors, so the interpolation leans on FSR's optical flow.
/// Needs amd_fidelityfx_vk.dll next to the executable; without it the pass reports itself as
/// unavailable and nothing changes.
class FrameGenPass {
public:
    FrameGenPass();
    ~FrameGenPass();

    void Create(const Instance& instance);

    /// Whether the FidelityFX library was found and the device can run it.
    bool IsAvailable() const;

    /// Records the generation of the frame between the last one given and `frame_image`, which
    /// is expected in the general layout and is left in it. Returns the texture holding the
    /// generated frame (shader read only once `cmdbuf` has run), or nothing when there is no
    /// frame to show: on the first frame, after a reset or a size change, or on failure.
    std::optional<ImTextureID> Render(vk::CommandBuffer cmdbuf, vk::Image frame_image,
                                      vk::Extent2D size, bool is_hdr, float frame_time_ms,
                                      bool reset);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan::HostPasses
