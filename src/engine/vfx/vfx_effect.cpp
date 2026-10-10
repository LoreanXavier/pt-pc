#include "engine/vfx/vfx_effect.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "engine/core/log.h"

namespace pt::vfx {

namespace {

constexpr float kDegToRad = 0.017453292f;

constexpr uint32_t kHashSeparateDraw = 0x94390DB1;
constexpr uint32_t kHashScreenHeight = 0x91365199;
constexpr uint32_t kHashScreenWidth = 0x32FF2253;
constexpr uint32_t kHashBlendMode2D = 0xDE02160D;
constexpr uint32_t kHashUvRect = 0x4296121B;
constexpr uint32_t kHashDivisionW = 0x1D121378;
constexpr uint32_t kHashDivisionH = 0x9EC5A541;
constexpr uint32_t kHashFlipU = 0xAEADF7F1;
constexpr uint32_t kHashFlipV = 0x902AA0EB;
constexpr uint32_t kHashAnimationFrame = 0xEBAF6352;
constexpr uint32_t kHashCorrectionType = 0xE210591C;
constexpr uint32_t kHashCorrectionOffset = 0xBA7C713E;
constexpr uint32_t kHashFollowOffset = 0x73D22AD2;
constexpr uint32_t kHashFollowYaw = 0x849D3E0C;
constexpr uint32_t kHashLumAtMin = 0xAB96FA51;
constexpr uint32_t kHashLumAtMax = 0xFC08FF2F;
constexpr uint32_t kHashLiquidCube = 0x23E90E5F;
constexpr uint32_t kHashLiquidRefraction = 0xF6B16443;
constexpr uint32_t kHashLiquidFresnelBase = 0xBD8530BE;
constexpr uint32_t kHashLiquidFresnelPower = 0x3B831F9F;
constexpr uint32_t kHashLiquidReflection = 0x62133294;
constexpr uint32_t kHashLightMask = 0xA07B1E26;
constexpr uint32_t kHashLightAreaRotation = 0x733D3780;
constexpr const char* kDefaultLiquidCube = "/Assets/sh/effect/gr_pic/gr_cub01_ks_cbm_nmp.ftex";
constexpr uint32_t kHashRandomLifeMin = 0x9B076750;
constexpr uint32_t kHashRandomLifeMax = 0x8F88FA93;
constexpr uint32_t kHashSpreadRange = 0xEF65426D;
constexpr uint32_t kHashSpreadRotation = 0x05B462F3;
constexpr uint32_t kHashSpreadFixedElevation = 0xE6B68466;
constexpr uint32_t kHashSpreadEvenAngles = 0xD8F07EBD;
constexpr uint32_t kHashCameraAngleAxis = 0x506F061C;
constexpr uint32_t kHashFlareTemperature = 0x2642E962;
constexpr uint32_t kHashFlareAngle = 0x430D5D92;
constexpr uint32_t kHashFlareShapeLimit = 0x32CA14EC;
constexpr uint32_t kHashFlareTolerance = 0xF856246F;
constexpr uint32_t kHashReceiveGlobal = 0xFA4CEAA3;
constexpr uint32_t kHashScrollU = 0x1364CE7E;
constexpr uint32_t kHashScrollV = 0xACB0CDBB;
constexpr uint32_t kHashLodPercent = 0xFB5568AF;
constexpr uint32_t kHashModelUv = 0x7241DEDA;

class Compiler {
public:
    Compiler(const File& file, EffectDef& def) : file_(file), def_(def) {}

    void Run() {
        const Node* graph = nullptr;
        for (const Node& n : file_.nodes) {
            if (n.ClassName() == "FxModuleGraph") {
                graph = &n;
                break;
            }
        }
        if (!graph) {
            def_.unsupported.push_back("no FxModuleGraph");
            return;
        }
        def_.all_frame = std::max(1u, graph->UInt("allFrame", 1));
        def_.play_mode = graph->UInt("playMode");
        def_.fade_in_end = graph->UInt("fadeInEndFrame");
        def_.fade_out_start = graph->UInt("fadeOutStartFrame");
        def_.update_type = graph->UInt("updateType");
        std::vector<const Edge*> roots;
        for (const Edge& e : file_.edges) {
            if (e.to == graph->index) {
                roots.push_back(&e);
            }
        }
        std::sort(roots.begin(), roots.end(), [](const Edge* a, const Edge* b) { return a->to_port < b->to_port; });
        // one emitter per shape (0xBB5880); its index goes into the node ids of the random seeds (0xBB0E70)
        uint32_t emitter = 0;
        for (const Edge* e : roots) {
            const Node& n = file_.nodes[e->from];
            const std::string_view cls = n.ClassName();
            if (cls == "TppLensFlareProgramEffectNode") {
                AddFlare(n);
            } else if (cls == "FxSoundCallProgramEffectNode") {
                continue;
            } else if (!AddShape(n, emitter)) {
                Unsupported(n);
            }
            if (cls.ends_with("ShapeNode")) {
                ++emitter;
            }
        }
    }

private:
    void Unsupported(const Node& n) {
        std::string name(n.ClassName());
        if (std::find(def_.unsupported.begin(), def_.unsupported.end(), name) == def_.unsupported.end()) {
            def_.unsupported.push_back(name);
        }
    }

    void AddFlare(const Node& n) {
        FlareDef f;
        f.node = n.index;
        f.lens_flare = n.String("lensFlareName");
        f.lux = n.Float("lux", 10.0f);
        f.base_distance = n.Float("baseDistance", 5.0f);
        f.limit_distance = n.Float("limitDistance", 100.0f);
        f.temperature = n.Float(kHashFlareTemperature, 5500.0f);
        f.offset = n.Vec4("offsets");
        f.draw_priority = n.UInt("drawPriority");
        f.angle = n.Float(kHashFlareAngle, 360.0f);
        f.shape_limit = n.Bool(kHashFlareShapeLimit);
        f.depth_tolerance = n.Float(kHashFlareTolerance);
        def_.flares.push_back(std::move(f));
    }

    bool AddShape(const Node& n, uint32_t emitter) {
        const std::string_view cls = n.ClassName();
        ShapeDef s;
        s.node = n.index;
        int ports[kAttrCount] = {-1, -1, -1, -1, -1, -1};
        int material_port = 2;
        if (cls == "FxSpriteRotShapeNode" || cls == "FxPlaneRotShapeNode" || cls == "FxModelPrimitiveShapeNode") {
            s.kind = cls == "FxSpriteRotShapeNode" ? ShapeKind::SpriteRot : cls == "FxPlaneRotShapeNode" ? ShapeKind::Plane : ShapeKind::Model;
            ports[kPosition] = 3;
            ports[kScale] = 4;
            ports[kRotation] = 5;
            ports[kUv] = 6;
            ports[kColor] = 7;
        } else if (cls == "FxSpriteShapeNode") {
            s.kind = ShapeKind::Sprite;
            ports[kPosition] = 3;
            ports[kScale] = 4;
            ports[kUv] = 5;
            ports[kColor] = 6;
        } else if (cls == "FxSprite2DShapeNode") {
            s.kind = ShapeKind::Sprite2D;
            material_port = -1;
            ports[kPosition] = 2;
            ports[kScale] = 3;
            ports[kUv] = 4;
            ports[kColor] = 5;
        } else if (cls == "FxSpotLightShapeNode") {
            s.kind = ShapeKind::SpotLight;
            material_port = -1;
            ports[kPosition] = 2;
            ports[kRotation] = 3;
            ports[kExtra] = 4;
            ports[kColor] = 5;
        } else if (cls == "FxPointLightShapeNode") {
            s.kind = ShapeKind::PointLight;
            material_port = -1;
            ports[kPosition] = 2;
            ports[kColor] = 3;
            ports[kExtra] = 4;
            ports[kScale] = 5;
        } else {
            return false;
        }
        if (!n.Bool("enable", true)) {
            return true;
        }
        s.local_space = n.Bool("localSpace", true);
        s.center_u = n.Float("centerU", 0.5f);
        s.center_v = n.Float("centerV", 0.5f);
        s.sort_mode = n.UInt("sortMode");
        s.sort_offset = n.Float("sortOffset");
        s.base_size = n.Float("baseSizeScale", 1.0f);
        s.cull_face = n.Bool("cullFace");
        if (s.kind == ShapeKind::Model) {
            // 0xB8BED0
            s.model = n.Code("modelFile");
            s.invert_face = n.Bool("invertFace");
            s.model_uv = n.Bool(kHashModelUv);
        }
        s.axis_fix = n.UInt("axisFix");
        s.separate_draw = n.Bool(kHashSeparateDraw);
        s.max_particles = n.UInt("numSimulatedMaxParticle");
        if (s.kind == ShapeKind::Sprite2D) {
            s.screen_width = std::max(1u, n.UInt(kHashScreenWidth, 1280));
            s.screen_height = std::max(1u, n.UInt(kHashScreenHeight, 720));
            s.priority = n.UInt("priority");
            s.material.kind = MaterialKind::Unlit;
            s.material.texture = n.String("textureFile");
            const uint32_t blend = n.UInt(kHashBlendMode2D, 1);
            s.material.blend = blend == 2 ? BlendMode::Add : blend == 0 ? BlendMode::Opaque : BlendMode::Alpha;
        }
        if (s.kind == ShapeKind::SpotLight || s.kind == ShapeKind::PointLight) {
            s.inner_range = n.Float("innerRange");
            s.outer_range = n.Float("outerRange", 1.0f);
            s.attenuation = n.Float("attenuationExponent", 1.0f);
            s.cast_shadow = n.Bool("castShadow");
            s.specular = n.Bool("hasSpecular", true);
            s.view_bias = n.Float("viewBias") * 0.001f;
            s.shadow_bias = n.Float("shadowBias") * 0.001f;
            s.shadow_umbra_scale = n.Float("shadowUmbraAngleScale", 1.0f);
            s.shadow_penumbra_scale = n.Float("shadowPenumbraAngleScale", 1.0f);
            // 0xB84260: the light area (node +0x00 translation, +0x10 rotation, +0x20 scale, +0x6D bit 0 enable)
            s.light_area = n.Bool("enableLightArea");
            s.area_translation = glm::vec3(n.Vec4("lightAreaTranslation"));
            s.area_rotation = n.Vec4(kHashLightAreaRotation, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            s.area_scale = glm::vec3(n.Vec4("lightAreaScale", glm::vec4(1.0f)));
            // 0xB84260: projected mask texture, 0xB85E40 hands it to the light (0xCDBF70)
            if (s.kind == ShapeKind::SpotLight) {
                s.light_mask = n.String(kHashLightMask);
            }
        }
        const size_t first_emit = def_.emits.size();
        if (const Node* emit = file_.Input(n.index, 0)) {
            s.emit = CompileEmit(*emit);
        }
        if (const Node* life = file_.Input(n.index, 1)) {
            CompileLife(*life, s.life);
        }
        if (material_port >= 0) {
            if (const Node* material = file_.Input(n.index, static_cast<uint8_t>(material_port))) {
                CompileMaterial(*material, s);
            }
        }
        for (int a = 0; a < kAttrCount; ++a) {
            if (ports[a] >= 0) {
                if (const Node* in = file_.Input(n.index, static_cast<uint8_t>(ports[a]))) {
                    s.attr[a] = CompileExpr(*in);
                }
            }
        }
        if (s.attr[kPosition] >= 0) {
            s.world_position = def_.exprs[s.attr[kPosition]].world;
        }
        NumberNodes(n, emitter, s, first_emit);
        if (s.emit < 0) {
            return true;
        }
        def_.shapes.push_back(std::move(s));
        return true;
    }

    // Each node of an emitter is numbered when the instance starts (0xBB0E70): the instance's random value (0xB61AB0) + 0xFFFF x the
    // emitter + the node's place in the emitter, and a random node with randomGatherType 0 seeds itself with that id + the instance's
    // random value (0xBA1A40, 0xB6D5B0 and the other inits). The emitter's nodes are listed by 0x12BDEB0 in a walk from the shape, each
    // node before its inputs and the inputs in port order (0xB645E0, edges sorted by target port in 0x12C26C0), a node reached twice
    // listed twice, then numbered by kind (0xB5E650): emit nodes, life nodes, vector nodes, then the rest. node_id keeps
    // 0xFFFF x the emitter + the place; an expression shared by two shapes keeps the first shape's.
    void NumberNodes(const Node& shape, uint32_t emitter, ShapeDef& s, size_t first_emit) {
        std::vector<uint32_t> walk;
        Walk(shape.index, walk, 0);
        std::vector<uint32_t> emits;
        std::vector<uint32_t> lives;
        std::vector<uint32_t> vectors;
        std::vector<uint32_t> others;
        for (uint32_t index : walk) {
            const std::string_view cls = file_.nodes[index].ClassName();
            if (cls.ends_with("EmitNode")) {
                emits.push_back(index);
            } else if (cls.ends_with("LifeNode")) {
                lives.push_back(index);
            } else if (cls.ends_with("VectorNode") || cls.ends_with("Vector2Node")) {
                vectors.push_back(index);
            } else {
                others.push_back(index);
            }
        }
        const auto place = [](const std::vector<uint32_t>& list, uint32_t index) {
            return static_cast<uint32_t>(std::find(list.begin(), list.end(), index) - list.begin());
        };
        const uint32_t base = emitter * 0xFFFFu;
        s.material.node_id = base + static_cast<uint32_t>(emits.size() + lives.size() + vectors.size()) + place(others, s.material.node);
        for (size_t i = first_emit; i < def_.emits.size(); ++i) {
            def_.emits[i].node_id = base + place(emits, def_.emits[i].node);
        }
        if (s.life.node != 0xFFFFFFFFu) {
            s.life.node_id = base + static_cast<uint32_t>(emits.size()) + place(lives, s.life.node);
        }
        for (size_t k = 0; k < vectors.size(); ++k) {
            const auto it = memo_.find(vectors[k]);
            if (it == memo_.end() || it->second < 0) {
                continue;
            }
            Expr& e = def_.exprs[it->second];
            // 0xBA0670: a random start, random flips or fixed flips set 0x180 on the emitter, which then gives every new particle a
            // 16-bit random value (0xBA7F20)
            if (e.op == ExprOp::UvAnime && (e.flags & (2u | 4u | 8u | 16u | 32u))) {
                s.particle_random = true;
            }
            if (e.slot >= 0 && numbered_.insert(it->second).second) {
                e.node_id = base + static_cast<uint32_t>(emits.size() + lives.size() + k);
            }
        }
    }

    void Walk(uint32_t node, std::vector<uint32_t>& out, int depth) const {
        out.push_back(node);
        if (depth > 64) {
            return;
        }
        std::vector<const Edge*> inputs;
        for (const Edge& e : file_.edges) {
            if (e.to == node && e.from < file_.nodes.size()) {
                inputs.push_back(&e);
            }
        }
        std::stable_sort(inputs.begin(), inputs.end(), [](const Edge* a, const Edge* b) { return a->to_port < b->to_port; });
        for (const Edge* e : inputs) {
            Walk(e->from, out, depth + 1);
        }
    }

    int32_t CompileEmit(const Node& n) {
        const std::string_view cls = n.ClassName();
        EmitNode e;
        e.node = n.index;
        if (cls == "FxIntervalProbabilityEmitNode") {
            e.op = EmitOp::Interval;
            e.interval = static_cast<float>(n.UInt("intervalFrame"));
            e.probability = std::clamp(n.Float("probability", 100.0f), 0.0f, 100.0f);
            const uint32_t a = n.UInt("numMin", 1);
            const uint32_t b = n.UInt("numMax", 1);
            e.num_min = static_cast<uint16_t>(std::min(a, b));
            e.num_max = static_cast<uint16_t>(std::max(a, b));
            e.delay = static_cast<uint16_t>(n.UInt("delayFrame"));
            e.delay_range = static_cast<uint16_t>(n.UInt("delayFrameRandomRange"));
            e.life = static_cast<uint16_t>(n.UInt("lifeFrame"));
            e.life_range = static_cast<uint16_t>(n.UInt("lifeRandomRangeFrame"));
            e.fade_position = n.Float("fadeOutPosition", 1.0f);
            e.fade_reverse = n.Bool("fadeOutReverse");
            e.seed = n.UInt("randomGatherSeedValue");
            e.seed_type = static_cast<uint8_t>(n.UInt("randomGatherType"));
        } else if (cls == "FxDelayNumEmitNode") {
            e.op = EmitOp::Delay;
            e.delay_s = static_cast<float>(n.UInt("delayFrame")) / kFrameRate;
            e.delay_range_s = static_cast<float>(n.UInt("delayFrameRandomRange")) / kFrameRate;
            const uint32_t life = n.UInt("lifeFrame");
            e.life_s = static_cast<float>(life) / kFrameRate;
            e.once = life == 1;
            e.num = n.UInt("num", 1);
            e.seed = n.UInt("randomGatherSeedValue");
            e.seed_type = static_cast<uint8_t>(n.UInt("randomGatherType"));
        } else if (cls == "FxFirstLoopOnlyEmitNode" || cls == "FxNumLodEmitNode") {
            e.op = cls == "FxFirstLoopOnlyEmitNode" ? EmitOp::FirstLoopOnly : EmitOp::Lod;
            e.lod_distance = n.Float("lodDistance");
            e.lod_percent = std::clamp(n.Float(kHashLodPercent, 100.0f), 0.0f, 100.0f) * 0.01f;
            e.lod_inverse = n.Bool("inverse");
            const Node* in = file_.Input(n.index, 0);
            if (!in) {
                return -1;
            }
            e.input = CompileEmit(*in);
            if (e.input < 0) {
                return -1;
            }
        } else {
            Unsupported(n);
            return -1;
        }
        def_.emits.push_back(e);
        return static_cast<int32_t>(def_.emits.size() - 1);
    }

    void CompileLife(const Node& n, LifeDef& life) {
        const std::string_view cls = n.ClassName();
        if (cls == "FxConstLifeNode") {
            life.min = life.max = static_cast<float>(n.UInt("lifeFrame", 60)) / kFrameRate;
        } else if (cls == "FxRandomLifeNode") {
            // 0xB6D480 reads 0x9B076750 as the base life and 0x8F88FA93 as a range, both frames / 60; 0xB6D5F0 draws
            // base + (2 range r - range), r = xorshift (13, 7, 5) / 2^32: uniform in [base - range, base + range]. The port had
            // taken them as minimum and maximum, so the ending's ground smoke (360, 0) lived 0 to 6 s instead of 6 s
            const float a = static_cast<float>(n.UInt(kHashRandomLifeMin, 60)) / kFrameRate;
            const float b = static_cast<float>(n.UInt(kHashRandomLifeMax, 60)) / kFrameRate;
            life.min = a - b;
            life.max = a + b;
            life.seed = n.UInt("randomGatherSeedValue");
            life.seed_type = static_cast<uint8_t>(n.UInt("randomGatherType"));
            life.node = n.index;
        } else if (cls == "FxInfinityLifeNode") {
            if (const Node* in = file_.Input(n.index, 0)) {
                CompileLife(*in, life);
            }
            life.infinite = true;
        } else {
            Unsupported(n);
        }
    }

    void CompileMaterial(const Node& n, ShapeDef& s) {
        MaterialDef& m = s.material;
        m.node = n.index;
        const std::string_view cls = n.ClassName();
        m.texture = n.String("textureFile");
        m.shader_type = n.UInt("shaderType");
        m.soft_factor = n.Float("softBlendFactor", 1.0f);
        m.opaque = n.Bool("opaque");
        m.camera_z_offset = n.Float("cameraZOffset");
        m.fade_near = n.Float("cameraFadeInNear");
        m.fade_far = n.Float("cameraFadeInFar");
        m.anime_fps = n.Float("textureAnimeBlendFrame", 1.0f);
        m.anime_fps = m.anime_fps > 1e-5f ? kFrameRate / m.anime_fps : kFrameRate;
        m.anime_w = std::max(1u, n.UInt("textureAnimeBlendWidth", 1));
        m.anime_h = std::max(1u, n.UInt("textureAnimeBlendHeight", 1));
        if (cls == "FxDynamicLuminanceMaterialNode") {
            m.kind = MaterialKind::DynamicLuminance;
            m.blend = static_cast<BlendMode>(std::min(n.UInt("blendType"), 5u));
            m.soft = m.shader_type != 0;
            m.anime_blend = m.shader_type == 2;
            m.min_ev = n.Float("minExposure");
            m.max_ev = n.Float("maxExposure", 1.0f);
            m.lum_min = n.Float(kHashLumAtMin, 1.0f);
            m.lum_max = n.Float(kHashLumAtMax, 1.0f);
        } else if (cls == "FxLightInfluenceMaterialNode") {
            m.kind = MaterialKind::Lit;
            m.blend = m.opaque ? BlendMode::Opaque : BlendMode::Alpha;
            m.soft = n.Bool("softBlend");
            m.anime_blend = n.Bool("textureAnimeBlend");
            m.ambient_rate = n.Float("ambientRate", 1.0f);
            m.directional_rate = n.Float("directionalLightRate");
            m.point_rate = n.Float("pointLightRate");
            // shaderType 0, 1, 2 pick Prim_Poly_LitDP1, DP2, DP3: the draw's first one, two or three point lights (tub_lit_trace_f060:
            // the f060 bathtub's green dust, fx_sh_wtrbld01b_s2 shaderType 0, draws with LitDP1_NS and m_lightParams[2..3] alone)
            m.point_lights = std::clamp(n.UInt("shaderType") + 1u, 1u, 3u);
        } else if (cls == "TppLiquidMaterial2Node" || cls == "TppLiquidMaterial2HNMNode") {
            m.kind = MaterialKind::Liquid;
            m.blend = BlendMode::Alpha;
            m.soft = n.Bool("softBlend");
            m.ambient_rate = n.Float("ambientRate", 1.0f);
            m.directional_rate = n.Float("directionalLightRate");
            m.point_rate = n.Float("pointLightRate");
            m.transparency = n.Float("transparency", 1.0f);
            m.roughness = n.Float("roughness");
            m.normal = m.texture;
            m.liquid_hnm = cls == "TppLiquidMaterial2HNMNode";
            m.refraction = n.Float(kHashLiquidRefraction);
            m.fresnel_base = n.Float(kHashLiquidFresnelBase);
            m.fresnel_power = n.Float(kHashLiquidFresnelPower);
            m.reflection = n.Float(kHashLiquidReflection);
            m.reflection_texture = n.String(kHashLiquidCube);
            if (m.reflection_texture.empty()) {
                m.reflection_texture = kDefaultLiquidCube;
            }
        } else if (cls == "FxScrollAnimationMaterialNode") {
            m.kind = MaterialKind::Scroll;
            m.blend = static_cast<BlendMode>(std::min(n.UInt("blendType"), 5u));
            // B71F40/B72510: Scroll has its own soft factor, independent of shaderType.
            m.soft_factor = n.Float(0x0E3C1540);
            m.soft = m.soft_factor > 0.0f;
            m.luminance = n.Float("luminance", 1.0f);
            m.rain_texture = n.String(0xF77D9DEA);
            m.rain_scale = glm::vec2(n.Float(0x99EA2F62, 1.0f), n.Float(0x1F7E7CF7, 1.0f));
            m.rain_angle = glm::radians(glm::vec2(n.Float(kHashScrollU), n.Float(0x6B880E78)));
            m.rain_swing = glm::radians(glm::vec2(n.Float(kHashScrollV), n.Float(0xF1A308EB)));
            m.rain_scroll_speed = n.Float(0x27F25FE0);
            m.rain_phase_speed = n.Float(0xFBFFDA19);
            m.texture_scale = glm::vec2(n.Float(0x332DD0CB, 1.0f), n.Float(0xEDA9BBFB, 1.0f));
            m.texture_angle = glm::radians(n.Float(0x495EBA72));
        } else {
            Unsupported(n);
            m.kind = MaterialKind::Unlit;
        }
    }

    int32_t Push(Expr e) {
        def_.exprs.push_back(std::move(e));
        return static_cast<int32_t>(def_.exprs.size() - 1);
    }

    int32_t Input(const Node& n, uint8_t port) {
        const Node* in = file_.Input(n.index, port);
        return in ? CompileExpr(*in) : -1;
    }

    uint32_t NewSlot() { return def_.random_slots++; }

    static glm::vec4 Degrees(glm::vec4 v, bool rotation) {
        if (rotation) {
            v.x *= kDegToRad;
            v.y *= kDegToRad;
            v.z *= kDegToRad;
        }
        return v;
    }

    static Curve ReadCurve(const Node& n, const char* times, const char* values) {
        Curve c;
        c.times = n.Floats(times);
        c.values = n.Floats(values);
        const size_t count = std::min(c.times.size(), c.values.size());
        c.times.resize(count);
        c.values.resize(count);
        c.inv_span.resize(count);
        float previous = 1.0f;
        for (size_t i = 0; i < count; ++i) {
            const float span = c.times[i] - previous;
            c.inv_span[i] = 1.0f / (std::fabs(span) < 1e-5f ? 1.0f : span);
            previous = c.times[i];
        }
        return c;
    }

    int32_t CompileExpr(const Node& n) {
        if (auto it = memo_.find(n.index); it != memo_.end()) {
            return it->second;
        }
        const int32_t index = CompileExprUncached(n);
        memo_[n.index] = index;
        return index;
    }

    int32_t CompileExprUncached(const Node& n) {
        const std::string_view cls = n.ClassName();
        Expr e;
        if (cls == "FxConstVectorNode") {
            e.op = ExprOp::Const;
            e.v0 = Degrees(n.Vec4("vector"), n.UInt("vectorType") == 1) * n.Float("force", 1.0f);
            e.flags = n.Bool("global") ? 1u : 0u;
        } else if (cls == "FxColorVectorNode") {
            e.op = ExprOp::Const;
            e.v0 = n.Vec4("color");
        } else if (cls == "FxRandomVectorNode") {
            e.op = ExprOp::Random;
            const bool rot = n.UInt("vectorType") == 1;
            const float force = n.Float("force", 1.0f);
            e.v0 = Degrees(n.Vec4("randomMin"), rot) * force;
            e.v1 = Degrees(n.Vec4("randomMax"), rot) * force;
            e.flags = (n.Bool("xySquere") ? 2u : 0u) | (n.Bool("global") ? 1u : 0u);
            e.seed = n.UInt("randomGatherSeedValue");
            e.seed_type = static_cast<uint8_t>(n.UInt("randomGatherType"));
            e.slot = static_cast<int32_t>(NewSlot());
        } else if (cls == "FxCompositionVectorNode" || cls == "FxMultiplyVectorNode") {
            e.op = cls == "FxCompositionVectorNode" ? ExprOp::Composition : ExprOp::Multiply;
            e.in[0] = Input(n, 0);
            e.in[1] = Input(n, 1);
            const float mask = n.Float("maskValue", 1.0f);
            e.v0 = glm::vec4(n.Bool("secondMaskX") ? 1.0f : 0.0f, n.Bool("secondMaskY") ? 1.0f : 0.0f, n.Bool("secondMaskZ") ? 1.0f : 0.0f,
                             n.Bool("secondMaskW") ? 1.0f : 0.0f);
            e.v1 = glm::vec4(mask);
        } else if (cls == "FxUniformVelocityVectorNode") {
            e.op = ExprOp::UniformVelocity;
            e.in[0] = Input(n, 0);
        } else if (cls == "FxUniformAccelVectorNode") {
            e.op = ExprOp::UniformAccel;
            e.in[0] = Input(n, 0);
            e.in[1] = Input(n, 1);
        } else if (cls == "FxUniformVelocityTimeVectorNode") {
            e.op = ExprOp::UniformVelocityTime;
            e.in[0] = Input(n, 0);
            e.in[1] = Input(n, 1);
        } else if (cls == "FxDragTimeVectorNode") {
            e.op = ExprOp::DragTime;
            e.v0 = glm::vec4(n.Float("drag", 1.0f), n.Float("scale", 1.0f), 0.0f, 0.0f);
            e.mode = static_cast<uint8_t>(n.UInt("method"));
        } else if (cls == "FxTimeScaleVectorNode") {
            e.op = ExprOp::TimeScale;
            e.in[0] = Input(n, 0);
            e.v0 = glm::vec4(n.Bool("maskX") ? 1.0f : 0.0f, n.Bool("maskY") ? 1.0f : 0.0f, n.Bool("maskZ") ? 1.0f : 0.0f,
                             n.Bool("maskW") ? 1.0f : 0.0f);
            e.v1 = glm::vec4(n.Float("startScale", 1.0f), n.Float("endScale", 1.0f), 0.0f, 0.0f);
        } else if (cls == "FxKeyframeVectorNode") {
            e.op = ExprOp::Keyframe;
            e.in[0] = Input(n, 0);
            e.mode = static_cast<uint8_t>(n.UInt("operatorMethod"));
            const uint32_t ratio = n.UInt("timeRatio");
            e.time = ratio == 0   ? TimeSource::Age
                     : ratio == 1 ? TimeSource::LifeRatio
                     : ratio == 2 ? TimeSource::EffectTime
                                  : TimeSource::EffectRatio;
            e.curves[0] = ReadCurve(n, "xTimes", "xValues");
            e.curves[1] = ReadCurve(n, "yTimes", "yValues");
            e.curves[2] = ReadCurve(n, "zTimes", "zValues");
            e.curves[3] = ReadCurve(n, "wTimes", "wValues");
        } else if (cls == "FxOscillateVector2Node") {
            e.op = ExprOp::Oscillate;
            e.in[0] = Input(n, 0);
            e.in[1] = Input(n, 1);
        } else if (cls == "FxUVMapVectorNode") {
            e.op = ExprOp::UvMap;
            glm::vec4 rect = n.Vec4(kHashUvRect, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
            if (n.Bool(kHashFlipU)) {
                rect.x += rect.z;
                rect.z = -rect.z;
            }
            if (n.Bool(kHashFlipV)) {
                rect.y += rect.w;
                rect.w = -rect.w;
            }
            e.v0 = rect;
        } else if (cls == "FxUVMapRandomVectorNode") {
            e.op = ExprOp::UvRandom;
            e.v0 = glm::vec4(static_cast<float>(n.UInt("randomDivisionWidthGrid")), static_cast<float>(n.UInt("randomDivisionHeightGrid")), 0.0f,
                             0.0f);
            e.flags = (n.Bool("randomFlipU") ? 1u : 0u) | (n.Bool("randomFlipV") ? 2u : 0u);
            e.seed = n.UInt("randomGatherSeedValue");
            e.seed_type = static_cast<uint8_t>(n.UInt("randomGatherType"));
            e.slot = static_cast<int32_t>(NewSlot());
        } else if (cls == "FxUVAnimeIntervalVectorNode") {
            e.op = ExprOp::UvAnime;
            const float interval = n.Float(kHashAnimationFrame, 1.0f);
            const float frames = std::fabs(interval) < 1e-5f ? 1.0f : interval;
            e.v0 = glm::vec4(kFrameRate / frames, static_cast<float>(std::max(1u, n.UInt(kHashDivisionW, 1))),
                             static_cast<float>(std::max(1u, n.UInt(kHashDivisionH, 1))), 0.0f);
            e.flags = (n.Bool("clamp") ? 1u : 0u) | (n.Bool("randomStart") ? 2u : 0u) | (n.Bool("randomFlipU") ? 4u : 0u) |
                      (n.Bool("randomFlipV") ? 8u : 0u) | (n.Bool(kHashFlipU) ? 16u : 0u) | (n.Bool(kHashFlipV) ? 32u : 0u);
            // no seed of its own: 0xBA0670 does not read randomGatherType, the start and flips come from the particle's random value
            e.slot = static_cast<int32_t>(NewSlot());
        } else if (cls == "FxCameraCorrectionVectorNode") {
            e.op = ExprOp::CameraCorrection;
            e.in[0] = Input(n, 0);
            e.mode = static_cast<uint8_t>(n.UInt(kHashCorrectionType));
            e.v0 = glm::vec4(n.Float(kHashCorrectionOffset), 0.0f, 0.0f, 0.0f);
        } else if (cls == "FxCameraFollowVectorNode") {
            e.op = ExprOp::CameraFollow;
            e.in[0] = Input(n, 0);
            e.v0 = n.Vec4(kHashFollowOffset);
            e.v0.w = 0.0f;
            e.flags = n.Bool(kHashFollowYaw) ? 1u : 0u;
            e.world = true;
        } else if (cls == "FxCenterScrollVectorNode") {
            e.op = ExprOp::CenterScroll;
            e.in[0] = Input(n, 0);
            e.v0 = n.Vec4("range", glm::vec4(1.0f));
            e.world = e.in[0] >= 0 && def_.exprs[e.in[0]].world;
            if (e.in[0] >= 0 && def_.exprs[e.in[0]].op == ExprOp::CameraFollow) {
                e.v1 = def_.exprs[e.in[0]].v0;
            }
        } else if (cls == "FxPoolVectorNode") {
            e.op = ExprOp::Pass;
            e.in[0] = Input(n, 0);
            e.world = e.in[0] >= 0 && def_.exprs[e.in[0]].world;
            if (e.in[0] >= 0) {
                e.v1 = def_.exprs[e.in[0]].v1;
                pool_center_ = e.v1;
            }
        } else if (cls == "FxCenterDistRateVectorNode") {
            e.op = ExprOp::CenterDistRate;
            e.in[0] = Input(n, 0);
            e.v0 = n.Vec4("nearDist", glm::vec4(0.0f));
            e.v1 = n.Vec4("farDist", glm::vec4(1.0f));
            e.v2 = glm::vec4(n.Vec4("nearScale", glm::vec4(1.0f)).x, n.Vec4("farScale", glm::vec4(0.0f)).x, 0.0f, 0.0f);
            e.v3 = pool_center_;
        } else if (cls == "FxCameraAngleVectorNode") {
            e.op = ExprOp::CameraAngle;
            e.mode = static_cast<uint8_t>(n.UInt(kHashCameraAngleAxis, 2));
            e.flags = n.Bool("localCoordinate") ? 1u : 0u;
        } else if (cls == "FxInterpolateLineVectorNode") {
            e.op = ExprOp::InterpolateLine;
            e.v0 = n.Vec4("beginPosition");
            e.v1 = n.Vec4("endPosition");
            e.v0.w = e.v1.w = 0.0f;
        } else if (cls == "FxSpreadVectorNode") {
            // factory 0xB96B50: elevation / 180, force, its random range 0xEF65426D (with forceType != 0 the length is uniform in
            // force -/+ range, the range cut to force so it never goes below 0), rangeAngle in radians, the rotation 0x05B462F3 and
            // the flags 0xE6B68466 (a fixed polar angle instead of a random one) and 0xD8F07EBD (one length per batch, even angles)
            e.op = ExprOp::Spread;
            const float force = n.Float("force", 1.0f);
            float range = n.Float(kHashSpreadRange);
            if (force - range < 0.0f) {
                range = force;
            }
            const bool random_force = n.UInt("forceType") != 0;
            e.v0 = glm::vec4(n.Float("elevation", 360.0f) / 180.0f, n.Float("rangeAngle", 360.0f) * kDegToRad,
                             random_force ? force - range : force, random_force ? force + range : force);
            e.v1 = n.Vec4(kHashSpreadRotation, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            e.mode = (n.Bool(kHashSpreadFixedElevation) ? 1u : 0u) | (n.Bool(kHashSpreadEvenAngles) ? 2u : 0u);
            e.seed = n.UInt("randomGatherSeedValue");
            e.seed_type = static_cast<uint8_t>(n.UInt("randomGatherType"));
            e.slot = static_cast<int32_t>(NewSlot());
        } else if (cls == "FxReceiveVectorNode") {
            e.op = ExprOp::Receive;
            e.v0 = n.Vec4("defaultVector", glm::vec4(1.0f));
            e.v1 = glm::vec4(n.Float("force", 1.0f));
            e.receive = NameHash(n.String("receiveName"));
            e.flags = n.Bool(kHashReceiveGlobal) ? 1u : 0u;
        } else if (cls == "WindFxVectorNode") {
            e.op = ExprOp::Wind;
            e.v0 = glm::vec4(n.Float("airResistanceRate"), 0.0f, 0.0f, 0.0f);
        } else {
            Unsupported(n);
            e.op = ExprOp::Zero;
        }
        return Push(std::move(e));
    }

    const File& file_;
    EffectDef& def_;
    std::unordered_map<uint32_t, int32_t> memo_;
    std::unordered_set<int32_t> numbered_;
    glm::vec4 pool_center_{0.0f};
};

}

float Curve::Eval(float t) const {
    const size_t count = times.size();
    if (count == 0) {
        return 0.0f;
    }
    if (count == 1) {
        return values[0];
    }
    for (size_t i = 0; i < count; ++i) {
        if (t < times[i]) {
            if (i == 0) {
                return values[0];
            }
            return values[i - 1] + (t - times[i - 1]) * inv_span[i] * (values[i] - values[i - 1]);
        }
    }
    return values[count - 1];
}

std::shared_ptr<EffectDef> CompileEffect(const File& file, std::string name) {
    auto def = std::make_shared<EffectDef>();
    def->name = std::move(name);
    Compiler compiler(file, *def);
    compiler.Run();
    if (!def->unsupported.empty()) {
        std::string list;
        for (const std::string& s : def->unsupported) {
            list += (list.empty() ? "" : ", ") + s;
        }
        LogDebug("vfx: {} unsupported nodes: {}", def->name, list);
    }
    return def;
}

}
