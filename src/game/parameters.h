#pragma once

#include <array>
#include <map>
#include <string>

#include "engine/script/lua_vm.h"

namespace pt::game {

struct SpeedRange {
    float min = 0.5f;
    float max = 1.0f;
};

struct PlayerParameters {
    SpeedRange front{0.50f, 1.00f};
    SpeedRange left{0.40f, 0.75f};
    SpeedRange right{0.40f, 0.75f};
    SpeedRange back{0.25f, 0.50f};
    float focal_length = 13.0f;
    float rot_vel_max_x = 87.0f * 1.25f;
    float rot_vel_max_y = 87.0f * 1.25f;
    float rot_interp_half_life = 2.0f / 60.0f;
};

struct HandyLightParameters {
    float inner_range = 0.005f;
    float outer_range = 5.0f;
    float color[3] = {1.0f, 1.0f, 1.0f};
    float temperature = 5000.0f;
    float lumen = 100.0f;
    float light_size = 0.05f;
    float umbra_angle = 78.0f;
    float penumbra_angle = 30.0f;
    float attenuation_exponent = 1.2f;
    float dimmer = 0.1f;
    float power_scale = 1.0f;
};

struct LightingRow {
    float min_exposure = -10.0f;
    float max_exposure = 1.0f;
    float exposure_compensation = 0.0f;
    float key_value = 1.0f;
    float add_exp_comp[3] = {-4.8f, -3.8f, -2.4f};
    float add_exp_comp_ev[3] = {0.0f, -3.0f, -5.0f};
    float bloom_weight = 1.2f;
    float bloom_brightness_extraction = 3.0f;
    float bloom_size = 2.0f;
    float shutter_speed = 0.035f;
};

struct GameParameters {
    PlayerParameters player;
    HandyLightParameters handy_light;
    std::map<std::string, LightingRow> lighting;
    bool loaded = false;

    const LightingRow* Lighting(int row) const;
    bool LoadFromLua(lua_State* L, int index);
};

}
