// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <set>
#include <tsl/robin_map.h>

#include "common/types.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Runtime;
class Scheduler;

/// Keeps textures that alias a render target in sync with it on the GPU.
///
/// On PS4 a render target and a texture with another format at the same address share
/// memory, so a pass can render into one and the next pass sample the other. Here each
/// one is a separate VkImage, and the texture would only see stale guest memory. Before a
/// texture is bound, copy the last render target written at its address into it (once per
/// submit). The copy is recorded in the command buffer, so it costs no CPU/GPU wait.
///
/// Based on the RenderTargetSync of the DiegoLix fork (diegolix29/shadPS4).
class RenderTargetSync {
public:
    RenderTargetSync(Scheduler& scheduler, Runtime& runtime,
                     VideoCore::TextureCache& texture_cache);

    /// Records that a render target was bound at this address.
    void RecordRtWrite(VAddr addr, VideoCore::ImageId id);

    /// Copies the last render target at `addr` into `tex_id` if they are different images.
    void CopyFromLastRt(VAddr addr, VideoCore::ImageId tex_id);

    /// Forgets all records (called on each submit).
    void ClearRecords();

private:
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::TextureCache& texture_cache;

    tsl::robin_map<VAddr, VideoCore::ImageId> rt_writes;
    std::map<VAddr, std::set<VideoCore::ImageId>> copied;
};

} // namespace Vulkan
