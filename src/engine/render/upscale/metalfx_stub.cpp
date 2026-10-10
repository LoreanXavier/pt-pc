#include "engine/render/upscale/upscale.h"

// MetalFX is the macOS build's (metalfx_backend.mm, cmake/MacOS.cmake): with -DPT_METALFX=OFF, or off macOS, the backend is not
// built and the upscaler row reads as unavailable (upscale.cpp). PT_WITH_METALFX is set by cmake/MacOS.cmake.
#if !defined(PT_WITH_METALFX)

namespace pt {

std::unique_ptr<UpscaleBackend> CreateMetalfxBackend(vk::Context&) {
    return nullptr;
}

}  // namespace pt

#endif
