#include "engine/data/fox2.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>

#include "engine/core/log.h"

namespace pt::fox2 {
namespace {

constexpr uint32_t kMagic = 0x786F62F2;
constexpr uint64_t kEmptyStringHash = 0xB8A0BF169F98ull;

template <typename T>
T Read(std::span<const uint8_t> data, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

size_t Align16(size_t v) {
    return (v + 15) & ~size_t(15);
}

template <typename T>
T ReadElement(const Property& p, size_t index) {
    T value{};
    if (index < p.Count() && sizeof(T) <= p.element_size) {
        std::memcpy(&value, p.Element(index), sizeof(T));
    }
    return value;
}

}

uint32_t DataTypeSize(DataType type) {
    switch (type) {
    case DataType::Int8:
    case DataType::UInt8:
    case DataType::Bool: return 1;
    case DataType::Int16:
    case DataType::UInt16: return 2;
    case DataType::Int32:
    case DataType::UInt32:
    case DataType::Float: return 4;
    case DataType::Int64:
    case DataType::UInt64:
    case DataType::Double:
    case DataType::String:
    case DataType::Path:
    case DataType::EntityPtr:
    case DataType::FilePtr:
    case DataType::EntityHandle: return 8;
    case DataType::Vector3:
    case DataType::Vector4:
    case DataType::Quat:
    case DataType::Color:
    case DataType::WideVector3: return 16;
    case DataType::Matrix3: return 36;
    case DataType::Matrix4: return 64;
    case DataType::EntityLink: return 32;
    case DataType::PropertyInfo: return 0;
    }
    return 0;
}

const Property* Entity::Find(std::string_view name) const {
    for (const Property& p : properties) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

const Property* Entity::FindDynamic(std::string_view name) const {
    for (const Property& p : dynamic_properties) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

bool DataSetFile::Load(std::string name, std::span<const uint8_t> data) {
    name_ = std::move(name);
    if (Read<uint32_t>(data, 0) != kMagic) {
        LogError("fox2: {} has a bad magic", name_);
        return false;
    }
    const uint32_t entity_count = Read<uint32_t>(data, 8);
    const uint32_t string_table = Read<uint32_t>(data, 0x0C);
    const uint32_t first_entity = Read<uint32_t>(data, 0x10);

    strings_.clear();
    strings_[kEmptyStringHash] = "";
    for (size_t pos = string_table; pos + 12 <= data.size();) {
        const uint64_t hash = Read<uint64_t>(data, pos);
        if (hash == 0) {
            break;
        }
        const uint32_t length = Read<uint32_t>(data, pos + 8);
        if (pos + 12 + length > data.size()) {
            break;
        }
        strings_[hash].assign(reinterpret_cast<const char*>(data.data() + pos + 12), length);
        pos += 12 + length;
    }

    auto parse_properties = [&](size_t base, uint32_t count, std::vector<Property>& out) {
        size_t pos = base;
        for (uint32_t i = 0; i < count; ++i) {
            Property p;
            p.name_hash = Read<uint64_t>(data, pos);
            p.type = static_cast<DataType>(Read<uint8_t>(data, pos + 8));
            p.container = static_cast<Container>(Read<uint8_t>(data, pos + 9));
            const uint16_t element_count = Read<uint16_t>(data, pos + 0x0A);
            const uint16_t payload_offset = Read<uint16_t>(data, pos + 0x0C);
            const uint16_t size = Read<uint16_t>(data, pos + 0x0E);
            p.name = Lookup(p.name_hash);
            p.element_size = DataTypeSize(p.type);
            const size_t payload = pos + payload_offset;
            if (p.container == Container::StringMap) {
                const size_t stride = Align16(8 + p.element_size);
                for (uint16_t e = 0; e < element_count; ++e) {
                    const size_t entry = payload + e * stride;
                    p.keys.push_back(Read<uint64_t>(data, entry));
                    p.payload.insert(p.payload.end(), data.begin() + entry + 8, data.begin() + entry + 8 + p.element_size);
                }
            } else if (p.element_size) {
                p.payload.assign(data.begin() + payload, data.begin() + payload + size_t(element_count) * p.element_size);
            }
            out.push_back(std::move(p));
            pos += size;
        }
    };

    entities_.clear();
    entities_.reserve(entity_count);
    size_t pos = first_entity;
    for (uint32_t i = 0; i < entity_count && pos + 0x40 <= data.size(); ++i) {
        if (std::memcmp(data.data() + pos + 6, "ent", 3) != 0) {
            LogError("fox2: {} entity {} has no signature", name_, i);
            return false;
        }
        Entity e;
        e.class_id = Read<uint16_t>(data, pos + 2);
        e.address = Read<uint64_t>(data, pos + 0x0A);
        e.id = Read<uint64_t>(data, pos + 0x12);
        e.class_version = Read<uint16_t>(data, pos + 0x1A);
        e.class_hash = Read<uint64_t>(data, pos + 0x1C);
        e.class_name = Lookup(e.class_hash);
        const uint16_t static_count = Read<uint16_t>(data, pos + 0x24);
        const uint16_t dynamic_count = Read<uint16_t>(data, pos + 0x26);
        const uint32_t static_offset = Read<uint32_t>(data, pos + 0x28);
        const uint32_t dynamic_offset = Read<uint32_t>(data, pos + 0x2C);
        const uint32_t entity_size = Read<uint32_t>(data, pos + 0x30);
        parse_properties(pos + static_offset, static_count, e.properties);
        parse_properties(pos + dynamic_offset, dynamic_count, e.dynamic_properties);
        by_address_[e.address] = entities_.size();
        entities_.push_back(std::move(e));
        pos += entity_size;
    }
    for (size_t i = 0; i < entities_.size(); ++i) {
        const std::string n = GetString(entities_[i], "name");
        if (!n.empty()) {
            by_name_[n] = i;
            const size_t bar = n.rfind('|');
            if (bar != std::string::npos) {
                by_short_name_.emplace(n.substr(bar + 1), i);
            }
        }
    }
    return true;
}

std::string DataSetFile::Lookup(uint64_t hash) const {
    auto it = strings_.find(hash);
    if (it != strings_.end()) {
        return it->second;
    }
    return hash == 0 ? std::string() : std::format("#{:012X}", hash);
}

const Entity* DataSetFile::ByAddress(uint64_t address) const {
    auto it = by_address_.find(address);
    return it == by_address_.end() ? nullptr : &entities_[it->second];
}

const Entity* DataSetFile::ByName(std::string_view name) const {
    auto it = by_name_.find(std::string(name));
    return it == by_name_.end() ? nullptr : &entities_[it->second];
}

const Entity* DataSetFile::ByShortName(std::string_view name) const {
    if (const Entity* e = ByName(name)) {
        return e;
    }
    auto it = by_short_name_.find(std::string(name));
    return it == by_short_name_.end() ? nullptr : &entities_[it->second];
}

int32_t DataSetFile::GetInt(const Entity& e, std::string_view prop, size_t index, int32_t fallback) const {
    const Property* p = e.Find(prop);
    if (!p) {
        p = e.FindDynamic(prop);
    }
    if (!p || index >= p->Count()) {
        return fallback;
    }
    switch (p->type) {
    case DataType::Int8: return ReadElement<int8_t>(*p, index);
    case DataType::UInt8: return ReadElement<uint8_t>(*p, index);
    case DataType::Int16: return ReadElement<int16_t>(*p, index);
    case DataType::UInt16: return ReadElement<uint16_t>(*p, index);
    case DataType::Int32: return ReadElement<int32_t>(*p, index);
    case DataType::UInt32: return static_cast<int32_t>(ReadElement<uint32_t>(*p, index));
    case DataType::Float: return static_cast<int32_t>(ReadElement<float>(*p, index));
    case DataType::Bool: return ReadElement<uint8_t>(*p, index);
    default: return fallback;
    }
}

uint32_t DataSetFile::GetUInt(const Entity& e, std::string_view prop, size_t index, uint32_t fallback) const {
    return static_cast<uint32_t>(GetInt(e, prop, index, static_cast<int32_t>(fallback)));
}

float DataSetFile::GetFloat(const Entity& e, std::string_view prop, size_t index, float fallback) const {
    const Property* p = e.Find(prop);
    if (!p) {
        p = e.FindDynamic(prop);
    }
    if (!p || index >= p->Count()) {
        return fallback;
    }
    if (p->type == DataType::Float) {
        return ReadElement<float>(*p, index);
    }
    if (p->type == DataType::Double) {
        return static_cast<float>(ReadElement<double>(*p, index));
    }
    return static_cast<float>(GetInt(e, prop, index, static_cast<int32_t>(fallback)));
}

bool DataSetFile::GetBool(const Entity& e, std::string_view prop, size_t index, bool fallback) const {
    return GetInt(e, prop, index, fallback ? 1 : 0) != 0;
}

std::string DataSetFile::GetString(const Entity& e, std::string_view prop, size_t index) const {
    const Property* p = e.Find(prop);
    if (!p) {
        p = e.FindDynamic(prop);
    }
    if (!p || index >= p->Count()) {
        return {};
    }
    if (p->type == DataType::String || p->type == DataType::Path || p->type == DataType::FilePtr) {
        return Lookup(ReadElement<uint64_t>(*p, index));
    }
    return {};
}

glm::vec4 DataSetFile::GetVec4(const Entity& e, std::string_view prop, size_t index) const {
    const Property* p = e.Find(prop);
    if (!p) {
        p = e.FindDynamic(prop);
    }
    if (!p || index >= p->Count() || p->element_size < 16) {
        return glm::vec4(0.0f);
    }
    float v[4];
    std::memcpy(v, p->Element(index), 16);
    return glm::vec4(v[0], v[1], v[2], v[3]);
}

glm::quat DataSetFile::GetQuat(const Entity& e, std::string_view prop, size_t index) const {
    const glm::vec4 v = GetVec4(e, prop, index);
    return glm::quat(v.w, v.x, v.y, v.z);
}

const Property* DataSetFile::FindProperty(const Entity& e, std::string_view prop) const {
    const Property* p = e.Find(prop);
    return p ? p : e.FindDynamic(prop);
}

const Entity* DataSetFile::ElementEntity(const Property& p, size_t index) const {
    if (index >= p.Count()) {
        return nullptr;
    }
    if (p.type == DataType::EntityPtr || p.type == DataType::EntityHandle) {
        return ByAddress(ReadElement<uint64_t>(p, index));
    }
    if (p.type == DataType::EntityLink) {
        return ResolveLink(ReadElement<EntityLink>(p, index));
    }
    return nullptr;
}

std::string DataSetFile::ElementString(const Property& p, size_t index) const {
    if (index >= p.Count()) {
        return {};
    }
    if (p.type == DataType::String || p.type == DataType::Path || p.type == DataType::FilePtr) {
        return Lookup(ReadElement<uint64_t>(p, index));
    }
    return {};
}

std::string DataSetFile::KeyString(const Property& p, size_t index) const {
    return index < p.keys.size() ? Lookup(p.keys[index]) : std::to_string(index);
}

const Entity* DataSetFile::GetEntity(const Entity& e, std::string_view prop, size_t index) const {
    const Property* p = FindProperty(e, prop);
    return p ? ElementEntity(*p, index) : nullptr;
}

std::optional<EntityLink> DataSetFile::GetLink(const Entity& e, std::string_view prop, size_t index) const {
    const Property* p = FindProperty(e, prop);
    if (!p || p->type != DataType::EntityLink || index >= p->Count()) {
        return std::nullopt;
    }
    return ReadElement<EntityLink>(*p, index);
}

const Entity* DataSetFile::ResolveLink(const EntityLink& link) const {
    if (link.handle) {
        return ByAddress(link.handle);
    }
    if (link.name_in_archive) {
        return ByName(Lookup(link.name_in_archive));
    }
    return nullptr;
}

std::vector<std::pair<std::string, const Entity*>> DataSetFile::GetEntityMap(const Entity& e, std::string_view prop) const {
    std::vector<std::pair<std::string, const Entity*>> out;
    const Property* p = FindProperty(e, prop);
    if (!p) {
        return out;
    }
    for (size_t i = 0; i < p->Count(); ++i) {
        const std::string key = i < p->keys.size() ? Lookup(p->keys[i]) : std::to_string(i);
        const Entity* target = nullptr;
        if (p->type == DataType::EntityLink) {
            target = ResolveLink(ReadElement<EntityLink>(*p, i));
        } else if (p->type == DataType::EntityPtr || p->type == DataType::EntityHandle) {
            target = ByAddress(ReadElement<uint64_t>(*p, i));
        }
        out.emplace_back(key, target);
    }
    return out;
}

std::string DataSetFile::EntityName(const Entity& e) const {
    return GetString(e, "name");
}

glm::mat4 DataSetFile::LocalTransform(const Entity& e) const {
    const Entity* t = GetEntity(e, "transform");
    if (!t) {
        return glm::mat4(1.0f);
    }
    const glm::vec3 scale = glm::vec3(GetVec4(*t, "transform_scale"));
    const glm::quat rotation = GetQuat(*t, "transform_rotation_quat");
    const glm::vec3 translation = glm::vec3(GetVec4(*t, "transform_translation"));
    return glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
}

glm::mat4 DataSetFile::WorldTransform(const Entity& e) const {
    glm::mat4 world = LocalTransform(e);
    const Entity* parent = GetEntity(e, "parent");
    int guard = 0;
    while (parent && guard++ < 64) {
        world = LocalTransform(*parent) * world;
        parent = GetEntity(*parent, "parent");
    }
    return world;
}

}
