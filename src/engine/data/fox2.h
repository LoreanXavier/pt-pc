#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pt::fox2 {

enum class DataType : uint8_t {
    Int8, UInt8, Int16, UInt16, Int32, UInt32, Int64, UInt64, Float, Double, Bool, String, Path, EntityPtr,
    Vector3, Vector4, Quat, Matrix3, Matrix4, Color, FilePtr, EntityHandle, EntityLink, PropertyInfo, WideVector3,
};

enum class Container : uint8_t { StaticArray = 0, DynamicArray = 1, StringMap = 2, List = 3 };

struct EntityLink {
    uint64_t package_path = 0;
    uint64_t archive_path = 0;
    uint64_t name_in_archive = 0;
    uint64_t handle = 0;
};

struct Property {
    uint64_t name_hash = 0;
    std::string name;
    DataType type = DataType::Int8;
    Container container = Container::StaticArray;
    uint32_t element_size = 0;
    std::vector<uint64_t> keys;
    std::vector<uint8_t> payload;

    size_t Count() const { return element_size ? payload.size() / element_size : 0; }
    const uint8_t* Element(size_t i) const { return payload.data() + i * element_size; }
};

class DataSetFile;

struct Entity {
    std::string class_name;
    uint64_t class_hash = 0;
    uint64_t address = 0;
    uint64_t id = 0;
    uint16_t class_id = 0;
    uint16_t class_version = 0;
    std::vector<Property> properties;
    std::vector<Property> dynamic_properties;

    const Property* Find(std::string_view name) const;
    const Property* FindDynamic(std::string_view name) const;
};

class DataSetFile {
public:
    bool Load(std::string name, std::span<const uint8_t> data);

    const std::string& Name() const { return name_; }
    const std::vector<Entity>& Entities() const { return entities_; }
    const Entity* ByAddress(uint64_t address) const;
    const Entity* ByName(std::string_view name) const;
    const Entity* ByShortName(std::string_view name) const;
    std::string Lookup(uint64_t hash) const;

    int32_t GetInt(const Entity& e, std::string_view prop, size_t index = 0, int32_t fallback = 0) const;
    uint32_t GetUInt(const Entity& e, std::string_view prop, size_t index = 0, uint32_t fallback = 0) const;
    float GetFloat(const Entity& e, std::string_view prop, size_t index = 0, float fallback = 0.0f) const;
    bool GetBool(const Entity& e, std::string_view prop, size_t index = 0, bool fallback = false) const;
    std::string GetString(const Entity& e, std::string_view prop, size_t index = 0) const;
    glm::vec4 GetVec4(const Entity& e, std::string_view prop, size_t index = 0) const;
    glm::quat GetQuat(const Entity& e, std::string_view prop, size_t index = 0) const;
    const Entity* GetEntity(const Entity& e, std::string_view prop, size_t index = 0) const;
    const Entity* ResolveLink(const EntityLink& link) const;
    std::optional<EntityLink> GetLink(const Entity& e, std::string_view prop, size_t index = 0) const;
    std::vector<std::pair<std::string, const Entity*>> GetEntityMap(const Entity& e, std::string_view prop) const;

    const Property* FindProperty(const Entity& e, std::string_view prop) const;
    const Entity* ElementEntity(const Property& p, size_t index) const;
    std::string ElementString(const Property& p, size_t index) const;
    std::string KeyString(const Property& p, size_t index) const;

    std::string EntityName(const Entity& e) const;
    glm::mat4 LocalTransform(const Entity& e) const;
    glm::mat4 WorldTransform(const Entity& e) const;

private:
    std::string name_;
    std::vector<Entity> entities_;
    std::unordered_map<uint64_t, size_t> by_address_;
    std::unordered_map<std::string, size_t> by_name_;
    std::unordered_map<std::string, size_t> by_short_name_;
    std::unordered_map<uint64_t, std::string> strings_;
};

uint32_t DataTypeSize(DataType type);

}
