#include "engine/render/renderer.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "engine/core/log.h"
#include "engine/render/upscale/streamline.h"
#include "engine/render/upscale/streamline.h"
#include "engine/render/upscale/upscale.h"

namespace pt {
namespace {

VkFormat g_imgui_format = VK_FORMAT_UNDEFINED;

void ImGuiCheck(VkResult result) {
    if (result != VK_SUCCESS) {
        LogError("imgui vulkan: {}", vk::ResultName(result));
    }
}

}

bool Renderer::Init(SDL_Window* window, const RendererSettings& settings) {
    settings_ = settings;
    if (const char* brightness = std::getenv("PT_OUTPUT_BRIGHTNESS")) {
        brightness_override_ = static_cast<float>(std::atof(brightness));
    }
    window_ = settings.headless ? nullptr : window;
    if (!ctx_.Init(window_, settings.validation)) {
        return false;
    }
    uint32_t width = settings.width;
    uint32_t height = settings.height;
    VkFormat output_format = VK_FORMAT_R8G8B8A8_UNORM;
    if (window_) {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        width = static_cast<uint32_t>(w);
        height = static_cast<uint32_t>(h);
        if (!ctx_.CreateSwapchain(width, height, settings.vsync)) {
            /* DLSS-G's swapchain hook can refuse the window (borderless fullscreen on some setups); the game goes on without it. */
            if (!streamline::UnloadFrameGen()) {
                return false;
            }
            LogError("frame generation: DLSS Frame Generation refused the swapchain, turned off for this run");
            UpscaleHost::Get().SetDlssFrameGenFailed(true);
            if (!ctx_.CreateSwapchain(width, height, settings.vsync)) {
                return false;
            }
        }
        output_format = ctx_.swapchain.format;
        width = ctx_.swapchain.extent.width;
        height = ctx_.swapchain.extent.height;
    }
    output_format_ = output_format;
    for (Frame& frame : frames_) {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = ctx_.queue_family;
        vkCreateCommandPool(ctx_.device, &pool_info, nullptr, &frame.pool);
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = frame.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(ctx_.device, &alloc, &frame.cmd);
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(ctx_.device, &semaphore_info, nullptr, &frame.image_available);
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(ctx_.device, &fence_info, nullptr, &frame.in_flight);
    }
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx_.device, &sampler_info, nullptr, &linear_sampler_);
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(ctx_.device, &sampler_info, nullptr, &wrap_sampler_);
    if (!CreateTargets(width, height) || !CreateCompositePipeline(output_format)) {
        return false;
    }
    if (window_ && !InitImGui(window_)) {
        return false;
    }
    return true;
}

bool Renderer::InitImGui(SDL_Window* window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "pt_imgui.ini";
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForVulkan(window);
    g_imgui_format = ctx_.swapchain.format;
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = ctx_.instance;
    info.PhysicalDevice = ctx_.physical;
    info.Device = ctx_.device;
    info.QueueFamily = ctx_.queue_family;
    info.Queue = ctx_.queue;
    info.DescriptorPoolSize = 256;
    info.MinImageCount = ctx_.swapchain.min_image_count;
    info.ImageCount = static_cast<uint32_t>(ctx_.swapchain.images.size());
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_imgui_format;
    info.CheckVkResultFn = ImGuiCheck;
    if (!ImGui_ImplVulkan_Init(&info)) {
        LogError("ImGui_ImplVulkan_Init failed");
        return false;
    }
    imgui_ready_ = true;
    return true;
}

bool Renderer::CreateTargets(uint32_t width, uint32_t height) {
    const VkExtent3D extent{width, height, 1};
    if (!ctx_.CreateImage(scene_color_, kSceneColorFormat, extent,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }
    if (settings_.headless &&
        !ctx_.CreateImage(output_, VK_FORMAT_R8G8B8A8_UNORM, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }
    output_ready_ = false;
    if (!settings_.headless) {
        const VkFormat format = ctx_.swapchain.format;
        const bool capture = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_UNORM ||
                             format == VK_FORMAT_R8G8B8A8_SRGB;
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!capture) {
            LogWarn("renderer: swapchain format {} has no 8-bit layout, windowed screenshots disabled", static_cast<int>(format));
        } else if (!ctx_.CreateImage(output_, format, extent, usage)) {
            LogWarn("renderer: no capture image, windowed screenshots disabled");
        }
    }
    if (!ctx_.CreateImage(final_, output_format_, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
        return false;
    }
    WriteCompositeSets();
    return true;
}

void Renderer::WriteCompositeSets() {
    if (!composite_set_ || !final_set_) {
        return;
    }
    const VkImageView noise = grain_noise_ ? grain_noise_ : scene_color_.view;
    VkDescriptorImageInfo infos[4] = {{linear_sampler_, scene_color_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {linear_sampler_, final_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {wrap_sampler_, noise, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {wrap_sampler_, noise, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[4]{};
    for (int i = 0; i < 4; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = i % 2 == 0 ? composite_set_ : final_set_;
        writes[i].dstBinding = i < 2 ? 0 : 1;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(ctx_.device, 4, writes, 0, nullptr);
}

void Renderer::SetGrainNoise(VkImageView view) {
    if (grain_noise_ == view) {
        return;
    }
    vkDeviceWaitIdle(ctx_.device);
    grain_noise_ = view;
    WriteCompositeSets();
}

void Renderer::DestroyTargets() {
    ctx_.DestroyImage(scene_color_);
    ctx_.DestroyImage(final_);
    ctx_.DestroyImage(output_);
}

bool Renderer::CreateCompositePipeline(VkFormat output_format) {
    VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                                {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 2;
    set_info.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx_.device, &set_info, nullptr, &composite_set_layout_);
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    vkCreateDescriptorPool(ctx_.device, &pool_info, nullptr, &composite_pool_);
    const VkDescriptorSetLayout layouts[2] = {composite_set_layout_, composite_set_layout_};
    VkDescriptorSet sets[2] = {};
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = composite_pool_;
    alloc.descriptorSetCount = 2;
    alloc.pSetLayouts = layouts;
    vkAllocateDescriptorSets(ctx_.device, &alloc, sets);
    composite_set_ = sets[0];
    final_set_ = sets[1];
    WriteCompositeSets();

    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &composite_set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    vkCreatePipelineLayout(ctx_.device, &layout_info, nullptr, &composite_layout_);

    VkShaderModule vert = vk::LoadShaderModule(ctx_.device, "fullscreen.vert");
    VkShaderModule frag = vk::LoadShaderModule(ctx_.device, "composite.frag");
    if (!vert || !frag) {
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &output_format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = composite_layout_;
    const bool ok = vk::Check(vkCreateGraphicsPipelines(ctx_.device, VK_NULL_HANDLE, 1, &info, nullptr, &composite_pipeline_), "composite pipeline");
    vkDestroyShaderModule(ctx_.device, vert, nullptr);
    vkDestroyShaderModule(ctx_.device, frag, nullptr);
    return ok;
}

void Renderer::Shutdown() {
    if (!ctx_.device) {
        return;
    }
    vkDeviceWaitIdle(ctx_.device);
    if (imgui_ready_) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }
    DestroyXr();
    vkDestroyPipeline(ctx_.device, composite_pipeline_, nullptr);
    vkDestroyPipelineLayout(ctx_.device, composite_layout_, nullptr);
    vkDestroyDescriptorPool(ctx_.device, composite_pool_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_.device, composite_set_layout_, nullptr);
    vkDestroySampler(ctx_.device, linear_sampler_, nullptr);
    vkDestroySampler(ctx_.device, wrap_sampler_, nullptr);
    DestroyTargets();
    for (Frame& frame : frames_) {
        vkDestroyFence(ctx_.device, frame.in_flight, nullptr);
        vkDestroySemaphore(ctx_.device, frame.image_available, nullptr);
        vkDestroyCommandPool(ctx_.device, frame.pool, nullptr);
    }
    ctx_.Shutdown();
}

void Renderer::SetVsync(bool enabled) {
    if(settings_.vsync == enabled) return;
    settings_.vsync = enabled;
    if(window_) swapchain_dirty_ = true;
    LogInfo("display: VSync {} (applied at next frame)", enabled ? "on" : "off");
}

void Renderer::Resize(uint32_t, uint32_t) {
    swapchain_dirty_ = true;
}

bool Renderer::BeginFrame(bool present) {
    Frame& frame = frames_[frame_index_];
    streamline::BeginFrame();
    ctx_.CheckDeviceLost(vkWaitForFences(ctx_.device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX), "frame fence wait");
    grain[0] = 0.0f;
    if (render_extent_.width > 0 && render_extent_.height > 0 &&
        (scene_color_.extent.width != render_extent_.width || scene_color_.extent.height != render_extent_.height)) {
        vkDeviceWaitIdle(ctx_.device);
        DestroyTargets();
        if (!CreateTargets(render_extent_.width, render_extent_.height)) {
            return false;
        }
        LogInfo("renderer: render size {}x{} (VR)", render_extent_.width, render_extent_.height);
    }
    presenting_ = window_ && present;
    if (presenting_) {
        if (swapchain_dirty_) {
            int w = 0;
            int h = 0;
            SDL_GetWindowSizeInPixels(window_, &w, &h);
            if (w == 0 || h == 0) {
                return false;
            }
            if (!ctx_.CreateSwapchain(static_cast<uint32_t>(w), static_cast<uint32_t>(h), settings_.vsync)) {
                return false;
            }
            DestroyTargets();
            if (render_extent_.width > 0 && render_extent_.height > 0) {
                CreateTargets(render_extent_.width, render_extent_.height);
            } else {
                CreateTargets(ctx_.swapchain.extent.width, ctx_.swapchain.extent.height);
            }
            if (imgui_ready_) {
                ImGui_ImplVulkan_SetMinImageCount(ctx_.swapchain.min_image_count);
            }
            swapchain_dirty_ = false;
        }
        const VkResult acquire = ctx_.AcquireNextImage(frame.image_available, &image_index_);
        ctx_.CheckDeviceLost(acquire, "swapchain acquire");
        if (acquire == VK_NOT_READY || acquire == VK_TIMEOUT) {
            return false;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            swapchain_dirty_ = true;
            return false;
        }
        if (acquire == VK_SUBOPTIMAL_KHR) {
            swapchain_dirty_ = true;
        }
    }
    vkResetFences(ctx_.device, 1, &frame.in_flight);
    vkResetCommandPool(ctx_.device, frame.pool, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.cmd, &begin);

    vk::ImageBarrier(frame.cmd, scene_color_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = scene_color_.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, RenderExtent()};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(frame.cmd, &rendering);
    vkCmdEndRendering(frame.cmd);
    streamline::SetMarker(streamline::Marker::SimulationEnd);
    streamline::SetMarker(streamline::Marker::RenderSubmitStart);
    return true;
}

void Renderer::Composite(VkCommandBuffer cmd, VkDescriptorSet set, float mode, VkExtent2D extent, VkOffset2D offset) {
    VkViewport viewport{static_cast<float>(offset.x), static_cast<float>(offset.y), static_cast<float>(extent.width), static_cast<float>(extent.height),
                        0.0f, 1.0f};
    VkRect2D scissor{offset, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_layout_, 0, 1, &set, 0, nullptr);
    const float brightness = std::clamp(brightness_override_ > 0.0f ? brightness_override_ : output_brightness, 0.1f, 4.0f);
    /* The grain tiles three times across the original 16:9 frame; scaling by the window's width in 16:9 frames keeps the grain texel size on ultrawide. */
    const float across = extent.height ? (static_cast<float>(extent.width) / static_cast<float>(extent.height)) / (16.0f / 9.0f) : 1.0f;
    const float push[16] = {exposure, brightness, mode, 0.0f, fade[0], fade[1], fade[2], fade[3],
                            grain[0], grain[1], grain[2], grain[3], grain_offset[0], grain_offset[1], across, 0.0f};
    vkCmdPushConstants(cmd, composite_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void Renderer::EndFrame(bool draw_ui) {
    Frame& frame = frames_[frame_index_];
    VkCommandBuffer cmd = frame.cmd;
    vk::ImageBarrier(cmd, scene_color_.image, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkImage target_image = window_ ? (presenting_ ? ctx_.swapchain.images[image_index_] : VK_NULL_HANDLE) : output_.image;
    VkImageView target_view = window_ ? (presenting_ ? ctx_.swapchain.views[image_index_] : VK_NULL_HANDLE) : output_.view;
    const VkExtent2D extent = RenderExtent();
    const bool xr = std::exchange(xr_pending_, false);
    vk::ImageBarrier(cmd, final_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = final_.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    const vk::Image* hud = presenting_ && hudless && !xr ? hudless(image_index_) : nullptr;
    vkCmdBeginRendering(cmd, &rendering);
    Composite(cmd, composite_set_, 0.0f, extent);
    if (overlay && !hud && (!xr || xr_frame_.overlay_on_frame)) {
        overlay(cmd, final_.view, extent);
    }
    vkCmdEndRendering(cmd);
    vk::ImageBarrier(cmd, final_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (hud) {
        vk::ImageBarrier(cmd, hud->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        color.imageView = hud->view;
        vkCmdBeginRendering(cmd, &rendering);
        Composite(cmd, final_set_, 1.0f, extent);
        vkCmdEndRendering(cmd);
        vk::ImageBarrier(cmd, hud->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (overlay) {
            vk::ImageBarrier(cmd, final_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            color.imageView = final_.view;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            vkCmdBeginRendering(cmd, &rendering);
            overlay(cmd, final_.view, extent);
            vkCmdEndRendering(cmd);
            color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            vk::ImageBarrier(cmd, final_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    if (xr) {
        RecordXr(cmd);
    }
    const VkExtent2D window_extent = presenting_ ? ctx_.swapchain.extent : extent;
    const bool fitted = window_extent.width != extent.width || window_extent.height != extent.height;
    if (target_image) {
        vk::ImageBarrier(cmd, target_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        color.imageView = target_view;
        if (fitted) {
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            rendering.renderArea = {{0, 0}, window_extent};
        }
        vkCmdBeginRendering(cmd, &rendering);
        if (fitted) {
            const float scale = std::min(static_cast<float>(window_extent.width) / static_cast<float>(extent.width),
                                         static_cast<float>(window_extent.height) / static_cast<float>(extent.height));
            const VkExtent2D shown{std::max(1u, static_cast<uint32_t>(extent.width * scale)), std::max(1u, static_cast<uint32_t>(extent.height * scale))};
            Composite(cmd, final_set_, 1.0f, shown,
                      {static_cast<int32_t>((window_extent.width - shown.width) / 2), static_cast<int32_t>((window_extent.height - shown.height) / 2)});
        } else {
            Composite(cmd, final_set_, 1.0f, extent);
        }
        if (draw_ui && imgui_ready_) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        }
        vkCmdEndRendering(cmd);
    }
    if (presenting_ && output_.image && !fitted) {
        vk::ImageBarrier(cmd, target_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk::ImageBarrier(cmd, output_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = {extent.width, extent.height, 1};
        vkCmdCopyImage(cmd, target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        vk::ImageBarrier(cmd, output_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk::ImageBarrier(cmd, target_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0, ctx_.PresentLayout());
        output_ready_ = true;
    } else if (presenting_) {
        vk::ImageBarrier(cmd, target_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                         ctx_.PresentLayout());
    } else if (!window_) {
        output_ready_ = true;
        vk::ImageBarrier(cmd, target_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = frame.image_available;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    if (presenting_) {
        signal.semaphore = ctx_.swapchain.render_finished[image_index_];
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
    }
    ctx_.CheckDeviceLost(vkQueueSubmit2(ctx_.queue, 1, &submit, frame.in_flight), "frame submit");
    streamline::SetMarker(streamline::Marker::RenderSubmitEnd);
    if (presenting_) {
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &ctx_.swapchain.render_finished[image_index_];
        present.swapchainCount = 1;
        present.pSwapchains = &ctx_.swapchain.handle;
        present.pImageIndices = &image_index_;
        streamline::SetMarker(streamline::Marker::PresentStart);
        const VkResult result = ctx_.QueuePresent(present);
        streamline::SetMarker(streamline::Marker::PresentEnd);
        ctx_.CheckDeviceLost(result, "present");
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            swapchain_dirty_ = true;
        }
    }
    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
}

bool Renderer::SaveScreenshot(const std::filesystem::path& path) {
    if (!output_.image || !output_ready_) {
        LogError("screenshot {} not written: {}", path.string(), output_.image ? "no frame rendered yet" : "no capture image");
        return false;
    }
    vkDeviceWaitIdle(ctx_.device);
    const uint32_t width = output_.extent.width;
    const uint32_t height = output_.extent.height;
    vk::Buffer readback;
    if (!ctx_.CreateBuffer(readback, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) {
        return false;
    }
    ctx_.Submit([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, output_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
    });
    vmaInvalidateAllocation(ctx_.allocator, readback.allocation, 0, VK_WHOLE_SIZE);
    uint8_t* pixels = static_cast<uint8_t*>(readback.mapped);
    const bool bgra = output_.format == VK_FORMAT_B8G8R8A8_UNORM || output_.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0, n = size_t(width) * height; i < n; ++i) {
        uint8_t* p = pixels + i * 4;
        if (bgra) {
            std::swap(p[0], p[2]);
        }
        p[3] = 255;
    }
    const int ok = stbi_write_png(path.string().c_str(), static_cast<int>(width), static_cast<int>(height), 4, pixels, static_cast<int>(width * 4));
    ctx_.DestroyBuffer(readback);
    if (ok) {
        LogInfo("screenshot written to {}", path.string());
    } else {
        LogError("screenshot {} write failed", path.string());
    }
    return ok != 0;
}

/* XR_KHR_vulkan_enable2 hands the swapchain images over in colour attachment layout and wants them back the same way, hence no transitions here. */
void Renderer::RecordXr(VkCommandBuffer cmd) {
    if (!xr_layout_) {
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &composite_set_layout_;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        if (!vk::Check(vkCreatePipelineLayout(ctx_.device, &layout_info, nullptr, &xr_layout_), "xr copy layout")) {
            return;
        }
    }
    if (xr_frame_.hud) {
        if (!hud_.image) {
            if (!ctx_.CreateImage(hud_, output_format_, {kHudExtent.width, kHudExtent.height, 1},
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
                return;
            }
            VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
            VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pool_info.maxSets = 1;
            pool_info.poolSizeCount = 1;
            pool_info.pPoolSizes = &pool_size;
            vkCreateDescriptorPool(ctx_.device, &pool_info, nullptr, &xr_pool_);
            VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            alloc.descriptorPool = xr_pool_;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &composite_set_layout_;
            vkAllocateDescriptorSets(ctx_.device, &alloc, &hud_set_);
            VkDescriptorImageInfo infos[2] = {{linear_sampler_, hud_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                              {linear_sampler_, hud_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkWriteDescriptorSet writes[2]{};
            for (uint32_t i = 0; i < 2; ++i) {
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = hud_set_;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &infos[i];
            }
            vkUpdateDescriptorSets(ctx_.device, 2, writes, 0, nullptr);
        }
        vk::ImageBarrier(cmd, hud_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = hud_.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, kHudExtent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &rendering);
        if (overlay) {
            overlay(cmd, hud_.view, kHudExtent);
        }
        vkCmdEndRendering(cmd);
        vk::ImageBarrier(cmd, hud_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CopyToXr(cmd, hud_set_, *xr_frame_.hud, true);
    }
    if (xr_frame_.eye) {
        CopyToXr(cmd, final_set_, *xr_frame_.eye, false);
    }
}

void Renderer::CopyToXr(VkCommandBuffer cmd, VkDescriptorSet set, const XrTarget& target, bool premultiplied) {
    const VkPipeline pipeline = XrPipeline(target.format);
    if (!pipeline || !target.image) {
        return;
    }
    vk::ImageBarrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, 1);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, target.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(target.extent.width), static_cast<float>(target.extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, target.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, xr_layout_, 0, 1, &set, 0, nullptr);
    const float brightness = std::clamp(brightness_override_ > 0.0f ? brightness_override_ : output_brightness, 0.1f, 4.0f);
    const float across = target.rect.w > 0.0f && target.extent.height
                             ? (static_cast<float>(target.extent.width) / static_cast<float>(target.extent.height)) / (16.0f / 9.0f)
                             : 1.0f;
    const float push[16] = {target.rect.x, target.rect.y, target.rect.z, target.rect.w,
                            premultiplied ? 1.0f : 0.0f, premultiplied ? 1.0f : brightness, 0.0f, 0.0f,
                            premultiplied ? 0.0f : grain[0], grain[1], grain[2], grain[3],
                            grain_offset[0], grain_offset[1], across, 0.0f};
    vkCmdPushConstants(cmd, xr_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

VkPipeline Renderer::XrPipeline(VkFormat format) {
    for (const auto& [f, pipeline] : xr_pipelines_) {
        if (f == format) {
            return pipeline;
        }
    }
    VkShaderModule vert = vk::LoadShaderModule(ctx_.device, "fullscreen.vert");
    VkShaderModule frag = vk::LoadShaderModule(ctx_.device, "xr_copy.frag");
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vert && frag) {
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = xr_layout_;
        if (!vk::Check(vkCreateGraphicsPipelines(ctx_.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "xr copy pipeline")) {
            pipeline = VK_NULL_HANDLE;
        }
    }
    if (vert) vkDestroyShaderModule(ctx_.device, vert, nullptr);
    if (frag) vkDestroyShaderModule(ctx_.device, frag, nullptr);
    xr_pipelines_.emplace_back(format, pipeline);
    return pipeline;
}

void Renderer::DestroyXr() {
    for (const auto& [format, pipeline] : xr_pipelines_) {
        if (pipeline) vkDestroyPipeline(ctx_.device, pipeline, nullptr);
    }
    xr_pipelines_.clear();
    if (xr_layout_) vkDestroyPipelineLayout(ctx_.device, xr_layout_, nullptr);
    xr_layout_ = VK_NULL_HANDLE;
    if (xr_pool_) vkDestroyDescriptorPool(ctx_.device, xr_pool_, nullptr);
    xr_pool_ = VK_NULL_HANDLE;
    hud_set_ = VK_NULL_HANDLE;
    ctx_.DestroyImage(hud_);
}

}
