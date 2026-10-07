#include "game/stage_data.h"

#include "engine/core/log.h"

namespace pt::game {
namespace {

std::vector<std::string> StringArray(const fox2::DataSetFile& file, const fox2::Entity& e, std::string_view prop) {
    std::vector<std::string> out;
    const fox2::Property* p = e.Find(prop);
    if (p) {
        for (size_t i = 0; i < p->Count(); ++i) {
            out.push_back(file.GetString(e, prop, i));
        }
    }
    return out;
}

std::vector<TrapCallback> Callbacks(const fox2::DataSetFile& file, const fox2::Entity& e, std::string_view prop) {
    std::vector<TrapCallback> out;
    const fox2::Property* p = e.Find(prop);
    if (!p) {
        return out;
    }
    for (size_t i = 0; i < p->Count(); ++i) {
        const fox2::Entity* callback = file.GetEntity(e, prop, i);
        out.push_back({callback ? callback->class_name : std::string(), callback});
    }
    return out;
}

TrapCondition BuildCondition(const fox2::DataSetFile& file, const fox2::Entity& e) {
    TrapCondition c;
    c.name = file.EntityName(e);
    c.entity = &e;
    c.enable = file.GetBool(e, "enable", 0, true);
    c.is_once = file.GetBool(e, "isOnce");
    c.is_and_check = file.GetBool(e, "isAndCheck");
    c.check_names = StringArray(file, e, "checkFuncNames");
    c.exec_names = StringArray(file, e, "execFuncNames");
    c.checks = Callbacks(file, e, "checkCallbackDataElements");
    c.execs = Callbacks(file, e, "execCallbackDataElements");
    return c;
}

}

std::unique_ptr<StageData> BuildStageData(std::shared_ptr<fox2::DataSetFile> file, std::string package_path) {
    auto stage = std::make_unique<StageData>();
    stage->package_path = std::move(package_path);
    stage->file = file;
    const fox2::DataSetFile& f = *file;
    for (const fox2::Entity& e : f.Entities()) {
        const std::string& cls = e.class_name;
        if (cls == "ShRelativeStageLocator") {
            stage->root = f.WorldTransform(e);
            stage->has_root = true;
            for (const auto& [key, target] : f.GetEntityMap(e, "connectors")) {
                if (target) {
                    stage->connectors[key] = f.WorldTransform(*target);
                }
            }
        } else if (cls == "StaticModel") {
            StaticModelPlacement m;
            m.name = f.EntityName(e);
            m.entity = &e;
            m.model_file = f.GetString(e, "modelFile");
            m.geom_file = f.GetString(e, "geomFile");
            m.world = f.WorldTransform(e);
            m.color = f.GetVec4(e, "color");
            m.visible_geom = f.GetBool(e, "isVisibleGeom");
            m.draw_rejection_level = f.GetInt(e, "drawRejectionLevel");
            stage->static_models.push_back(std::move(m));
        } else if (cls == "PointLight" || cls == "SpotLight") {
            LightPlacement l;
            l.name = f.EntityName(e);
            l.entity = &e;
            l.class_name = cls;
            l.world = f.WorldTransform(e);
            l.enable = f.GetBool(e, "enable", 0, true);
            l.color = f.GetVec4(e, "color");
            l.temperature = f.GetFloat(e, "temperature", 0, 6500.0f);
            l.lumen = f.GetFloat(e, "lumen");
            l.inner_range = f.GetFloat(e, "innerRange");
            l.outer_range = f.GetFloat(e, "outerRange");
            l.light_size = f.GetFloat(e, "lightSize");
            l.dimmer = f.GetFloat(e, "dimmer", 0, 1.0f);
            l.umbra_angle = f.GetFloat(e, "umbraAngle");
            l.penumbra_angle = f.GetFloat(e, "penumbraAngle");
            l.attenuation_exponent = f.GetFloat(e, "attenuationExponent", 0, 1.0f);
            l.cast_shadow = f.GetBool(e, "castShadow");
            l.has_specular = f.GetBool(e, "hasSpecular", 0, true);
            stage->lights.push_back(std::move(l));
        } else if (cls == "GeoTrap") {
            TrapPlacement t;
            t.name = f.EntityName(e);
            t.entity = &e;
            t.enable = f.GetBool(e, "enable", 0, true);
            const fox2::Property* children = e.Find("children");
            for (size_t i = 0; children && i < children->Count(); ++i) {
                const fox2::Entity* child = f.GetEntity(e, "children", i);
                if (child && child->class_name == "BoxShape") {
                    t.boxes.push_back(f.WorldTransform(*child));
                }
            }
            const fox2::Property* conditions = e.Find("conditionArray");
            for (size_t i = 0; conditions && i < conditions->Count(); ++i) {
                if (const fox2::Entity* c = f.GetEntity(e, "conditionArray", i)) {
                    t.conditions.push_back(BuildCondition(f, *c));
                }
            }
            stage->traps.push_back(std::move(t));
        } else if (cls == "Locator") {
            stage->locators.push_back({f.EntityName(e), cls, f.WorldTransform(e), &e});
        } else if (cls == "GameObjectLocator") {
            stage->game_objects.push_back({f.EntityName(e), f.GetString(e, "typeName"), f.WorldTransform(e), &e});
        } else if (cls == "ShDemoScript" || cls == "ShGameControllerMessageScript") {
            MessageScript s;
            s.name = f.EntityName(e);
            s.class_name = cls;
            s.entity = &e;
            s.enable = f.GetBool(e, "enable", 0, true);
            s.demo_id = f.GetString(e, "demoId");
            s.message_name = f.GetString(e, "messageName");
            s.order_floor = f.GetBool(e, "orderFloor");
            s.floor_names = StringArray(f, e, "floorNames");
            s.script_file = f.GetString(e, "scriptFile");
            stage->message_scripts.push_back(std::move(s));
        } else if (cls == "SoundSource") {
            stage->sound_sources.push_back({f.EntityName(e), &e, f.GetString(e, "eventName"), f.WorldTransform(e), f.GetFloat(e, "playRange")});
        }
    }
    return stage;
}

void LogStageData(const StageData& stage) {
    LogInfo("stage {}: {} models, {} lights, {} traps, {} locators, {} game objects, {} message scripts, {} sound sources, {} connectors",
            stage.package_path, stage.static_models.size(), stage.lights.size(), stage.traps.size(), stage.locators.size(),
            stage.game_objects.size(), stage.message_scripts.size(), stage.sound_sources.size(), stage.connectors.size());
}

}
