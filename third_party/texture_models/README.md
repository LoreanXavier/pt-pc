# Real-ESRGAN general model

The included ncnn weights are a conversion of Xintao Wang's official `realesr-general-x4v3.pth` (Real-ESRGAN v0.2.5.0), BSD-3-Clause. No P.T. textures or other game data are included.

- Source: https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-x4v3.pth
- Source SHA256: `8dc7edb9ac80ccdc30c3a5dca6616509367f05fbc184ad95b731f05bece96292`
- Converted .bin SHA256: `01450f4a79b81c0f1f3eeefc31121167886125c25e41a6d4773e8ec8062528a1`
- Converted .param SHA256: `a19dd387713e96835729bffd5dfad462e5d835daf27ee5ecb1c4421b8f7459b69`

`tools/convert_texture_model.py` records the conversion used by the previous research session (NumPy required only for conversion, not for building or running the game). The network produces 4x restoration, followed by a 2x2 average to obtain 2x textures. This is the general photo model, not the anime model. The cache includes a fingerprint of both model files and rejects changed models.

The runtime is the upstream 20220424/v0.2.0 Windows release, downloaded and SHA256 checked by CMake. Its MIT and bundled third-party notice, this model's BSD license and bc7enc's notice are copied into portable builds.
