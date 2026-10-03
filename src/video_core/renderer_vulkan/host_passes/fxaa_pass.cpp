//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/fxaa_comp.h"

#include "video_core/renderer_vulkan/host_passes/fxaa_pass.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

namespace {
struct FxaaConstants {
    float inv_size[2];
    u32 linear_input;
};
constexpr u32 GroupSize = 8;
} // namespace

void FxaaPass::Create(vk::Device device, VmaAllocator allocator, u32 num_images) {
    this->device = device;
    this->num_images = num_images;

    sampler = Check<"create fxaa sampler">(device.createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxAnisotropy = 1.0f,
        .minLod = -1000.0f,
        .maxLod = 1000.0f,
    }));

    const std::array<vk::DescriptorSetLayoutBinding, 3> layout_bindings{{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .pImmutableSamplers = &sampler.get(),
        },
    }};

    descriptor_set_layout =
        Check<"create fxaa descriptor set layout">(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
            .bindingCount = layout_bindings.size(),
            .pBindings = layout_bindings.data(),
        }));

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(FxaaConstants),
    };

    pipeline_layout = Check<"fxaa pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout.get(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    }));
    SetObjectName(device, pipeline_layout.get(), "fxaa pipeline layout");

    const auto& module = CompileSPV(FXAA_COMP, device);
    ASSERT(module);
    SetObjectName(device, module, "fxaa.comp");
    const vk::ComputePipelineCreateInfo pinfo{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = module,
            .pName = "main",
        },
        .layout = pipeline_layout.get(),
    };
    pipeline = Check<"fxaa compute pipeline">(device.createComputePipelineUnique({}, pinfo));
    SetObjectName(device, pipeline.get(), "fxaa pipeline");
    device.destroyShaderModule(module);

    available_imgs.resize(num_images);
    for (u32 i = 0; i < num_images; ++i) {
        auto& img = available_imgs[i];
        img.id = static_cast<u8>(i);
        img.image = VideoCore::UniqueImage(device, allocator);
    }
}

vk::ImageView FxaaPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView input,
                               vk::Extent2D input_size, bool enable, bool linear_input) {
    if (!enable) {
        return input;
    }

    if (input_size != cur_size) {
        cur_size = input_size;
        for (auto& img : available_imgs) {
            img.dirty = true;
        }
    }

    auto& img = available_imgs[cur_image];
    if (++cur_image >= available_imgs.size()) {
        cur_image = 0;
    }
    if (img.dirty) {
        CreateImage(img);
    }

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Host/FXAA",
        });
    }

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const vk::ImageMemoryBarrier2 enter_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = img.image,
        .subresourceRange = simple_subresource,
    };
    cmdbuf.pipelineBarrier2({
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &enter_barrier,
    });

    const std::array<vk::DescriptorImageInfo, 3> img_info{{
        {
            .imageView = input,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        {
            .imageView = img.image_view.get(),
            .imageLayout = vk::ImageLayout::eGeneral,
        },
        {
            .sampler = sampler.get(),
        },
    }};
    const std::array<vk::WriteDescriptorSet, 3> set_writes{{
        {
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .pImageInfo = &img_info[0],
        },
        {
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .pImageInfo = &img_info[1],
        },
        {
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampler,
            .pImageInfo = &img_info[2],
        },
    }};

    const FxaaConstants consts{
        .inv_size = {1.0f / static_cast<float>(input_size.width),
                     1.0f / static_cast<float>(input_size.height)},
        .linear_input = linear_input ? 1u : 0u,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.get());
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0,
                                set_writes);
    cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(FxaaConstants), &consts);
    cmdbuf.dispatch((input_size.width + GroupSize - 1) / GroupSize,
                    (input_size.height + GroupSize - 1) / GroupSize, 1);

    const vk::ImageMemoryBarrier2 return_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = img.image,
        .subresourceRange = simple_subresource,
    };
    cmdbuf.pipelineBarrier2({
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &return_barrier,
    });

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    return img.image_view.get();
}

void FxaaPass::CreateImage(Img& img) const {
    img.dirty = false;
    img.image_view.reset();
    img.image.Destroy();

    const vk::ImageCreateInfo image_create_info{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent{
            .width = cur_size.width,
            .height = cur_size.height,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    img.image.Create(image_create_info);
    SetObjectName(device, static_cast<vk::Image>(img.image), "FXAA Output Image #{}", img.id);

    img.image_view = Check<"create fxaa image view">(device.createImageViewUnique({
        .image = img.image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    }));
    SetObjectName(device, img.image_view.get(), "FXAA Output ImageView #{}", img.id);
}

} // namespace Vulkan::HostPasses
