#define GLM_ENABLE_EXPERIMENTAL
#include "game/lua_entity.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <cstring>
#include <format>
#include <string>
#include <string_view>

#include "engine/core/log.h"
#include "game/stage_manager.h"

namespace pt::game {
namespace {

constexpr const char* kEntityMeta = "pt.Entity";
constexpr const char* kVector3Meta = "pt.Vector3";
constexpr const char* kQuatMeta = "pt.Quat";
constexpr const char* kHostKey = "pt.EntityHost";

EntityHost* Host(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kHostKey);
    auto* host = static_cast<EntityHost*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return host;
}

struct Resolved {
    const EntityRef* ref = nullptr;
    Stage* stage = nullptr;
    const StageData* file = nullptr;
};

Resolved Resolve(lua_State* L, int index) {
    Resolved r;
    r.ref = ToEntity(L, index);
    if (!r.ref || !r.ref->entity) {
        return r;
    }
    EntityHost* host = Host(L);
    r.stage = host ? host->FindStage(r.ref->stage_id) : nullptr;
    if (r.stage) {
        r.file = r.stage->FileOf(r.ref->entity);
    }
    return r;
}

template <typename T>
T ReadAt(const fox2::Property& p, size_t index) {
    T value{};
    std::memcpy(&value, p.Element(index), sizeof(T));
    return value;
}

bool PushElement(lua_State* L, const Stage& stage, const fox2::DataSetFile& file, const fox2::Property& p, size_t i) {
    using fox2::DataType;
    switch (p.type) {
    case DataType::Int8: lua_pushnumber(L, ReadAt<int8_t>(p, i)); return true;
    case DataType::UInt8: lua_pushnumber(L, ReadAt<uint8_t>(p, i)); return true;
    case DataType::Int16: lua_pushnumber(L, ReadAt<int16_t>(p, i)); return true;
    case DataType::UInt16: lua_pushnumber(L, ReadAt<uint16_t>(p, i)); return true;
    case DataType::Int32: lua_pushnumber(L, ReadAt<int32_t>(p, i)); return true;
    case DataType::UInt32: lua_pushnumber(L, ReadAt<uint32_t>(p, i)); return true;
    case DataType::Int64: lua_pushnumber(L, static_cast<lua_Number>(ReadAt<int64_t>(p, i))); return true;
    case DataType::UInt64: lua_pushnumber(L, static_cast<lua_Number>(ReadAt<uint64_t>(p, i))); return true;
    case DataType::Float: lua_pushnumber(L, ReadAt<float>(p, i)); return true;
    case DataType::Double: lua_pushnumber(L, ReadAt<double>(p, i)); return true;
    case DataType::Bool: lua_pushboolean(L, ReadAt<uint8_t>(p, i) != 0); return true;
    case DataType::String:
    case DataType::Path:
    case DataType::FilePtr: {
        const std::string text = file.ElementString(p, i);
        lua_pushlstring(L, text.data(), text.size());
        return true;
    }
    case DataType::EntityPtr:
    case DataType::EntityHandle:
    case DataType::EntityLink: {
        const fox2::Entity* target = file.ElementEntity(p, i);
        if (!target) {
            return false;
        }
        PushEntity(L, stage, target, false);
        return true;
    }
    case DataType::Vector3:
    case DataType::WideVector3: {
        const float* f = reinterpret_cast<const float*>(p.Element(i));
        PushVector3(L, glm::vec3(f[0], f[1], f[2]));
        return true;
    }
    case DataType::Vector4:
    case DataType::Color: {
        const float* f = reinterpret_cast<const float*>(p.Element(i));
        lua_createtable(L, 0, 4);
        lua_pushnumber(L, f[0]);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, f[1]);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, f[2]);
        lua_setfield(L, -2, "z");
        lua_pushnumber(L, f[3]);
        lua_setfield(L, -2, "w");
        return true;
    }
    case DataType::Quat: {
        const float* f = reinterpret_cast<const float*>(p.Element(i));
        PushQuat(L, glm::quat(f[3], f[0], f[1], f[2]));
        return true;
    }
    default:
        return false;
    }
}

std::string BodyClassName(const fox2::Entity& entity) {
    return entity.class_name + "Body";
}

int EntityGetDataBody(lua_State* L) {
    Resolved r = Resolve(L, 1);
    if (!r.stage) {
        lua_pushnil(L);
        return 1;
    }
    PushEntity(L, *r.stage, r.ref->entity, true);
    return 1;
}

int EntityGetChildren(lua_State* L) {
    Resolved r = Resolve(L, 1);
    lua_newtable(L);
    if (!r.file) {
        return 1;
    }
    const fox2::DataSetFile& file = *r.file->file;
    int n = 0;
    for (const char* name : {"children", "members"}) {
        const fox2::Property* p = file.FindProperty(*r.ref->entity, name);
        if (!p) {
            continue;
        }
        for (size_t i = 0; i < p->Count(); ++i) {
            const fox2::Entity* child = file.ElementEntity(*p, i);
            if (!child) {
                continue;
            }
            const std::string key = file.EntityName(*child);
            PushEntity(L, *r.stage, child, false);
            if (key.empty()) {
                lua_rawseti(L, -2, ++n);
            } else {
                lua_setfield(L, -2, key.c_str());
            }
        }
        break;
    }
    return 1;
}

int EntityIsKindOf(lua_State* L) {
    const EntityRef* ref = ToEntity(L, 1);
    const std::string_view name = luaL_checkstring(L, 2);
    bool result = false;
    if (ref && ref->entity) {
        const std::string& cls = ref->entity->class_name;
        result = name == cls || (ref->body && name == BodyClassName(*ref->entity)) || name == "Entity" || name == "Data" ||
                 (ref->body && name == "DataBody");
        if (!result && name == "Light") {
            result = cls == "PointLight" || cls == "SpotLight";
        }
    }
    lua_pushboolean(L, result);
    return 1;
}

glm::mat4 EntityWorld(const Resolved& r) {
    return r.stage->ToWorld(r.file->file->WorldTransform(*r.ref->entity));
}

int EntityGetWorldTransform(lua_State* L) {
    Resolved r = Resolve(L, 1);
    if (!r.file) {
        lua_pushnil(L);
        return 1;
    }
    PushTransform(L, EntityWorld(r));
    return 1;
}

void SetBodyField(lua_State* L, const Resolved& r, BodyField field, bool value) {
    BodyState& body = r.stage->Body(r.ref->entity);
    bool* slot = field == BodyField::Enable ? &body.enable : field == BodyField::Visible ? &body.visible : &body.geom_active;
    if (*slot == value) {
        return;
    }
    *slot = value;
    if (EntityHost* host = Host(L)) {
        host->OnBodyChanged(*r.stage, *r.ref->entity, field);
    }
}

int EntityVisible(lua_State* L) {
    Resolved r = Resolve(L, 1);
    if (r.file) {
        SetBodyField(L, r, BodyField::Visible, true);
    }
    return 0;
}

int EntityInvisible(lua_State* L) {
    Resolved r = Resolve(L, 1);
    if (r.file) {
        SetBodyField(L, r, BodyField::Visible, false);
    }
    return 0;
}

int EntitySetupMessageBox(lua_State* L) {
    Resolved r = Resolve(L, 1);
    if (r.file) {
        if (EntityHost* host = Host(L)) {
            host->OnSetupMessageBox(*r.stage, *r.ref->entity);
        }
    }
    return 0;
}

int EntityGetClassName(lua_State* L) {
    const EntityRef* ref = ToEntity(L, 1);
    if (!ref || !ref->entity) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, ref->body ? BodyClassName(*ref->entity).c_str() : ref->entity->class_name.c_str());
    return 1;
}

int EntityIndex(lua_State* L) {
    const std::string_view key = luaL_checkstring(L, 2);
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "methods");
    lua_getfield(L, -1, key.data());
    if (!lua_isnil(L, -1)) {
        return 1;
    }
    lua_pop(L, 3);
    Resolved r = Resolve(L, 1);
    if (!r.file) {
        lua_pushnil(L);
        return 1;
    }
    const fox2::DataSetFile& file = *r.file->file;
    const fox2::Entity& entity = *r.ref->entity;
    if (key == "name") {
        const std::string name = file.EntityName(entity);
        lua_pushlstring(L, name.data(), name.size());
        return 1;
    }
    if (key == "worldTransform") {
        PushTransform(L, EntityWorld(r));
        return 1;
    }
    if (r.ref->body) {
        const BodyState& body = r.stage->Body(&entity);
        if (key == "enable") {
            lua_pushboolean(L, body.enable);
            return 1;
        }
        if (key == "isVisible") {
            lua_pushboolean(L, body.visible);
            return 1;
        }
        if (key == "isGeomActive") {
            lua_pushboolean(L, body.geom_active);
            return 1;
        }
    }
    const fox2::Property* p = file.FindProperty(entity, key);
    if (!p || !PushProperty(L, *r.stage, file, *p)) {
        lua_pushnil(L);
    }
    return 1;
}

int EntityNewIndex(lua_State* L) {
    const std::string_view key = luaL_checkstring(L, 2);
    Resolved r = Resolve(L, 1);
    if (!r.file) {
        return 0;
    }
    const bool value = lua_toboolean(L, 3) != 0;
    if (key == "enable") {
        SetBodyField(L, r, BodyField::Enable, value);
    } else if (key == "isVisible") {
        SetBodyField(L, r, BodyField::Visible, value);
    } else if (key == "isGeomActive") {
        SetBodyField(L, r, BodyField::GeomActive, value);
    } else {
        LogDebug("lua: ignored write {}.{}", r.ref->entity->class_name, key);
    }
    return 0;
}

int EntityEq(lua_State* L) {
    const EntityRef* a = ToEntity(L, 1);
    const EntityRef* b = ToEntity(L, 2);
    lua_pushboolean(L, a && b && a->stage_id == b->stage_id && a->entity == b->entity && a->body == b->body);
    return 1;
}

int EntityToString(lua_State* L) {
    Resolved r = Resolve(L, 1);
    if (!r.file) {
        lua_pushstring(L, "Entity(null)");
        return 1;
    }
    const std::string text = std::format("{}({})", r.ref->body ? BodyClassName(*r.ref->entity) : r.ref->entity->class_name,
                                         r.file->file->EntityName(*r.ref->entity));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int EntityIsNullFn(lua_State* L) {
    lua_pushboolean(L, IsNullEntity(L, 1));
    return 1;
}

int VectorToString(lua_State* L) {
    lua_getfield(L, 1, "x");
    lua_getfield(L, 1, "y");
    lua_getfield(L, 1, "z");
    const std::string text = std::format("Vector3({:.4f}, {:.4f}, {:.4f})", lua_tonumber(L, -3), lua_tonumber(L, -2), lua_tonumber(L, -1));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int QuatToString(lua_State* L) {
    lua_getfield(L, 1, "x");
    lua_getfield(L, 1, "y");
    lua_getfield(L, 1, "z");
    lua_getfield(L, 1, "w");
    const std::string text =
        std::format("Quat({:.4f}, {:.4f}, {:.4f}, {:.4f})", lua_tonumber(L, -4), lua_tonumber(L, -3), lua_tonumber(L, -2), lua_tonumber(L, -1));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int VectorCall(lua_State* L) {
    PushVector3(L, glm::vec3(static_cast<float>(luaL_optnumber(L, 2, 0.0)), static_cast<float>(luaL_optnumber(L, 3, 0.0)),
                             static_cast<float>(luaL_optnumber(L, 4, 0.0))));
    return 1;
}

int QuatCall(lua_State* L) {
    PushQuat(L, glm::quat(static_cast<float>(luaL_optnumber(L, 5, 1.0)), static_cast<float>(luaL_optnumber(L, 2, 0.0)),
                          static_cast<float>(luaL_optnumber(L, 3, 0.0)), static_cast<float>(luaL_optnumber(L, 4, 0.0))));
    return 1;
}

int VectorGetX(lua_State* L) {
    lua_getfield(L, 1, "x");
    return 1;
}

int VectorGetY(lua_State* L) {
    lua_getfield(L, 1, "y");
    return 1;
}

int VectorGetZ(lua_State* L) {
    lua_getfield(L, 1, "z");
    return 1;
}

void MakeCallableClass(lua_State* L, const char* global, const char* meta, lua_CFunction call, lua_CFunction to_string) {
    luaL_newmetatable(L, meta);
    lua_pushcfunction(L, to_string);
    lua_setfield(L, -2, "__tostring");
    lua_newtable(L);
    lua_pushcfunction(L, VectorGetX);
    lua_setfield(L, -2, "GetX");
    lua_pushcfunction(L, VectorGetY);
    lua_setfield(L, -2, "GetY");
    lua_pushcfunction(L, VectorGetZ);
    lua_setfield(L, -2, "GetZ");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, call);
    lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);
    lua_setglobal(L, global);
}

}

void RegisterEntityApi(lua_State* L, EntityHost* host) {
    lua_pushlightuserdata(L, host);
    lua_setfield(L, LUA_REGISTRYINDEX, kHostKey);

    luaL_newmetatable(L, kEntityMeta);
    lua_newtable(L);
    const luaL_Reg methods[] = {
        {"GetDataBodyWithReferrer", EntityGetDataBody},
        {"GetDataBody", EntityGetDataBody},
        {"GetChildren", EntityGetChildren},
        {"IsKindOf", EntityIsKindOf},
        {"GetWorldTransform", EntityGetWorldTransform},
        {"Visible", EntityVisible},
        {"Invisible", EntityInvisible},
        {"SetupMessageBox", EntitySetupMessageBox},
        {"GetClassName", EntityGetClassName},
        {nullptr, nullptr},
    };
    for (const luaL_Reg* m = methods; m->name; ++m) {
        lua_pushcfunction(L, m->func);
        lua_setfield(L, -2, m->name);
    }
    lua_setfield(L, -2, "methods");
    lua_pushcfunction(L, EntityIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, EntityNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, EntityEq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, EntityToString);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    lua_getglobal(L, "Entity");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "Entity");
    }
    lua_pushcfunction(L, EntityIsNullFn);
    lua_setfield(L, -2, "IsNull");
    lua_pop(L, 1);

    MakeCallableClass(L, "Vector3", kVector3Meta, VectorCall, VectorToString);
    MakeCallableClass(L, "Quat", kQuatMeta, QuatCall, QuatToString);
}

void PushEntity(lua_State* L, const Stage& stage, const fox2::Entity* entity, bool body) {
    if (!entity) {
        lua_pushnil(L);
        return;
    }
    auto* ref = static_cast<EntityRef*>(lua_newuserdata(L, sizeof(EntityRef)));
    ref->stage_id = stage.id;
    ref->entity = entity;
    ref->body = body;
    luaL_getmetatable(L, kEntityMeta);
    lua_setmetatable(L, -2);
}

const EntityRef* ToEntity(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TUSERDATA || !lua_getmetatable(L, index)) {
        return nullptr;
    }
    luaL_getmetatable(L, kEntityMeta);
    const bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return same ? static_cast<const EntityRef*>(lua_touserdata(L, index)) : nullptr;
}

bool IsNullEntity(lua_State* L, int index) {
    const EntityRef* ref = ToEntity(L, index);
    if (!ref || !ref->entity) {
        return true;
    }
    EntityHost* host = Host(L);
    return !host || !host->FindStage(ref->stage_id);
}

void PushVector3(lua_State* L, const glm::vec3& v) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z);
    lua_setfield(L, -2, "z");
    luaL_getmetatable(L, kVector3Meta);
    lua_setmetatable(L, -2);
}

void PushQuat(lua_State* L, const glm::quat& q) {
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, q.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, q.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, q.z);
    lua_setfield(L, -2, "z");
    lua_pushnumber(L, q.w);
    lua_setfield(L, -2, "w");
    luaL_getmetatable(L, kQuatMeta);
    lua_setmetatable(L, -2);
}

bool ReadVector3(lua_State* L, int index, glm::vec3& out) {
    if (!lua_istable(L, index)) {
        return false;
    }
    index = index < 0 ? lua_gettop(L) + index + 1 : index;
    lua_getfield(L, index, "x");
    lua_getfield(L, index, "y");
    lua_getfield(L, index, "z");
    const bool ok = lua_isnumber(L, -3) && lua_isnumber(L, -2) && lua_isnumber(L, -1);
    if (ok) {
        out = glm::vec3(static_cast<float>(lua_tonumber(L, -3)), static_cast<float>(lua_tonumber(L, -2)), static_cast<float>(lua_tonumber(L, -1)));
    }
    lua_pop(L, 3);
    return ok;
}

bool PushProperty(lua_State* L, const Stage& stage, const fox2::DataSetFile& file, const fox2::Property& p) {
    const bool scalar = p.container == fox2::Container::StaticArray && p.Count() == 1;
    if (scalar) {
        return PushElement(L, stage, file, p, 0);
    }
    lua_createtable(L, static_cast<int>(p.Count()), 0);
    int n = 0;
    for (size_t i = 0; i < p.Count(); ++i) {
        if (!PushElement(L, stage, file, p, i)) {
            continue;
        }
        if (p.container == fox2::Container::StringMap) {
            const std::string key = file.KeyString(p, i);
            lua_setfield(L, -2, key.c_str());
        } else {
            lua_rawseti(L, -2, ++n);
        }
    }
    return true;
}

void PushTransform(lua_State* L, const glm::mat4& world) {
    glm::vec3 scale;
    glm::quat rotation;
    glm::vec3 translation;
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::decompose(world, scale, rotation, translation, skew, perspective);
    lua_createtable(L, 0, 3);
    PushVector3(L, translation);
    lua_setfield(L, -2, "translation");
    PushQuat(L, rotation);
    lua_setfield(L, -2, "rotQuat");
    PushVector3(L, scale);
    lua_setfield(L, -2, "scale");
}

}
