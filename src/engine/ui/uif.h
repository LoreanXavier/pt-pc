#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pt::ui {

enum class UifNodeType : uint16_t { Root = 0, Null = 1, Mesh = 2, Text = 3 };

enum UifTextureSlot : int { kUifBase = 0, kUifLayer = 1, kUifMask = 2, kUifScreen = 3 };

struct UifMaterial {
    std::array<int, 4> textures{-1, -1, -1, -1};
    std::array<float, 28> params{};
};

struct UifPoint {
    uint64_t name = 0;
    glm::vec2 position{0.0f};
    std::vector<uint16_t> vertices;
};

struct UifNode {
    uint16_t id = 0;
    UifNodeType type = UifNodeType::Null;
    int parent = -1;
    uint16_t flags = 0;
    uint16_t text_flags = 0;
    glm::vec2 size{1.0f};
    glm::vec2 scale{1.0f};
    glm::vec4 rotation{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 translate{0.0f};
    float priority = 0.0f;
    glm::vec4 color{1.0f};
    std::vector<glm::vec2> positions;
    std::vector<glm::vec2> uvs;
    std::vector<uint16_t> indices;
    std::vector<UifPoint> points;
    UifMaterial material;
    glm::vec4 box_min{0.0f};
    glm::vec4 box_max{0.0f};
    uint64_t text_name = 0;
    uint64_t font_name = 0;
    uint64_t material_name = 0;

    bool Additive() const { return (flags & 0xFF) == 4; }
};

int UifParamSlot(uint32_t code32);

class UifModel {
public:
    bool Parse(std::span<const uint8_t> data, std::string* error);

    const std::vector<UifNode>& Nodes() const { return nodes_; }
    const std::vector<uint64_t>& Names() const { return names_; }
    const std::vector<std::string>& Textures() const { return textures_; }
    int IndexOfId(uint16_t id) const;
    const UifNode* FindById(uint16_t id) const;
    UifNode* FindById(uint16_t id);
    uint16_t AddNode(UifNode node, uint64_t name);
    int AddTexture(std::string path);

private:
    std::vector<UifNode> nodes_;
    std::vector<uint64_t> names_;
    std::vector<std::string> textures_;
    std::vector<int> index_of_id_;
};

}
