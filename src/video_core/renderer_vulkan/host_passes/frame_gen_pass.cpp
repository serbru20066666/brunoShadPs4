//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/frame_gen_pass.h"

#ifdef _WIN32

#include <array>
#include <mutex>
#include <string>

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/vk/ffx_api_vk.h>

#include <windows.h>

namespace Vulkan::HostPasses {

namespace {

constexpr vk::ImageSubresourceRange ColorRange = {
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .levelCount = 1,
    .layerCount = 1,
};

constexpr vk::ImageSubresourceLayers ColorLayers = {
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .layerCount = 1,
};

/// The value the constant depth buffer is filled with. The emulator has no depth to give, and
/// a flat one tells the interpolation that nothing is hidden behind anything else.
constexpr float FlatDepth = 0.5f;

/// Frames the interpolation takes to start using optical flow after a reset. FSR's shader counts
/// ten; a couple more keep clear of the edge.
constexpr u32 SettleFrames = 12;

void Transition(vk::CommandBuffer cmdbuf, vk::Image image, vk::ImageLayout from,
                vk::ImageLayout to) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2({
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    });
}

FfxApiResource Resource(const VideoCore::UniqueImage& image, u32 state) {
    const VkImage handle = static_cast<VkImage>(image.image);
    const auto description = ffxApiGetImageResourceDescriptionVK(
        handle, static_cast<VkImageCreateInfo>(image.image_ci), 0);
    return ffxApiGetResourceVK(handle, description, state);
}

/// The library asks for some functions by the name of the extension that introduced them. The
/// emulator's device has them as core functions without enabling those extensions, where the
/// suffixed name resolves to nothing, so fall back to the core name.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
    const auto lookup = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    if (const auto function = lookup(device, name)) {
        return function;
    }
    std::string core_name{name};
    if (core_name.ends_with("KHR")) {
        core_name.resize(core_name.size() - 3);
        return lookup(device, core_name.c_str());
    }
    return nullptr;
}

void LogMessage(uint32_t type, const wchar_t* message) {
    std::string text;
    for (; message && *message; ++message) {
        text.push_back(*message < 0x80 ? static_cast<char>(*message) : '?');
    }
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        LOG_ERROR(Render_Vulkan, "FidelityFX: {}", text);
    } else {
        LOG_WARNING(Render_Vulkan, "FidelityFX: {}", text);
    }
}

} // namespace

struct FrameGenPass::Impl {
    explicit Impl(const Instance& instance_)
        : instance{instance_}, device{instance_.GetDevice()},
          input{device, instance_.GetAllocator()}, output{device, instance_.GetAllocator()},
          depth{device, instance_.GetAllocator()}, motion{device, instance_.GetAllocator()} {}

    ~Impl() {
        if (context) {
            WaitForGpu();
            destroy_context(&context, nullptr);
        }
        output_view.reset();
        if (library) {
            FreeLibrary(library);
        }
    }

    void Load() {
        const auto features = instance.GetPhysicalDevice().getFeatures();
        if (!features.shaderStorageImageReadWithoutFormat ||
            !features.shaderStorageImageWriteWithoutFormat) {
            LOG_WARNING(Render_Vulkan,
                        "Frame generation needs storage images without format, which this GPU "
                        "does not support");
            return;
        }
        library = LoadLibraryW(L"amd_fidelityfx_vk.dll");
        if (!library) {
            LOG_WARNING(Render_Vulkan,
                        "Frame generation is enabled but amd_fidelityfx_vk.dll was not found "
                        "next to the executable");
            return;
        }
        create_context =
            reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(library, "ffxCreateContext"));
        destroy_context =
            reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(library, "ffxDestroyContext"));
        configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(library, "ffxConfigure"));
        dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(library, "ffxDispatch"));
        available = create_context && destroy_context && configure && dispatch;
        if (!available) {
            LOG_ERROR(Render_Vulkan, "amd_fidelityfx_vk.dll does not export the FidelityFX API");
        }
    }

    /// Nothing recorded earlier may still be running when the context or its images go away.
    void WaitForGpu() {
        std::scoped_lock lock{Scheduler::submit_mutex};
        static_cast<void>(device.waitIdle());
    }

    bool Recreate(vk::Extent2D new_size, bool new_hdr) {
        WaitForGpu();
        if (context) {
            destroy_context(&context, nullptr);
            context = nullptr;
        }
        if (output_texture) {
            ImGui::Vulkan::RemoveTexture(output_texture);
            output_texture = {};
        }
        output_view.reset();
        input.Destroy();
        output.Destroy();
        depth.Destroy();
        motion.Destroy();

        size = new_size;
        is_hdr = new_hdr;
        // The motion vector and depth inputs carry no information, so they can be small.
        render_size = vk::Extent2D{std::max(size.width / 2, 64u), std::max(size.height / 2, 64u)};
        has_history = false;
        inputs_cleared = false;

        // Formats every device can write from a compute shader, whatever the swapchain uses.
        const vk::Format format =
            is_hdr ? vk::Format::eA2B10G10R10UnormPack32 : vk::Format::eR8G8B8A8Unorm;
        const auto make_info = [](vk::Format format, vk::Extent2D extent,
                                  vk::ImageUsageFlags usage) {
            return vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = {extent.width, extent.height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .usage = usage,
                .initialLayout = vk::ImageLayout::eUndefined,
            };
        };
        using Usage = vk::ImageUsageFlagBits;
        input.Create(
            make_info(format, size, Usage::eSampled | Usage::eTransferDst | Usage::eTransferSrc));
        output.Create(make_info(format, size,
                                Usage::eSampled | Usage::eStorage | Usage::eTransferDst |
                                    Usage::eTransferSrc));
        depth.Create(
            make_info(vk::Format::eR32Sfloat, render_size, Usage::eSampled | Usage::eTransferDst));
        motion.Create(make_info(vk::Format::eR16G16Sfloat, render_size,
                                Usage::eSampled | Usage::eTransferDst));
        SetObjectName(device, input.image, "Frame generation input");
        SetObjectName(device, output.image, "Frame generation output");
        SetObjectName(device, depth.image, "Frame generation flat depth");
        SetObjectName(device, motion.image, "Frame generation zero motion");

        output_view = Check<"create frame generation view">(device.createImageViewUnique({
            .image = output.image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            // The interpolation leaves its own data in the alpha channel; show the frame opaque.
            .components{.a = vk::ComponentSwizzle::eOne},
            .subresourceRange = ColorRange,
        }));
        output_texture =
            ImGui::Vulkan::AddTexture(*output_view, vk::ImageLayout::eShaderReadOnlyOptimal);

        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.vkDevice = static_cast<VkDevice>(device);
        backend.vkPhysicalDevice = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice());
        backend.vkDeviceProcAddr = &GetDeviceProcAddr;

        ffxCreateContextDescFrameGeneration description{};
        description.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        description.header.pNext = &backend.header;
        description.displaySize = {size.width, size.height};
        description.maxRenderSize = {render_size.width, render_size.height};
        description.backBufferFormat = ffxApiGetSurfaceFormatVK(static_cast<VkFormat>(format));
        if (const auto result = create_context(&context, &description.header, nullptr);
            result != FFX_API_RETURN_OK) {
            LOG_ERROR(Render_Vulkan, "Creating the frame generation context failed with {}",
                      result);
            context = nullptr;
            return false;
        }

        ffxConfigureDescGlobalDebug1 debug{};
        debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
        debug.fpMessage = &LogMessage;
        debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
        configure(&context, &debug.header);

        LOG_INFO(Render_Vulkan, "Frame generation ready at {}x{}{}", size.width, size.height,
                 is_hdr ? " (HDR)" : "");
        return true;
    }

    std::optional<ImTextureID> Render(vk::CommandBuffer cmdbuf, vk::Image frame_image,
                                      vk::Extent2D frame_size, bool frame_hdr, float frame_time_ms,
                                      bool reset) {
        if (!available) {
            return std::nullopt;
        }
        if (!context || frame_size != size || frame_hdr != is_hdr) {
            if (!Recreate(frame_size, frame_hdr)) {
                available = false;
                return std::nullopt;
            }
        }

        using Layout = vk::ImageLayout;
        if (!inputs_cleared) {
            // No game hands over motion vectors or depth: motion is all zero and depth is flat,
            // which leaves the interpolation to its optical flow.
            Transition(cmdbuf, depth.image, Layout::eUndefined, Layout::eTransferDstOptimal);
            Transition(cmdbuf, motion.image, Layout::eUndefined, Layout::eTransferDstOptimal);
            cmdbuf.clearColorImage(
                depth.image, Layout::eTransferDstOptimal,
                vk::ClearColorValue{.float32 = std::array{FlatDepth, 0.0f, 0.0f, 0.0f}},
                ColorRange);
            cmdbuf.clearColorImage(
                motion.image, Layout::eTransferDstOptimal,
                vk::ClearColorValue{.float32 = std::array{0.0f, 0.0f, 0.0f, 0.0f}}, ColorRange);
            Transition(cmdbuf, depth.image, Layout::eTransferDstOptimal,
                       Layout::eShaderReadOnlyOptimal);
            Transition(cmdbuf, motion.image, Layout::eTransferDstOptimal,
                       Layout::eShaderReadOnlyOptimal);
            inputs_cleared = true;
        }

        // Copy the frame into an image of our own, converting the channel order when needed.
        const vk::Offset3D corner{static_cast<s32>(size.width), static_cast<s32>(size.height), 1};
        const vk::ImageBlit blit{
            .srcSubresource = ColorLayers,
            .srcOffsets = std::array{vk::Offset3D{}, corner},
            .dstSubresource = ColorLayers,
            .dstOffsets = std::array{vk::Offset3D{}, corner},
        };
        Transition(cmdbuf, input.image, Layout::eUndefined, Layout::eTransferDstOptimal);
        cmdbuf.blitImage(frame_image, Layout::eGeneral, input.image, Layout::eTransferDstOptimal,
                         blit, vk::Filter::eNearest);
        Transition(cmdbuf, input.image, Layout::eTransferDstOptimal,
                   Layout::eShaderReadOnlyOptimal);
        Transition(cmdbuf, output.image, Layout::eUndefined, Layout::eGeneral);

        const FfxApiRect2D whole_frame{0, 0, static_cast<s32>(size.width),
                                       static_cast<s32>(size.height)};
        const auto command_list = static_cast<VkCommandBuffer>(cmdbuf);

        ffxConfigureDescFrameGeneration config{};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.frameGenerationEnabled = true;
        // The emulator paces and presents the frames itself; only the interpolation is used.
        config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        config.generationRect = whole_frame;
        config.frameID = frame_id;

        ffxDispatchDescFrameGenerationPrepareCameraInfo camera{};
        camera.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
        camera.cameraRight[0] = 1.0f;
        camera.cameraUp[1] = 1.0f;
        camera.cameraForward[2] = 1.0f;

        ffxDispatchDescFrameGenerationPrepare prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        prepare.header.pNext = &camera.header;
        prepare.frameID = frame_id;
        prepare.commandList = command_list;
        prepare.renderSize = {render_size.width, render_size.height};
        prepare.motionVectorScale = {1.0f, 1.0f};
        prepare.frameTimeDelta = frame_time_ms;
        prepare.cameraNear = 0.1f;
        prepare.cameraFar = 1000.0f;
        prepare.cameraFovAngleVertical = 1.0f;
        prepare.viewSpaceToMetersFactor = 1.0f;
        prepare.depth = Resource(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prepare.motionVectors = Resource(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);

        ffxDispatchDescFrameGeneration generate{};
        generate.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        generate.commandList = command_list;
        generate.presentColor = Resource(input, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        generate.outputs[0] = Resource(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        generate.numGeneratedFrames = 1;
        generate.reset = reset || !has_history;
        generate.backbufferTransferFunction = is_hdr ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ
                                                     : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        generate.minMaxLuminance[0] = 0.0f;
        generate.minMaxLuminance[1] = 1000.0f;
        generate.generationRect = whole_frame;
        generate.frameID = frame_id;

        const auto run = [this](const char* step, ffxReturnCode_t result) {
            if (result == FFX_API_RETURN_OK) {
                return true;
            }
            LOG_ERROR(Render_Vulkan, "Frame generation failed to {} with {}, turning it off", step,
                      result);
            available = false;
            return false;
        };
        const bool done = run("configure", configure(&context, &config.header)) &&
                          run("prepare", dispatch(&context, &prepare.header)) &&
                          run("generate", dispatch(&context, &generate.header));
        Transition(cmdbuf, output.image, Layout::eGeneral, Layout::eShaderReadOnlyOptimal);
        ++frame_id;

        // For its first frames after a reset the interpolation ignores optical flow and blends
        // the two frames in place, which shows as a translucent ghost of the previous one. Those
        // are not worth showing: the rendered frames go out alone until it has settled.
        frames_since_reset = generate.reset ? 0 : frames_since_reset + 1;
        const bool generated = done && frames_since_reset >= SettleFrames;
        has_history = done;
        if (!generated) {
            return std::nullopt;
        }
        return output_texture;
    }

    const Instance& instance;
    vk::Device device;

    HMODULE library{};
    PfnFfxCreateContext create_context{};
    PfnFfxDestroyContext destroy_context{};
    PfnFfxConfigure configure{};
    PfnFfxDispatch dispatch{};
    bool available{};

    ffxContext context{};
    vk::Extent2D size{};
    vk::Extent2D render_size{};
    bool is_hdr{};
    bool has_history{};
    u32 frames_since_reset{};
    bool inputs_cleared{};
    u64 frame_id{};

    VideoCore::UniqueImage input;
    VideoCore::UniqueImage output;
    VideoCore::UniqueImage depth;
    VideoCore::UniqueImage motion;
    vk::UniqueImageView output_view;
    ImTextureID output_texture{};
};

FrameGenPass::FrameGenPass() = default;

FrameGenPass::~FrameGenPass() = default;

void FrameGenPass::Create(const Instance& instance) {
    if (!EmulatorSettings.IsFrameGenerationEnabled()) {
        return;
    }
    impl = std::make_unique<Impl>(instance);
    impl->Load();
}

bool FrameGenPass::IsAvailable() const {
    return impl && impl->available;
}

std::optional<ImTextureID> FrameGenPass::Render(vk::CommandBuffer cmdbuf, vk::Image frame_image,
                                                vk::Extent2D size, bool is_hdr, float frame_time_ms,
                                                bool reset) {
    if (!impl) {
        return std::nullopt;
    }
    return impl->Render(cmdbuf, frame_image, size, is_hdr, frame_time_ms, reset);
}

} // namespace Vulkan::HostPasses

#else

namespace Vulkan::HostPasses {

struct FrameGenPass::Impl {};

FrameGenPass::FrameGenPass() = default;

FrameGenPass::~FrameGenPass() = default;

void FrameGenPass::Create(const Instance&) {}

bool FrameGenPass::IsAvailable() const {
    return false;
}

std::optional<ImTextureID> FrameGenPass::Render(vk::CommandBuffer, vk::Image, vk::Extent2D, bool,
                                                float, bool) {
    return std::nullopt;
}

} // namespace Vulkan::HostPasses

#endif
