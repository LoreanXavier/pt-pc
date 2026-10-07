#include "game/demo_stream.h"

#include "engine/anim/anim_codec.h"
#include "engine/audio/sound_package.h"
#include "engine/data/fox2.h"

namespace pt::game {
namespace {

std::map<std::string, std::string> StringMap(const fox2::DataSetFile& f, const fox2::Entity& e, std::string_view prop) {
    std::map<std::string, std::string> out;
    if (const fox2::Property* p = f.FindProperty(e, prop)) {
        for (size_t i = 0; i < p->Count(); ++i) {
            out[f.KeyString(*p, i)] = f.ElementString(*p, i);
        }
    }
    return out;
}

}

bool DemoStreamData::Load(std::vector<uint8_t> bytes, std::string* error) {
    if (auto audio = audio::ExtractDemoStreamAudio(bytes)) {
        sound_wem = std::move(audio->wem);
    }
    if (!file.Parse(std::move(bytes), error)) {
        return false;
    }
    length_frames = file.Length();
    tracks.assign(file.Tracks().size(), anim::DecodedTrack{});
    for (size_t i = 0; i < file.Tracks().size(); ++i) {
        if (file.Tracks()[i].valid) {
            file.DecodeTrack(i, length_frames, tracks[i]);
        }
    }
    events = file.FirstLoopEvents();
    for (const anim::StreamEvent* e : events) {
        has_camera = has_camera || e->type == anim::kEventCreateCamera;
    }
    camera_actor = file.FindActor(anim::ActorKind::Camera);
    camera_param_actor = file.FindActor(anim::ActorKind::CameraParam);
    return true;
}

bool DemoStreamData::Sample(int track, double frame, glm::vec4& out) const {
    if (track < 0 || static_cast<size_t>(track) >= tracks.size()) {
        return false;
    }
    return tracks[static_cast<size_t>(track)].Sample(frame, out);
}

bool DemoStreamData::SampleTransform(size_t actor, double frame, glm::quat& rotation, glm::vec3& translation) const {
    if (actor >= file.Actors().size() || file.Actors()[actor].units.empty()) {
        return false;
    }
    const anim::ActorUnit& unit = file.Actors()[actor].units[0];
    glm::vec4 v;
    bool any = false;
    if (Sample(unit.rotation, frame, v)) {
        rotation = anim::ToQuat(v);
        any = true;
    }
    if (Sample(unit.translation, frame, v)) {
        translation = glm::vec3(v);
        any = true;
    }
    return any;
}

const DemoControlCharacter* DemoInfo::ControlCharacter(std::string_view model) const {
    for (const DemoControlCharacter& c : control_characters) {
        if (c.model == model) {
            return &c;
        }
    }
    return nullptr;
}

bool DemoInfo::ControlsPlayer() const {
    for (const DemoControlCharacter& c : control_characters) {
        if (c.character_id == "Player") {
            return true;
        }
    }
    return false;
}

bool ReadDemoInfo(const fox2::DataSetFile& f, const fox2::Entity& e, DemoInfo& out) {
    out.demo_id = f.GetString(e, "demoId");
    if (out.demo_id.empty()) {
        return false;
    }
    out.stream_path = f.GetString(e, "demoStreamPath");
    out.motion_path = f.GetString(e, "motionPath");
    out.audio_path = f.GetString(e, "audioPath");
    out.length_frames = f.GetInt(e, "demoLength");
    out.on_memory = f.GetBool(e, "onMemory", 0, true);
    out.camera_interp_type = f.GetInt(e, "cameraInterpType");
    out.camera_interp_frames = f.GetInt(e, "cameraInterpFrame");
    out.camera_interp_curve_rate = f.GetFloat(e, "cameraInterpCurveRate", 0, 1.0f);
    out.camera_interp_scurve_center = f.GetFloat(e, "cameraInterpScurveCenter", 0, 0.5f);
    out.camera_end_translation = glm::vec3(f.GetVec4(e, "cameraTranslation"));
    out.camera_end_rotation = f.GetQuat(e, "cameraRotation");
    out.camera_end_focal = f.GetFloat(e, "cameraParam", 0, 13.0f);
    out.camera_start_translation = glm::vec3(f.GetVec4(e, "cameraStartTranslation"));
    out.camera_start_rotation = f.GetQuat(e, "cameraStartRotation");
    out.camera_start_focal = f.GetFloat(e, "cameraStartParam", 0, 13.0f);
    if (const fox2::Entity* t = f.GetEntity(e, "transform")) {
        out.transform_rotation = f.GetQuat(*t, "transform_rotation_quat");
        out.transform_translation = glm::vec3(f.GetVec4(*t, "transform_translation"));
    }
    for (const auto& [model, desc] : f.GetEntityMap(e, "controlCharacters")) {
        if (!desc) {
            continue;
        }
        DemoControlCharacter c;
        c.model = model;
        c.character_id = f.GetString(*desc, "characterId");
        c.translation = glm::vec3(f.GetVec4(*desc, "translation"));
        c.rotation = f.GetQuat(*desc, "rotation");
        c.start_translation = glm::vec3(f.GetVec4(*desc, "startTranslation"));
        c.start_rotation = f.GetQuat(*desc, "startRotation");
        c.controlled_at_start = f.GetBool(*desc, "controlledAtStart", 0, true);
        out.control_characters.push_back(std::move(c));
    }
    for (const auto& [name, node] : f.GetEntityMap(e, "entityParams")) {
        if (node) {
            for (const auto& [model, part] : StringMap(f, *node, "partNames")) {
                out.part_names[model] = part;
            }
        }
    }
    const fox2::Entity* animation = f.GetEntity(e, "streamAnimation");
    if (animation) {
        if (const fox2::Property* p = f.FindProperty(*animation, "locatorTypes")) {
            for (size_t i = 0; i < p->Count(); ++i) {
                out.locator_types[f.KeyString(*p, i)] = f.GetInt(*animation, "locatorTypes", i);
            }
        }
        out.model_files = StringMap(f, *animation, "modelFiles");
        out.help_bone_files = StringMap(f, *animation, "helpBoneFiles");
        out.parts_files = StringMap(f, *animation, "partsFiles");
        out.model_parts = StringMap(f, *animation, "modelPartsDictionary");
    }
    out.setup_lights = StringMap(f, e, "setupLights");
    out.file_params = StringMap(f, e, "fileParams");
    if (const fox2::Property* clips = f.FindProperty(e, "clipDatas")) {
        for (size_t i = 0; i < clips->Count(); ++i) {
            if (const fox2::Entity* c = f.ElementEntity(*clips, i)) {
                out.clips.push_back({f.GetString(*c, "name"), f.GetString(*c, "cameraName"), f.GetInt(*c, "startFrame"), f.GetInt(*c, "endFrame"),
                                     f.GetInt(*c, "offsetFrame")});
            }
        }
    }
    return true;
}

}
