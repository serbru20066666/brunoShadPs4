//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::HostPasses {

/// Edge anti-aliasing on the guest frame at its own resolution, before FSR or the final
/// scaling. Useful when a game's own MSAA can't be used on the host.
class FxaaPass {
public:
    void Create(vk::Device device, VmaAllocator allocator, u32 num_images);

    /// Returns the anti-aliased view (shader read only layout), or `input` when disabled.
    /// `linear_input` tells the shader the view decodes sRGB, so luma is estimated perceptually.
    vk::ImageView Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                         bool enable, bool linear_input);

private:
    struct Img {
        u8 id{};
        bool dirty{true};
        VideoCore::UniqueImage image;
        vk::UniqueImageView image_view;
    };

    void CreateImage(Img& img) const;

    vk::Device device{};
    u32 num_images{};

    vk::UniqueDescriptorSetLayout descriptor_set_layout{};
    vk::UniqueSampler sampler{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniquePipeline pipeline{};

    vk::Extent2D cur_size{};
    u32 cur_image{};
    std::vector<Img> available_imgs;
};

} // namespace Vulkan::HostPasses
