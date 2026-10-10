// Apple MetalFX (upscaling.md, Apple MetalFX): the port's temporal upscaler on a Metal command buffer. The renderer runs on
// MoltenVK, so the scaler's textures are the Vulkan targets' underlying MTLTextures (VK_EXT_metal_objects exports them) and the
// pass runs between two Vulkan submits of the frame, ordered by one MTLSharedEvent imported into a Vulkan timeline semaphore:
// the inputs are submitted signaling value v, MetalFX waits v and signals v+1, the resumed command buffer waits v+1.
// VK_USE_PLATFORM_METAL_EXT must come first: it brings vulkan_metal.h in through volk.h, and the port's other Vulkan
// translation units do not define it.
#define VK_USE_PLATFORM_METAL_EXT 1

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <volk.h>

#include "engine/render/upscale/frame_generation.h"
#include "engine/render/upscale/upscale.h"

#include <algorithm>
#include <cstdlib>

#include "engine/core/log.h"

#if defined(__APPLE__)

namespace pt {
namespace {

MTLPixelFormat MetalFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM: return MTLPixelFormatR8Unorm;
    case VK_FORMAT_R16_SFLOAT: return MTLPixelFormatR16Float;
    case VK_FORMAT_R16G16_SFLOAT: return MTLPixelFormatRG16Float;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return MTLPixelFormatRGBA16Float;
    case VK_FORMAT_R32_SFLOAT: return MTLPixelFormatR32Float;
    case VK_FORMAT_D32_SFLOAT: return MTLPixelFormatDepth32Float;
    // the swapchain's formats, for the frame interpolation's colour
    case VK_FORMAT_B8G8R8A8_UNORM: return MTLPixelFormatBGRA8Unorm;
    case VK_FORMAT_B8G8R8A8_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
    case VK_FORMAT_R8G8B8A8_UNORM: return MTLPixelFormatRGBA8Unorm;
    case VK_FORMAT_R8G8B8A8_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return MTLPixelFormatBGR10A2Unorm;
    default: return MTLPixelFormatInvalid;
    }
}

const char* MetalFormatName(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatR8Unorm: return "R8Unorm";
    case MTLPixelFormatR16Float: return "R16Float";
    case MTLPixelFormatRG16Float: return "RG16Float";
    case MTLPixelFormatRGBA16Float: return "RGBA16Float";
    case MTLPixelFormatDepth32Float: return "Depth32Float";
    default: return "?";
    }
}

// the texture usages the scaler asks for, against what MoltenVK gave the Vulkan image's MTLTexture
std::string UsageCheck(id<MTLTexture> texture, MTLTextureUsage wanted, const char* what) {
    if (!texture) {
        return std::format("{} is missing", what);
    }
    const MTLTextureUsage have = texture.usage;
    if ((have & wanted) != wanted) {
        return std::format("{} has usage {:x}, MetalFX wants {:x}", what, static_cast<uint64_t>(have), static_cast<uint64_t>(wanted));
    }
    return {};
}

// the HUD-less image and the interp image the renderer composites (UpscaleImage to the renderer's own)
vk::Image VkOf(const UpscaleImage& image) {
    vk::Image out;
    out.image = image.image;
    out.view = image.view;
    out.format = image.format;
    out.extent = {image.extent.width, image.extent.height, 1};
    out.usage = image.usage;
    return out;
}

}  // namespace

class MetalFxBackend final : public UpscaleBackend {
public:
    explicit MetalFxBackend(vk::Context& ctx) : ctx_(ctx) {}
    ~MetalFxBackend() override {
        Release();
        Shutdown();
    }

    UpscalerKind Kind() const override { return UpscalerKind::MetalFx; }

    bool Supported(std::string& reason) override {
        reason.clear();
        return true;
    }

    bool Available(std::string& reason) override {
        if (device_) {
            reason.clear();
            return true;
        }
        // volk.c is built without VK_USE_PLATFORM_METAL_EXT, so it does not carry this pointer: take it from the driver
        export_ = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(vkGetDeviceProcAddr(ctx_.device, "vkExportMetalObjectsEXT"));
        if (!export_) {
            reason = "the Vulkan driver has no VK_EXT_metal_objects";
            return false;
        }
        @autoreleasepool {
            VkExportMetalDeviceInfoEXT device_info{VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
            VkExportMetalObjectsInfoEXT objects{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
            objects.pNext = &device_info;
            export_(ctx_.device, &objects);
            id<MTLDevice> device = device_info.mtlDevice;
            if (!device) {
                reason = "the Vulkan driver would not name its Metal device";
                return false;
            }
            if (![MTLFXTemporalScalerDescriptor supportsDevice:device]) {
                reason = "this GPU has no MetalFX temporal upscaling (Apple silicon or a supported AMD GPU)";
                return false;
            }
            // one shared event orders the Vulkan input submit and MetalFX: Vulkan signals v, MetalFX waits v. Completion is
            // reported to a separate, host-signaled Vulkan timeline so the renderer can wait before presenting; the Vulkan
            // validation layer cannot observe a signal encoded by a Metal command buffer.
            id<MTLSharedEvent> event = [device newSharedEvent];
            if (!event) {
                reason = "Metal would not create a shared event";
                return false;
            }
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            type.initialValue = 0;
            VkImportMetalSharedEventInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_METAL_SHARED_EVENT_INFO_EXT};
            import.mtlSharedEvent = event;
            type.pNext = &import;
            VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            create.pNext = &type;
            VkSemaphore input_semaphore = VK_NULL_HANDLE;
            if (vkCreateSemaphore(ctx_.device, &create, nullptr, &input_semaphore) != VK_SUCCESS) {
                reason = "the Vulkan timeline semaphore behind the Metal shared event could not be created";
                return false;
            }
            VkSemaphoreTypeCreateInfo completion_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            completion_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo completion_create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            completion_create.pNext = &completion_type;
            VkSemaphore completion_semaphore = VK_NULL_HANDLE;
            if (vkCreateSemaphore(ctx_.device, &completion_create, nullptr, &completion_semaphore) != VK_SUCCESS) {
                vkDestroySemaphore(ctx_.device, input_semaphore, nullptr);
                reason = "the Vulkan completion timeline for MetalFX could not be created";
                return false;
            }
            id<MTLCommandQueue> queue = [device newCommandQueue];
            if (!queue) {
                vkDestroySemaphore(ctx_.device, input_semaphore, nullptr);
                vkDestroySemaphore(ctx_.device, completion_semaphore, nullptr);
                reason = "Metal would not create a command queue";
                return false;
            }
            device_ = device;
            event_ = event;
            queue_ = queue;
            input_semaphore_ = input_semaphore;
            completion_semaphore_ = completion_semaphore;
            input_value_ = 0;
            completion_value_ = 0;
            reason.clear();
            return true;
        }
    }

    bool Create(VkCommandBuffer, const UpscaleCreate& create) override {
        @autoreleasepool {
            const MTLPixelFormat color = MetalFormat(create.color_format);
            const MTLPixelFormat depth = MTLPixelFormatDepth32Float;
            const MTLPixelFormat motion = MTLPixelFormatRG16Float;
            const MTLPixelFormat reactive = MTLPixelFormatR8Unorm;
            if (color == MTLPixelFormatInvalid) {
                LogError("metalfx: no Metal pixel format for the colour target ({})", static_cast<int>(create.color_format));
                return false;
            }
            // the getter is isReactiveMaskTextureEnabled (the property is macOS 14.4's): this build needs 15.3's SDK for it
            const bool reactive_mask = [MTLFXTemporalScalerDescriptor instancesRespondToSelector:@selector(isReactiveMaskTextureEnabled)];
            MTLFXTemporalScalerDescriptor* descriptor = [MTLFXTemporalScalerDescriptor new];
            descriptor.colorTextureFormat = color;
            descriptor.depthTextureFormat = depth;
            descriptor.motionTextureFormat = motion;
            descriptor.outputTextureFormat = color;
            descriptor.inputWidth = create.render.width;
            descriptor.inputHeight = create.render.height;
            descriptor.outputWidth = create.display.width;
            descriptor.outputHeight = create.display.height;
            descriptor.autoExposureEnabled = NO;
            descriptor.inputContentPropertiesEnabled = NO;
            if (@available(macOS 14.4, *)) {
                descriptor.reactiveMaskTextureEnabled = reactive_mask;
                if (reactive_mask) {
                    descriptor.reactiveMaskTextureFormat = reactive;
                }
            }
            // compile in the background: the image quality is the same, the first frames run the interim upscaler
            descriptor.requiresSynchronousInitialization = NO;
            id<MTLFXTemporalScaler> scaler = [descriptor newTemporalScalerWithDevice:device_];
            [descriptor release];
            if (!scaler) {
                LogError("metalfx: MetalFX would not create the temporal scaler");
                return false;
            }
            temporal_ = scaler;
            reactive_mask_ = @available(macOS 14.4, *) && reactive_mask;
            // the scaler's own names for the signs, for the log and for the test overrides to flip
            static const float jitter_sign = [] {
                const char* e = std::getenv("PT_METALFX_JITTER_SIGN");
                return e ? static_cast<float>(std::atof(e)) : 1.0f;
            }();
            static const float motion_sign = [] {
                const char* e = std::getenv("PT_METALFX_MOTION_SIGN");
                return e ? static_cast<float>(std::atof(e)) : 1.0f;
            }();
            jitter_sign_ = jitter_sign;
            motion_sign_ = motion_sign;
            LogInfo("metalfx: temporal scaler for {}x{} -> {}x{}, reactive mask {}, input usage {:x}/{:x}/{:x}{}, output {:x}",
                    create.render.width, create.render.height, create.display.width, create.display.height, reactive_mask_ ? "on" : "off",
                    static_cast<uint64_t>(temporal_.colorTextureUsage), static_cast<uint64_t>(temporal_.depthTextureUsage),
                    static_cast<uint64_t>(temporal_.motionTextureUsage), reactive_mask_ ? static_cast<uint64_t>(temporal_.reactiveTextureUsage) : 0,
                    static_cast<uint64_t>(temporal_.outputTextureUsage));
            return true;
        }
    }

    bool Dispatch(const UpscaleDispatch&) override { return false; }

    bool ExternalSubmit() const override { return temporal_ != nullptr; }

    bool ExternalSync(VkSemaphore& signal_semaphore, uint64_t& signal_value, VkSemaphore& wait_semaphore, uint64_t& wait_value) override {
        if (!input_semaphore_ || !completion_semaphore_ || !temporal_) {
            return false;
        }
        input_value_ += 1;
        completion_value_ += 1;
        pending_input_value_ = input_value_;
        pending_completion_value_ = completion_value_;
        signal_semaphore = input_semaphore_;
        signal_value = input_value_;
        wait_semaphore = completion_semaphore_;
        wait_value = completion_value_;
        return true;
    }

    bool ExternalRun(const UpscaleDispatch& dispatch) override {
        if (!temporal_ || !queue_) {
            return false;
        }
        @autoreleasepool {
            const auto export_texture = [&](const UpscaleImage& image) -> id<MTLTexture> {
                if (!image.Valid()) {
                    return nil;
                }
                VkExportMetalTextureInfoEXT texture_info{VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT};
                texture_info.image = image.image;
                // the plane member is a plane aspect: PLANE_0 for a non-multi-planar image, whatever its aspect
                // (VUID-VkExportMetalObjectsInfoEXT-pNext-06798/-06799)
                texture_info.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
                VkExportMetalObjectsInfoEXT objects{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
                objects.pNext = &texture_info;
                export_(ctx_.device, &objects);
                return texture_info.mtlTexture;
            };
            id<MTLTexture> color = export_texture(dispatch.color);
            id<MTLTexture> depth = export_texture(dispatch.depth);
            id<MTLTexture> motion = export_texture(dispatch.motion);
            id<MTLTexture> output = export_texture(dispatch.output);
            id<MTLTexture> reactive = reactive_mask_ ? export_texture(dispatch.reactive) : nil;
            // the usages are checked once per size: a mismatch is a build-time problem, not a per-frame one
            if (!usage_checked_ || dispatch.render.width != usage_width_ || dispatch.render.height != usage_height_) {
                std::string problem;
                const auto check = [&](id<MTLTexture> texture, MTLTextureUsage wanted, const char* what) {
                    const std::string note = UsageCheck(texture, wanted, what);
                    if (!note.empty()) {
                        if (!problem.empty()) {
                            problem += "; ";
                        }
                        problem += note;
                    }
                };
                check(color, temporal_.colorTextureUsage, "colour");
                check(depth, temporal_.depthTextureUsage, "depth");
                check(motion, temporal_.motionTextureUsage, "motion");
                check(output, temporal_.outputTextureUsage, "output");
                if (reactive_mask_) {
                    check(reactive, temporal_.reactiveTextureUsage, "reactive");
                }
                if (!problem.empty()) {
                    LogError("metalfx: {}", problem);
                    return false;
                }
                usage_checked_ = true;
                usage_width_ = dispatch.render.width;
                usage_height_ = dispatch.render.height;
            }
            temporal_.colorTexture = color;
            temporal_.depthTexture = depth;
            temporal_.motionTexture = motion;
            temporal_.outputTexture = output;
            temporal_.inputContentWidth = dispatch.render.width;
            temporal_.inputContentHeight = dispatch.render.height;
            temporal_.depthReversed = YES;
            // the colour is pre-exposed by the frame's exposure, as for DLSS (upscaling.md, step 6); MetalFX divides it back
            temporal_.preExposure = dispatch.pre_exposure;
            temporal_.reset = dispatch.reset ? YES : NO;
            // the port's jitter moves the image content by +jitter pixels, the sign the other upscalers take too
            temporal_.jitterOffsetX = jitter_sign_ * dispatch.jitter.x;
            temporal_.jitterOffsetY = jitter_sign_ * dispatch.jitter.y;
            // the vectors are previous minus current in UV units (current to previous), which is MetalFX's convention too
            temporal_.motionVectorScaleX = motion_sign_ * dispatch.motion_scale.x;
            temporal_.motionVectorScaleY = motion_sign_ * dispatch.motion_scale.y;
            if (reactive_mask_) {
                temporal_.reactiveMaskTexture = reactive;
            }
            id<MTLCommandBuffer> command = [queue_ commandBuffer];
            if (!command) {
                LogError("metalfx: Metal would not give a command buffer");
                return false;
            }
            [command encodeWaitForEvent:event_ value:pending_input_value_];
            [temporal_ encodeToCommandBuffer:command];
            const VkDevice vk_device = ctx_.device;
            const VkSemaphore vk_semaphore = completion_semaphore_;
            const uint64_t vk_value = pending_completion_value_;
            [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                (void)completed;
                VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
                signal.semaphore = vk_semaphore;
                signal.value = vk_value;
                const VkResult result = vkSignalSemaphore(vk_device, &signal);
                if (result != VK_SUCCESS) {
                    LogError("metalfx: host signal for temporal upscaling failed ({})", static_cast<int>(result));
                }
            }];
            [command commit];
            return true;
        }
    }

    void Release() override {
        @autoreleasepool {
            if (temporal_) {
                [temporal_ release];
                temporal_ = nullptr;
            }
            usage_checked_ = false;
            usage_width_ = usage_height_ = 0;
        }
    }

protected:
    void Shutdown() {
        @autoreleasepool {
            if (queue_) {
                [queue_ release];
                queue_ = nullptr;
            }
            if (event_) {
                [event_ release];
                event_ = nullptr;
            }
            if (input_semaphore_) {
                vkDestroySemaphore(ctx_.device, input_semaphore_, nullptr);
                input_semaphore_ = VK_NULL_HANDLE;
            }
            if (completion_semaphore_) {
                vkDestroySemaphore(ctx_.device, completion_semaphore_, nullptr);
                completion_semaphore_ = VK_NULL_HANDLE;
            }
        }
    }

    vk::Context& ctx_;
    PFN_vkExportMetalObjectsEXT export_ = nullptr;
    id<MTLDevice> device_ = nil;
    id<MTLSharedEvent> event_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    VkSemaphore input_semaphore_ = VK_NULL_HANDLE;
    VkSemaphore completion_semaphore_ = VK_NULL_HANDLE;
    id<MTLFXTemporalScaler> temporal_ = nullptr;
    bool reactive_mask_ = false;
    bool usage_checked_ = false;
    uint32_t usage_width_ = 0;
    uint32_t usage_height_ = 0;
    uint64_t input_value_ = 0;
    uint64_t completion_value_ = 0;
    uint64_t pending_input_value_ = 0;
    uint64_t pending_completion_value_ = 0;
    float jitter_sign_ = 1.0f;
    float motion_sign_ = 1.0f;
};

// Apple MetalFX frame interpolation (upscaling.md, Apple MetalFX frame interpolation): one generated frame between two
// rendered frames, shown by the renderer's own double present (GeneratedFrameHooks, renderer.h). MetalFX does not own the
// swapchain, so unlike FSR 3's or DLSS's frame generation no swapchain is replaced: the renderer acquires a second swapchain
// image a frame and presents the generated frame into it before the rendered one, and switching it on or off needs no
// swapchain recreation. The interpolated images are the HUD-less copies the renderer composites into one target per
// swapchain image, as for FSR 3, and the interpolation's depth and motion are the upscaler's inputs at the render extent, so
// it needs an upscaler. macOS 26 or newer: the frame interpolator is that API's (MTLFXFrameInterpolator, macOS 26).
class MetalFxFrameGeneration final : public FrameGeneration {
public:
    explicit MetalFxFrameGeneration(vk::Context& ctx) : ctx_(ctx) {}
    ~MetalFxFrameGeneration() override { Shutdown(); }

    bool Available(std::string& reason) override {
        if (checked_) {
            reason = reason_;
            return available_;
        }
        checked_ = true;
        @autoreleasepool {
            if (@available(macOS 26.0, *)) {
                export_fn_ = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(vkGetDeviceProcAddr(ctx_.device, "vkExportMetalObjectsEXT"));
                VkExportMetalDeviceInfoEXT device_info{VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
                VkExportMetalObjectsInfoEXT objects{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
                objects.pNext = &device_info;
                if (!export_fn_) {
                    reason_ = "the Vulkan driver has no VK_EXT_metal_objects";
                } else {
                    export_fn_(ctx_.device, &objects);
                    if (!device_info.mtlDevice) {
                        reason_ = "the Vulkan driver would not name its Metal device";
                    } else if (![MTLFXFrameInterpolatorDescriptor supportsDevice:device_info.mtlDevice]) {
                        reason_ = "this GPU has no MetalFX frame interpolation (needs Apple silicon)";
                    } else {
                        available_ = true;
                        reason_.clear();
                    }
                }
            } else {
                reason_ = "needs macOS 26 or newer";
            }
        }
        reason = reason_;
        LogInfo("frame generation: {}{}", available_ ? "available" : "unavailable: ", available_ ? "" : reason_);
        return available_;
    }

    bool Update(bool wanted) override {
        if (wanted && Available(reason_unused_) && EnsureTargets()) {
            generating_ = true;
            ctx_.force_vsync = true;
            return false;
        }
        if (generating_) {
            generating_ = false;
            ctx_.force_vsync = false;
        }
        DestroyTargets();
        return false;
    }

    bool Generating() const override { return generating_; }

    bool OwnsGeneratedFrame() const override { return true; }

    void Prepare(const FrameGenPrepare& p) override {
        if (!generating_) {
            return;
        }
        // MetalFX writes interp_ on its command buffer after the frame submit. Put the image in GENERAL in the submitted
        // Vulkan command buffer before exporting it; the generated-frame composite transitions it to sampled after the
        // shared-event wait. This is the Vulkan half of the cross-API ownership/layout hand-off.
        vk::ImageBarrier(p.cmd, interp_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, interp_layout_,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                         VK_IMAGE_LAYOUT_GENERAL);
        interp_layout_ = VK_IMAGE_LAYOUT_GENERAL;
        prepared_ = true;
        prepare_ = p;
        if (p.reset) {
            reset_ = true;
        }
    }

    const vk::Image* Present(uint32_t image_index) override {
        if (!generating_ || image_index >= hudless_.size()) {
            return nullptr;
        }
        previous_ = current_;
        current_ = image_index;
        has_previous_ = presented_count_ > 0 && previous_ != current_;
        ++presented_count_;
        return &hudless_[current_];
    }

    const vk::Image* GeneratedImage() override {
        if (!generating_ || !prepared_ || !has_previous_) {
            return nullptr;
        }
        prepared_ = false;
        const vk::Image* generated = Interpolate();
        if (generated) {
            // the renderer's generated-frame composite records GENERAL -> SHADER_READ_ONLY before this slot ends; keep the
            // state for the next submitted frame's transition back to GENERAL.
            interp_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        return generated;
    }

    bool ExternalSync(VkSemaphore& signal_semaphore, uint64_t& signal_value, VkSemaphore& wait_semaphore, uint64_t& wait_value) override {
        if (!input_semaphore_ || !completion_semaphore_) {
            return false;
        }
        metal_wait_value_ += 1;
        metal_signal_value_ += 1;
        signal_semaphore = input_semaphore_;
        signal_value = metal_wait_value_;
        wait_semaphore = completion_semaphore_;
        wait_value = metal_signal_value_;
        return true;
    }

    void Shutdown() override {
        if (generating_) {
            LogSummary();
        }
        generating_ = false;
        ctx_.force_vsync = false;
        // completion handlers host-signal semaphore_ after MetalFX; wait for the Vulkan queue that consumes those signals
        // before destroying the imported shared event and semaphore.
        if (ctx_.device) {
            vkDeviceWaitIdle(ctx_.device);
        }
        DestroyTargets();
        @autoreleasepool {
            if (interpolator_) {
                [interpolator_ release];
                interpolator_ = nil;
            }
            if (queue_) {
                [queue_ release];
                queue_ = nil;
            }
            if (event_) {
                [event_ release];
                event_ = nil;
            }
            if (input_semaphore_) {
                vkDestroySemaphore(ctx_.device, input_semaphore_, nullptr);
                input_semaphore_ = VK_NULL_HANDLE;
            }
            if (completion_semaphore_) {
                vkDestroySemaphore(ctx_.device, completion_semaphore_, nullptr);
                completion_semaphore_ = VK_NULL_HANDLE;
            }
        }
    }

private:
    bool EnsureTargets() {
        if (!ctx_.swapchain.handle || ctx_.swapchain.images.empty() || ctx_.swapchain.extent.width == 0 ||
            ctx_.swapchain.extent.height == 0) {
            // headless, or no window yet: the frame generation needs one (as FSR 3's does)
            if (window_note_ != "no window") {
                window_note_ = "no window";
                LogInfo("frame generation: Apple MetalFX unavailable: no window");
            }
            return false;
        }
        window_note_.clear();
        if (hudless_.size() == ctx_.swapchain.images.size() && display_.width == ctx_.swapchain.extent.width &&
            display_.height == ctx_.swapchain.extent.height && format_ == ctx_.swapchain.format) {
            return true;
        }
        DestroyTargets();
        const VkFormat format = ctx_.swapchain.format;
        const VkExtent2D display = ctx_.swapchain.extent;
        const MTLPixelFormat color = MetalFormat(format);
        if (color == MTLPixelFormatInvalid) {
            LogError("frame generation: no Metal pixel format for the swapchain ({})", static_cast<int>(format));
            return false;
        }
        if (!export_fn_) {
            return false;
        }
        VkExportMetalDeviceInfoEXT device_info{VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
        VkExportMetalObjectsInfoEXT objects{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
        objects.pNext = &device_info;
        export_fn_(ctx_.device, &objects);
        id<MTLDevice> device = device_info.mtlDevice;
        if (!device) {
            return false;
        }
        if (!event_) {
            event_ = [device newSharedEvent];
            queue_ = [device newCommandQueue];
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkImportMetalSharedEventInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_METAL_SHARED_EVENT_INFO_EXT};
            import.mtlSharedEvent = event_;
            type.pNext = &import;
            VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            create.pNext = &type;
            if (!event_ || !queue_ || vkCreateSemaphore(ctx_.device, &create, nullptr, &input_semaphore_) != VK_SUCCESS) {
                LogError("frame generation: the Metal event or queue could not be created");
                return false;
            }
            // a host-signaled completion timeline is deliberately separate from the input shared-event timeline: Metal
            // completion handlers signal this one only after interpolation, with no queue-signaled values pending.
            VkSemaphoreTypeCreateInfo completion_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            completion_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo completion_create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            completion_create.pNext = &completion_type;
            if (vkCreateSemaphore(ctx_.device, &completion_create, nullptr, &completion_semaphore_) != VK_SUCCESS) {
                LogError("frame generation: the MetalFX completion timeline could not be created");
                return false;
            }
        }
        if (@available(macOS 26.0, *)) {
            // the interpolator itself is built in Interpolate, once the upscaler's render extent is known (its input sizes
            // are the motion and depth textures')
        } else {
            return false;
        }
        format_ = format;
        display_ = display;
        hudless_.resize(ctx_.swapchain.images.size());
        for (vk::Image& image : hudless_) {
            if (!ctx_.CreateImage(image, format_, {display_.width, display_.height, 1},
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
                LogError("frame generation: hud-less target creation failed");
                DestroyTargets();
                return false;
            }
        }
        if (!ctx_.CreateImage(interp_, format_, {display_.width, display_.height, 1},
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) {
            LogError("frame generation: the generated frame's image could not be created");
            DestroyTargets();
            return false;
        }
        reset_ = true;
        presented_count_ = 0;
        generated_count_ = 0;
        current_ = previous_ = 0;
        has_previous_ = false;
        LogInfo("frame generation: Apple MetalFX for {}x{}, {} hud-less targets", display_.width, display_.height, hudless_.size());
        return true;
    }

    const vk::Image* Interpolate() {
        @autoreleasepool {
            if (@available(macOS 26.0, *)) {
                id<MTLFXFrameInterpolator> interpolator = reinterpret_cast<id<MTLFXFrameInterpolator>>(interpolator_);
                const auto export_texture = [&](const vk::Image& image) -> id<MTLTexture> {
                    if (!image.image) {
                        return nil;
                    }
                    VkExportMetalTextureInfoEXT texture_info{VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT};
                    texture_info.image = image.image;
                    texture_info.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
                    VkExportMetalObjectsInfoEXT objects{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
                    objects.pNext = &texture_info;
                    export_fn_(ctx_.device, &objects);
                    return texture_info.mtlTexture;
                };
                // the interpolator's input sizes are the motion and depth textures' (the upscaler's render extent); the colour
                // is at the output size. Built once per render extent.
                if (!interpolator_ || prepare_.render.width != interp_render_.width || prepare_.render.height != interp_render_.height) {
                    if (!CreateInterpolator(prepare_.render)) {
                        return nullptr;
                    }
                }
                id<MTLCommandBuffer> command = [queue_ commandBuffer];
                if (!command) {
                    LogError("frame generation: Metal would not give a command buffer");
                    return nullptr;
                }
                [command encodeWaitForEvent:event_ value:metal_wait_value_];
                // the HUD-less copy of the previous and this frame, and the upscaler's depth and motion of this frame
                const vk::Image& current = hudless_[current_];
                const vk::Image& previous = hudless_[previous_];
                interpolator.colorTexture = export_texture(current);
                interpolator.prevColorTexture = export_texture(previous);
                interpolator.depthTexture = export_texture(VkOf(prepare_.depth));
                interpolator.motionTexture = export_texture(VkOf(prepare_.motion));
                interpolator.outputTexture = export_texture(interp_);
                // colour is already the jitter-resolved output of the temporal upscaler; these full-output-size frames are not
                // jittered inputs, so do not apply the lower-resolution scene jitter to the frame interpolator
                interpolator.jitterOffsetX = 0.0f;
                interpolator.jitterOffsetY = 0.0f;
                interpolator.motionVectorScaleX = prepare_.motion_scale.x;
                interpolator.motionVectorScaleY = prepare_.motion_scale.y;
                interpolator.deltaTime = prepare_.frame_ms / 1000.0f;
                interpolator.nearPlane = prepare_.near_plane;
                interpolator.farPlane = 100000.0f;
                interpolator.fieldOfView = glm::degrees(prepare_.fov_y);
                interpolator.aspectRatio = static_cast<float>(display_.width) / static_cast<float>(std::max(1u, display_.height));
                interpolator.shouldResetHistory = reset_ ? YES : NO;
                reset_ = false;
                [interpolator encodeToCommandBuffer:command];
                const VkDevice vk_device = ctx_.device;
                const VkSemaphore vk_semaphore = completion_semaphore_;
                const uint64_t vk_value = metal_signal_value_;
                [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                    (void)completed;
                    // Metal's event signal is invisible to Vulkan validation (and leaves vkQueuePresentKHR's binary
                    // semaphore dependency unresolved there). Signal the imported timeline semaphore on the host from the
                    // Metal completion handler: presentation waits on a Vulkan-visible signal that occurs only after the
                    // interpolation actually finishes.
                    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
                    signal.semaphore = vk_semaphore;
                    signal.value = vk_value;
                    const VkResult result = vkSignalSemaphore(vk_device, &signal);
                    if (result != VK_SUCCESS) {
                        LogError("frame generation: host signal for MetalFX interpolation failed ({})", static_cast<int>(result));
                    }
                }];
                [command commit];
                ++generated_count_;
                return &interp_;
            }
        }
        return nullptr;
    }

    bool CreateInterpolator(VkExtent2D render) {
        if (!export_fn_) {
            return false;
        }
        VkExportMetalDeviceInfoEXT device_info{VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
        VkExportMetalObjectsInfoEXT objects{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
        objects.pNext = &device_info;
        export_fn_(ctx_.device, &objects);
        id<MTLDevice> device = device_info.mtlDevice;
        if (!device) {
            return false;
        }
        if (@available(macOS 26.0, *)) {
            const MTLPixelFormat color = MetalFormat(format_);
            if (color == MTLPixelFormatInvalid) {
                return false;
            }
            MTLFXFrameInterpolatorDescriptor* descriptor = [MTLFXFrameInterpolatorDescriptor new];
            descriptor.colorTextureFormat = color;
            descriptor.outputTextureFormat = color;
            descriptor.depthTextureFormat = MTLPixelFormatDepth32Float;
            descriptor.motionTextureFormat = MTLPixelFormatRG16Float;
            descriptor.inputWidth = render.width;
            descriptor.inputHeight = render.height;
            descriptor.outputWidth = display_.width;
            descriptor.outputHeight = display_.height;
            id interpolator = [descriptor newFrameInterpolatorWithDevice:device];
            [descriptor release];
            if (!interpolator) {
                LogError("frame generation: MetalFX would not create the frame interpolator");
                return false;
            }
            if (interpolator_) {
                [interpolator_ release];
            }
            interpolator_ = interpolator;
            interp_render_ = render;
            LogInfo("frame generation: interpolator for {}x{} motion and depth -> {}x{} colour", render.width, render.height, display_.width,
                    display_.height);
            return true;
        }
        return false;
    }

    void LogSummary() {
        if (presented_count_ > 0) {
            LogInfo("frame generation: MetalFX queued {} interpolated frames for {} rendered frames", generated_count_, presented_count_);
        }
    }

    void DestroyTargets() {
        for (vk::Image& image : hudless_) {
            ctx_.DestroyImage(image);
        }
        hudless_.clear();
        ctx_.DestroyImage(interp_);
        if (interpolator_) {
            [interpolator_ release];
            interpolator_ = nil;
        }
        format_ = VK_FORMAT_UNDEFINED;
        display_ = {};
        interp_render_ = {};
        interp_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        presented_count_ = 0;
        has_previous_ = false;
        prepared_ = false;
        reset_ = true;
    }

    vk::Context& ctx_;
    PFN_vkExportMetalObjectsEXT export_fn_ = nullptr;
    id interpolator_ = nil;
    id<MTLSharedEvent> event_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    VkSemaphore input_semaphore_ = VK_NULL_HANDLE;
    VkSemaphore completion_semaphore_ = VK_NULL_HANDLE;
    std::vector<vk::Image> hudless_;
    vk::Image interp_;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D display_{};
    VkExtent2D interp_render_{};
    VkImageLayout interp_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    FrameGenPrepare prepare_;
    bool checked_ = false;
    bool available_ = false;
    bool generating_ = false;
    bool prepared_ = false;
    bool reset_ = true;
    bool has_previous_ = false;
    uint32_t presented_count_ = 0;
    uint32_t generated_count_ = 0;
    uint32_t current_ = 0;
    uint32_t previous_ = 0;
    uint64_t metal_wait_value_ = 0;
    uint64_t metal_signal_value_ = 0;
    std::string reason_;
    std::string reason_unused_;
    std::string window_note_;
};

std::unique_ptr<UpscaleBackend> CreateMetalfxBackend(vk::Context& ctx) {
    return std::make_unique<MetalFxBackend>(ctx);
}

std::unique_ptr<FrameGeneration> CreateMetalfxFrameGeneration(vk::Context& ctx) {
    return std::make_unique<MetalFxFrameGeneration>(ctx);
}

}  // namespace pt

#endif
