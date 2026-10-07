#include "game/parameters.h"

#include "engine/core/log.h"

namespace pt::game {
namespace {

constexpr const char* kLightingOrder[] = {"f000", "f005", "f010", "f020", "f030", "f040", "f050", "f060",
                                          "f070", "f080", "f090", "f100", "f110", "f120", "f160"};

float Field(lua_State* L, int table, const char* key, float fallback) {
    lua_getfield(L, table, key);
    const float value = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return value;
}

bool Child(lua_State* L, int table, const char* key) {
    lua_getfield(L, table, key);
    if (lua_istable(L, -1)) {
        return true;
    }
    lua_pop(L, 1);
    return false;
}

void ReadRange(lua_State* L, int table, const char* key, SpeedRange& range) {
    if (Child(L, table, key)) {
        const int t = lua_gettop(L);
        range.min = Field(L, t, "min", range.min);
        range.max = Field(L, t, "max", range.max);
        lua_pop(L, 1);
    }
}

}

const LightingRow* GameParameters::Lighting(int row) const {
    if (row < 0 || row >= static_cast<int>(std::size(kLightingOrder))) {
        return nullptr;
    }
    auto it = lighting.find(kLightingOrder[row]);
    return it == lighting.end() ? nullptr : &it->second;
}

bool GameParameters::LoadFromLua(lua_State* L, int index) {
    if (!lua_istable(L, index)) {
        return false;
    }
    const int root = index < 0 ? lua_gettop(L) + index + 1 : index;
    if (Child(L, root, "playerParameter")) {
        const int pp = lua_gettop(L);
        if (Child(L, pp, "action")) {
            const int action = lua_gettop(L);
            if (Child(L, action, "moveSpeedRate")) {
                const int rate = lua_gettop(L);
                ReadRange(L, rate, "front", player.front);
                ReadRange(L, rate, "left", player.left);
                ReadRange(L, rate, "right", player.right);
                ReadRange(L, rate, "back", player.back);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        if (Child(L, pp, "camera")) {
            const int camera = lua_gettop(L);
            player.focal_length = Field(L, camera, "focalLength", player.focal_length);
            player.rot_vel_max_x = Field(L, camera, "rotVelMaxX", player.rot_vel_max_x);
            player.rot_vel_max_y = Field(L, camera, "rotVelMaxY", player.rot_vel_max_y);
            player.rot_interp_half_life = Field(L, camera, "rotInterpHalfLife", player.rot_interp_half_life);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    if (Child(L, root, "handyLightParameter")) {
        const int h = lua_gettop(L);
        HandyLightParameters& p = handy_light;
        p.inner_range = Field(L, h, "innerRange", p.inner_range);
        p.outer_range = Field(L, h, "outerRange", p.outer_range);
        p.temperature = Field(L, h, "temperature", p.temperature);
        p.lumen = Field(L, h, "lumen", p.lumen);
        p.light_size = Field(L, h, "lightSize", p.light_size);
        p.umbra_angle = Field(L, h, "umbraAngle", p.umbra_angle);
        p.penumbra_angle = Field(L, h, "penumbraAngle", p.penumbra_angle);
        p.attenuation_exponent = Field(L, h, "attenuationExponent", p.attenuation_exponent);
        p.dimmer = Field(L, h, "dimmer", p.dimmer);
        p.power_scale = Field(L, h, "powerScale", p.power_scale);
        if (Child(L, h, "color")) {
            const int c = lua_gettop(L);
            p.color[0] = Field(L, c, "r", 1.0f);
            p.color[1] = Field(L, c, "g", 1.0f);
            p.color[2] = Field(L, c, "b", 1.0f);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    if (Child(L, root, "lightingParameter")) {
        const int lp = lua_gettop(L);
        for (const char* name : kLightingOrder) {
            if (!Child(L, lp, name)) {
                continue;
            }
            const int r = lua_gettop(L);
            LightingRow row;
            row.min_exposure = Field(L, r, "minExposure", row.min_exposure);
            row.max_exposure = Field(L, r, "maxExposure", row.max_exposure);
            row.exposure_compensation = Field(L, r, "exposureCompensation", row.exposure_compensation);
            row.key_value = Field(L, r, "keyValue", row.key_value);
            row.add_exp_comp[0] = Field(L, r, "addExpComp0", row.add_exp_comp[0]);
            row.add_exp_comp[1] = Field(L, r, "addExpComp1", row.add_exp_comp[1]);
            row.add_exp_comp[2] = Field(L, r, "addExpComp2", row.add_exp_comp[2]);
            row.add_exp_comp_ev[0] = Field(L, r, "addExpComp_Ev0", row.add_exp_comp_ev[0]);
            row.add_exp_comp_ev[1] = Field(L, r, "addExpComp_Ev1", row.add_exp_comp_ev[1]);
            row.add_exp_comp_ev[2] = Field(L, r, "addExpComp_Ev2", row.add_exp_comp_ev[2]);
            row.bloom_weight = Field(L, r, "bloomWeight", row.bloom_weight);
            row.bloom_brightness_extraction = Field(L, r, "bloomBrightnessExtraction", row.bloom_brightness_extraction);
            row.bloom_size = Field(L, r, "bloomSize", row.bloom_size);
            row.shutter_speed = Field(L, r, "shutterSpeed", row.shutter_speed);
            lighting[name] = row;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    loaded = true;
    LogInfo("parameters: player focal {} rot {} half life {:.4f}, {} lighting rows", player.focal_length, player.rot_vel_max_x,
            player.rot_interp_half_life, lighting.size());
    return true;
}

}
