#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>

#include "engine/data/fox2.h"
#include "engine/script/lua_vm.h"

namespace pt::game {

struct Stage;

enum class BodyField { Enable, Visible, GeomActive };

class EntityHost {
public:
    virtual ~EntityHost() = default;
    virtual Stage* FindStage(uint32_t stage_id) = 0;
    virtual void OnBodyChanged(Stage& stage, const fox2::Entity& entity, BodyField field) = 0;
    virtual void OnSetupMessageBox(Stage& stage, const fox2::Entity& entity) = 0;
};

struct EntityRef {
    uint32_t stage_id = 0;
    const fox2::Entity* entity = nullptr;
    bool body = false;
};

void RegisterEntityApi(lua_State* L, EntityHost* host);
void PushEntity(lua_State* L, const Stage& stage, const fox2::Entity* entity, bool body);
const EntityRef* ToEntity(lua_State* L, int index);
bool IsNullEntity(lua_State* L, int index);

void PushVector3(lua_State* L, const glm::vec3& v);
void PushQuat(lua_State* L, const glm::quat& q);
bool ReadVector3(lua_State* L, int index, glm::vec3& out);

bool PushProperty(lua_State* L, const Stage& stage, const fox2::DataSetFile& file, const fox2::Property& p);
void PushTransform(lua_State* L, const glm::mat4& world);

}
