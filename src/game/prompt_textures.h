#pragma once

#include <array>
#include <cstdint>
#include <future>
#include <map>
#include <string>
#include <vector>

#include "engine/assets/ftex.h"
#include "engine/platform/input.h"

namespace pt {
class Vfs;
class TextureManager;
}

namespace pt::game {

// A box of a texture's level 0, in pixels
struct PromptBox {
    int x0, y0, x1, y1;
    int Width() const { return x1 - x0; }
    int Height() const { return y1 - y0; }
};

// The button painted into the game's own textures: the R3 chalked beside the fallen photo frame (FrameGround, shsb_labl001_rthr001, the
// zoom). It follows the prompt device as the menus do: another device's button is painted over the same spot of a copy of the texture, in its format and in every mip, once per device, and
// the materials that use the texture are pointed at the copy (TextureManager::RedirectTexture). With a window the painting runs on a
// worker thread and the copy is shown when it is ready; headless runs paint at once, so tests see the switch in the same frame.
class PromptTextures {
public:
    PromptTextures() = default;
    PromptTextures(const PromptTextures&) = delete;
    PromptTextures& operator=(const PromptTextures&) = delete;
    ~PromptTextures();

    void Init(Vfs& vfs, TextureManager& textures, bool background);
    void Update(const PromptStyle& style);

private:
    struct Texture {
        const char* path = nullptr;
        uint32_t original = 0;
        FtexTexture ftex;
        std::map<std::string, uint32_t> variants;
    };
    // A painting: the patched copies of the target's textures, in their order (false in ok: keep the original)
    struct Painted {
        std::string variant;
        std::vector<FtexTexture> textures;
        std::vector<bool> ok;
        double ms = 0.0;
    };
    struct Target {
        std::vector<Texture> textures;
        bool loaded = false;
        bool failed = false;
        bool applied = false;
        std::string shown;
        std::future<Painted> pending;
    };
    std::string VariantFor(const PromptStyle& style) const;
    static Painted Paint(Target& target, const std::string& variant);
    void Finish(Target& target, Painted painted);

    TextureManager* textures_ = nullptr;
    Vfs* vfs_ = nullptr;
    bool background_ = false;
    std::vector<Target> targets_;
    PromptStyle style_;
    bool styled_ = false;
};

}
