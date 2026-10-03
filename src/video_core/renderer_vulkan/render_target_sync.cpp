// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/render_target_sync.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

RenderTargetSync::RenderTargetSync(Scheduler& scheduler_, Runtime& runtime_,
                                   VideoCore::TextureCache& texture_cache_)
    : scheduler{scheduler_}, runtime{runtime_}, texture_cache{texture_cache_} {}

void RenderTargetSync::RecordRtWrite(VAddr addr, VideoCore::ImageId id) {
    rt_writes[addr] = id;
    // New contents at this address: textures have to pull again.
    copied.erase(addr);
}

void RenderTargetSync::CopyFromLastRt(VAddr addr, VideoCore::ImageId tex_id) {
    const auto it = rt_writes.find(addr);
    if (it == rt_writes.end() || it->second == tex_id) {
        return;
    }
    auto& rt = texture_cache.GetImage(it->second);
    auto& tex = texture_cache.GetImage(tex_id);

    // vkCmdCopyImage needs matching sample counts and texel sizes, and must stay in bounds.
    if (rt.info.props.is_depth || tex.info.props.is_depth ||
        rt.info.num_samples != tex.info.num_samples || rt.info.num_bits != tex.info.num_bits ||
        rt.info.props.is_block || tex.info.props.is_block ||
        rt.info.size.width < tex.info.size.width || rt.info.size.height < tex.info.size.height) {
        return;
    }
    if (!copied[addr].insert(tex_id).second) {
        return;
    }

    scheduler.EndRendering();
    runtime.Transit(&rt, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                    vk::AccessFlagBits2::eTransferRead);
    runtime.Transit(&tex, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eCopy,
                    vk::AccessFlagBits2::eTransferWrite);
    runtime.FlushBarriers();

    const vk::ImageCopy region = {
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {tex.info.size.width, tex.info.size.height, 1},
    };
    scheduler.CommandBuffer().copyImage(rt.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                        tex.GetImage(), vk::ImageLayout::eTransferDstOptimal,
                                        region);
    // The texture now holds GPU data newer than guest memory.
    tex.flags |= VideoCore::ImageFlagBits::GpuModified;
}

void RenderTargetSync::ClearRecords() {
    rt_writes.clear();
    copied.clear();
}

} // namespace Vulkan
