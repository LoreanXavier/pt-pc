# Enhanced texture runtime notices

These notices cover the dependencies of the pinned Windows runtime
Real-ESRGAN-ncnn-vulkan v0.2.0. Its release archive SHA256 and download URL
are in cmake/EnhancedTextures.cmake. The runtime and model licenses are
also copied into the portable package, alongside the BC7 encoder license.

The upstream v0.2.0 source pins ncnn at
6125c9f47cd14b589de0521350668cf9d3d37e3c and libwebp at
8ea81561d2fdd382da60f57958741a7c23a18eb6. That ncnn revision pins glslang
at 4afd69177258d0636f78d2c4efb823ab6382a187. sources.json records the
original source URLs and SHA256 hashes of the downloaded notices or
source headers. The stb MIT alternatives were extracted from the two
headers in the runtime's v0.2.0 source. The dirent attribution is taken
from its bundled win32dirent.h header and accompanies the MIT terms.

No proprietary P.T. textures, generated texture caches, controller
diagnostics or Microsoft debug runtime are distributed in the package.
