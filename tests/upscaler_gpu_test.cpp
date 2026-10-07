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

}

int main() {
    Expect(pt::AmdGcnGpu(pt::kAmdVendor, 64, 64), "Polaris (64 to 64) is GCN");
    Expect(!pt::AmdGcnGpu(pt::kAmdVendor, 32, 64), "RDNA (32 to 64) is not GCN");
    Expect(!pt::AmdGcnGpu(pt::kAmdVendor, 32, 32), "wave 32 only is not GCN");
    Expect(!pt::AmdGcnGpu(0x10DE, 32, 32), "NVIDIA is not GCN");
    Expect(!pt::AmdGcnGpu(0x8086, 8, 32), "Intel is not GCN");
    Expect(!pt::AmdGcnGpu(0x10DE, 64, 64), "a non-AMD vendor at 64 to 64 is not GCN");
    std::printf("upscaler gpu: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
