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
            // one shared event orders the two Vulkan submits and the Metal pass: the inputs are submitted signaling v,
            // MetalFX waits v and signals v+1, the resumed command buffer waits v+1
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
            VkSemaphore semaphore = VK_NULL_HANDLE;
            if (vkCreateSemaphore(ctx_.device, &create, nullptr, &semaphore) != VK_SUCCESS) {
                reason = "the Vulkan timeline semaphore behind the Metal shared event could not be created";
                return false;
            }
            id<MTLCommandQueue> queue = [device newCommandQueue];
            if (!queue) {
                vkDestroySemaphore(ctx_.device, semaphore, nullptr);
                reason = "Metal would not create a command queue";
                return false;
            }
            device_ = device;
            event_ = event;
            queue_ = queue;
            semaphore_ = semaphore;
            value_ = 0;
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
        if (!semaphore_ || !temporal_) {
            return false;
        }
        value_ += 2;
        signal_semaphore = semaphore_;
        signal_value = value_;
        wait_semaphore = semaphore_;
        wait_value = value_ + 1;
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
            [command encodeWaitForEvent:event_ value:value_];
            [temporal_ encodeToCommandBuffer:command];
            [command encodeSignalEvent:event_ value:value_ + 1];
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
            if (semaphore_) {
                vkDestroySemaphore(ctx_.device, semaphore_, nullptr);
                semaphore_ = VK_NULL_HANDLE;
            }
        }
    }

    vk::Context& ctx_;
    PFN_vkExportMetalObjectsEXT export_ = nullptr;
    id<MTLDevice> device_ = nil;
    id<MTLSharedEvent> event_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
    id<MTLFXTemporalScaler> temporal_ = nullptr;
    bool reactive_mask_ = false;
    bool usage_checked_ = false;
    uint32_t usage_width_ = 0;
    uint32_t usage_height_ = 0;
    uint64_t value_ = 0;
    float jitter_sign_ = 1.0f;
    float motion_sign_ = 1.0f;
};

std::unique_ptr<UpscaleBackend> CreateMetalfxBackend(vk::Context& ctx) {
    return std::make_unique<MetalFxBackend>(ctx);
}

}  // namespace pt

#endif
