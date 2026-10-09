// The hardware check of FSR 3 frame generation (src/engine/render/upscale/upscale.h, AmdGcnGpu; docs/upscaling.md, FSR 3 frame
// generation on GCN): an AMD GPU that runs wave 64 only is GCN (Polaris, Vega) and below AMD's minimum for frame generation;
// RDNA (wave 32 to 64) and other vendors pass. No GPU needed.
#include <cstdio>

#include "engine/render/upscale/upscale.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

}  // namespace

int main() {
    // the tester's RX 580 2048SP (Polaris) and a Vega: wave 64 only
    Expect(pt::AmdGcnGpu(pt::kAmdVendor, 64, 64), "Polaris (64 to 64) is GCN");
    // RDNA 1 to 4: wave 32 and 64
    Expect(!pt::AmdGcnGpu(pt::kAmdVendor, 32, 64), "RDNA (32 to 64) is not GCN");
    // a driver that offers a single wave 32 size (the floor of future parts) is not GCN either
    Expect(!pt::AmdGcnGpu(pt::kAmdVendor, 32, 32), "wave 32 only is not GCN");
    // NVIDIA (32 to 32) and Intel (8 to 32) never count, whatever their sizes
    Expect(!pt::AmdGcnGpu(0x10DE, 32, 32), "NVIDIA is not GCN");
    Expect(!pt::AmdGcnGpu(0x8086, 8, 32), "Intel is not GCN");
    Expect(!pt::AmdGcnGpu(0x10DE, 64, 64), "a non-AMD vendor at 64 to 64 is not GCN");
    std::printf("upscaler gpu: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
