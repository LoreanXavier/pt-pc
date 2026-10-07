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

struct PromptBox {
    int x0, y0, x1, y1;
    int Width() const { return x1 - x0; }
    int Height() const { return y1 - y0; }
};

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
