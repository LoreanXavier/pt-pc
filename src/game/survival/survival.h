#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/anim/physical_body.h"
#include "engine/anim/ragdoll.h"
#include "engine/render/mesh.h"
#include "engine/render/scene_renderer.h"
#include "game/survival/street_city.h"
#include "game/survival/survival_acoustics.h"
#include "game/survival/survival_models.h"
#include "game/survival/survival_sounds.h"
#include "game/survival/survival_waves.h"
#include "game/survival/survival_boons.h"
#include "game/survival/survival_profile.h"
#include "engine/vfx/vfx_system.h"

namespace pt {
struct InputState;
struct SceneLighting;
struct Camera;
namespace anim {
struct Skeleton;
}
namespace audio {
class Media;
}
}

namespace pt {
struct FmdlMaterial;
}

namespace pt::game {

class Game;
class SurvivalPoser;

enum class SurvivalEnemyKind : uint8_t { Doll, Crawler, Stretched, Mother, Lisa, Giant, Count };
constexpr int kSurvivalCauseDark = static_cast<int>(SurvivalEnemyKind::Count);
enum class SurvivalWeapon : uint8_t { Flashlight, Handgun, Shotgun, Magnum, Lever, Double, Count };
inline bool SurvivalCycled(SurvivalWeapon w) { return w == SurvivalWeapon::Shotgun || w == SurvivalWeapon::Lever; }
inline bool SurvivalLongGun(SurvivalWeapon w) { return w == SurvivalWeapon::Shotgun || w == SurvivalWeapon::Lever || w == SurvivalWeapon::Double; }
enum class SurvivalPickupKind : uint8_t { Ammo, Health, Weapon, Pipe, Flare };
enum class SurvivalDifficulty : uint8_t { Easy, Normal, Hard, Nightmare, Insanity, Count };
enum class SurvivalPhase : uint8_t { Countdown, Wave, Break, Dead };
enum class SurvivalStart : uint8_t { Fresh, Continue, Daily };
enum SurvivalEvent : uint32_t {
    kEventBlackout = 1u,
    kEventFog = 2u,
    kEventSiren = 4u,
    kEventFlank = 8u,
    kEventHorde = 16u,
    kEventDrought = 32u,
    kEventHunt = 64u,
    kEventHell = 128u,
};
constexpr int kSurvivalWaves = 100;

struct SurvivalHud {
    bool active = false;
    SurvivalPhase phase = SurvivalPhase::Countdown;
    SurvivalDifficulty difficulty = SurvivalDifficulty::Normal;
    int wave = 0;
    int enemies_left = 0;
    float countdown = 0.0f;
    float health = 1.0f;
    float heart_rate = 68.0f;
    float last_beat = 0.0f;
    std::array<float, 8> beats{-100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f};
    float low_health = 0.0f;
    float ammo_shown = 1.0f;
    float weapon_name_shown = 0.0f;
    float stamina = 1.0f;
    bool winded = false;
    SurvivalWeapon weapon = SurvivalWeapon::Flashlight;
    std::array<bool, static_cast<size_t>(SurvivalWeapon::Count)> owned{};
    int clip = 0;
    int clip_size = 0;
    int reserve = 0;
    bool reloading = false;
    float reload_progress = 0.0f;
    float hurt = 0.0f;
    float heal = 0.0f;
    float hurt_angle = 0.0f;
    float hurt_arc = 0.0f;
    float banner = 0.0f;
    std::string banner_key;
    int banner_value = 0;
    float pickup_note = 0.0f;
    std::string pickup_key;
    int pickup_value = 0;
    float omen = 0.0f;
    std::string omen_key;
    std::string notice_key;
    std::string notice_extra;
    float notice_hold = 0.0f;
    float notice_age = -1.0f;
    uint32_t notice_serial = 0;
    bool notice_vision = false;
    uint32_t glitch_serial = 0;
    std::string accuse_key;
    uint32_t accuse_serial = 0;
    int vision_kind = 0;
    int vision_index = -1;
    float vision_k = 0.0f;
    float vision_window = -1.0f;
    glm::vec3 vision_tint{1.0f};
    bool dead = false;
    bool caught = false;
    float dead_time = 0.0f;
    bool can_retry = false;
    int best_wave = 0;
    float run_seconds = 0.0f;
    int cause = -1;
    bool caught_end = false;
    int end_kills = 0;
    int end_headshots = 0;
    int end_accuracy = 0;
    int end_score = 0;
    int end_chain = 0;
    float end_seconds = 0.0f;
    bool end_new_best = false;
    int end_credits_earned = 0;
    int end_credits_total = 0;
    int end_daily_bonus = 0;
    int daily_mutator = -1;
    std::array<uint8_t, 3> offer{};
    std::array<uint8_t, 3> offer_rank{};
    int offer_count = 0;
    int offer_taken = -1;
    float offer_age = 0.0f;
    float taken_age = 0.0f;
    bool daily = false;
    float kept = 0.0f;
    int kept_wave = 0;
    float pipe = -1.0f;
    int flares = 0;
    std::string task_key;
    float task_age = -1.0f;
    float task_shown = 0.0f;
    int task_count = 0;
    int task_need = 0;
    bool task_done = false;
    bool task_lost = false;
    std::string use_key;
    float use_progress = 0.0f;
    std::vector<glm::vec3> task_marks;
    bool supply_on = false;
    glm::vec3 supply_at{0.0f};
    int dark_count = 0;
    float fear = 0.0f;
    int false_left = -1;
    float burst_ready = 1.0f;
    float aim = 0.0f;
    float sights = 0.0f;
    bool first_person = true;
    float spread = 0.0f;
    bool hit_head = false;
    float hit_mark = 0.0f;
    bool hit_mark_head = false;
    bool hit_mark_kill = false;
    float lisa_health = -1.0f;
    float giant_health = -1.0f;
    bool boss = false;
    std::string boss_key;
    glm::mat4 view{1.0f};
    float fov_y = 1.0f;
    glm::vec3 eye{0.0f};
};

class SurvivalNight {
public:
    explicit SurvivalNight(Game& game);
    ~SurvivalNight();

    bool Active() const { return active_; }
    float HouseDoor() const;
    void Act(const std::string& action);
    void AgentInput(InputState& input);
    void Start(int best_wave, SurvivalDifficulty difficulty, SurvivalStart start = SurvivalStart::Fresh);
    void Stop();
    void Update(float dt, const InputState& input, const Camera& camera);
    void AddDraws(std::vector<DrawItem>& out, const Camera& camera);
    bool CityUp() const { return city_.Up(); }
    void AddEffectQuads(vfx::RenderList& list, const Camera& camera) const;
    void AddContactQuads(vfx::RenderList& list, const Camera& camera) const;
    void AddLights(SceneLighting& lighting) const;
    void ApplyStreetEvents(SceneLighting& lighting) const;
    const SurvivalHud& Hud() const { return hud_; }
    const SurvivalModels* EnsureArmoryModels(int slot = 0) { EnsureArmoryAssets(slot); return &models_; }
    const SurvivalModels* ArmoryModelsIfLoaded() const { return models_.armory_slots_loaded ? &models_ : nullptr; }
    void RefreshLooks() { if (active_ && assets_ready_) ApplyLooks(true); }
    ReloadStyle ActiveReloadStyle() const;
    bool Choosing() const { return active_ && offer_count_ > 0; }
    void ChooseOffer(int index);
    float StreetLight() const { return active_ ? StreetLightLevel() : 1.0f; }
    float SpeedScale() const { return speed_scale_; }
    float FovKick() const { return fov_kick_; }
    float CameraShake() const { return shake_; }
    void TriggerFlashlightClick();
    float FlashClickWeight() const;
    float FlashClickPress() const;
    float FlashClickCameraDip() const;
    glm::vec2 ViewKick() const { return view_kick_; }
    float ViewPush() const;
    void DeathCamera(Camera& camera) const;
    bool Waking() const { return active_ && waking_; }
    bool WakingThirdPerson() const { return active_ && waking_ && wake_third_; }
    void WakeCamera(Camera& camera) const;
    void ClampCameraClear(Camera& camera) const;
    bool AtSights() const { return active_ && !hud_.dead && sights_; }
    float Lean() const { return lean_; }
    float RunWeight() const;
    float BodyRunWeight() const;
    float BodyRunArms() const;
    float ThirdPersonStockDeep() const { return weapon_ == SurvivalWeapon::Lever ? 0.03f : weapon_ == SurvivalWeapon::Double ? 0.02f : 0.0f; }
    float ThirdPersonElbowSwing() const { return SurvivalLongGun(weapon_) ? glm::mix(1.5f, 0.5f, ThirdPersonRaise()) : 0.4f; }
    float SprintWeight() const;
    glm::vec4 RunBob() const;
    float RunBobWeight() const { return RunWeight() * bob_weight_; }
    bool GunRaised() const { return active_ && !hud_.dead && weapon_ != SurvivalWeapon::Flashlight && SprintWeight() < 0.47f; }
    bool ThirdPersonGrip(const glm::mat4& body, const Camera& camera, glm::mat4& right_hand);
    struct StockLine {
        glm::vec3 first, second;
        glm::vec3 comb;
        glm::vec3 fore_first, fore_second;
    };
    std::optional<StockLine> ThirdPersonStock(const glm::mat4& body, const Camera& camera) const;
    const GpuMesh* FittedBody();
    const GpuMesh* FittedShirt() { return FittedBody() ? shirt_mesh_ : nullptr; }
    const GpuMesh* FittedBodyReady() const { return active_ ? body_mesh_ : nullptr; }
    const GpuMesh* FittedShirtReady() const { return active_ ? shirt_mesh_ : nullptr; }
    void FitBodyVertex(Vertex& v, const FmdlMaterial& material) const;
    glm::mat4 WeaponTransformFor(const Camera& camera) const { return WeaponTransform(camera); }
    void HeldGunBounds(glm::vec3& lo, glm::vec3& hi) const {
        const GpuMesh* body = models_.guns[static_cast<size_t>(weapon_)].body;
        lo = body && weapon_ != SurvivalWeapon::Flashlight ? body->bounds_min : glm::vec3(0.0f);
        hi = body && weapon_ != SurvivalWeapon::Flashlight ? body->bounds_max : glm::vec3(0.0f);
    }
    bool ThirdPersonSupport(const Camera& camera, glm::vec3& point, glm::vec3& bore) const;
    std::optional<std::array<glm::vec4, 5>> GripFingers() const;
    std::array<glm::vec4, 5> HeldFingers(const SurvivalGunModel& model) const;
    std::optional<std::array<glm::vec4, 5>> SupportFingers() const;
    std::array<glm::vec4, 5> LeftFingers(const SurvivalGunModel& model) const;
    bool ChestLight(glm::mat4& world) const;
    void AimChestLamp(float dt);
    bool ThirdPersonLeftHand(const Camera& camera, glm::mat4& world) const;
    float ThirdPersonAim() const;
    float ThirdPersonSteady() const;
    float ThirdPersonShove() const { return std::max({ShoveOut(), ShoveWind(), PipeHold()}); }
    float ThirdPersonLunge() const;
    glm::vec2 ThirdPersonCarry() const;
    void ThirdPersonStance(float& long_gun, float& recoil, std::optional<glm::vec3>& left_elbow, std::optional<glm::vec3>& right_elbow, float& right_share) const;
    glm::vec3 MeleeBody() const;
    int TakeRecord();

private:
    static constexpr size_t kSkinSlots = 36;
    struct Enemy {
        uint32_t id = 0;
        SurvivalEnemyKind kind = SurvivalEnemyKind::Doll;
        glm::vec3 position{0.0f};
        float yaw = 0.0f;
        float health = 1.0f;
        float max_health = 1.0f;
        float speed = 1.0f;
        float damage = 10.0f;
        enum class State : uint8_t { Rising, Chasing, Striking, Stunned, Dying } state = State::Rising;
        float timer = 0.0f;
        double anim_frame = 0.0;
        float gait = 0.0f;
        float hurt = 0.0f;
        float strike_cooldown = 0.0f;
        float slow = 0.0f;
        float sink = 0.0f;
        bool struck = false;
        size_t slot = 0;
        std::string motion;
        glm::vec2 flinch{0.0f};
        float stagger = 0.0f;
        float shoved = 0.0f;
        glm::vec3 knock{0.0f};
        glm::vec2 twitch{0.0f};
        glm::vec2 twitch_to{0.0f};
        float twitch_wait = 0.0f;
        glm::vec2 fall_dir{0.0f, -1.0f};
        float fall = 0.0f;
        float fall_speed = 0.0f;
        bool landed = false;
        bool crumple = false;
        float pause = 0.0f;
        float pause_wait = 3.0f;
        glm::vec3 leap_from{0.0f};
        glm::vec3 leap_to{0.0f};
        float pitch = 0.0f;
        bool latched = false;
        uint8_t affix = 0;
        float scream = -1.0f;
        bool screamed = false;
        float frenzy = 0.0f;
        bool perched = false;
        float burn = 0.0f;
        float pace = 1.0f;
        float fly = 0.26f;
        float span = 3.4f;
        float weave = 0.0f;
        bool way_round = false;
        float way_wait = 0.0f;
        float up_time = 0.0f;
        float drop = -1.0f;
        float hemmed = 0.0f;
        float push_through = 0.0f;
        float perch_wait = 0.0f;
        glm::vec3 head{0.0f};
        float head_radius = 0.12f;
        bool blood_left = false;
        float sound_wait = 0.0f;
        float run_sound_wait = 0.0f;
        double step_frame = 0.0;
        glm::vec2 bend{0.0f};
        float twist = 0.0f;
        glm::vec2 bend_speed{0.0f};
        float twist_speed = 0.0f;
        struct Wound {
            int bone = -1;
            glm::vec3 point{0.0f};
            glm::vec3 normal{0.0f, 0.0f, 1.0f};
            float size = 1.0f;
            float age = 0.0f;
            float drip = 0.0f;
        };
        std::vector<Wound> wounds;
        bool ragdoll = false;
        bool fall_heard = false;
        glm::vec3 last_hit_at{0.0f};
        glm::vec3 last_hit_dir{0.0f, 0.0f, -1.0f};
        float last_hit_power = 1.0f;
        bool last_hit_head = false;
        bool has_last_hit = false;
        bool boss = false;
        int phase = 0;
        float boss_timer = 0.0f;
        float beam_held = 0.0f;
    };
    struct Pickup {
        uint32_t id = 0;
        SurvivalPickupKind kind = SurvivalPickupKind::Ammo;
        SurvivalWeapon weapon = SurvivalWeapon::Handgun;
        glm::vec3 position{0.0f};
        glm::vec3 velocity{0.0f};
        float ground = 0.0f;
        float age = 0.0f;
        float life = 30.0f;
        bool settled = false;
    };
    struct Tracer {
        glm::vec3 from{0.0f};
        glm::vec3 to{0.0f};
        float age = 0.0f;
    };
    struct Debris {
        enum class Kind : uint8_t { Casing, Shell, Magazine, Blood, Spark, Mist, PipeHead, PipeTail, Heal } kind = Kind::Casing;
        uint32_t id = 0;
        glm::vec3 position{0.0f};
        glm::vec3 velocity{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 spin{0.0f};
        float ground = 0.0f;
        float age = 0.0f;
        float life = 6.0f;
        int bounces = 0;
        float size = 1.0f;
    };
    struct Mark {
        enum class Kind : uint8_t { Blood, Hole, Splat } kind = Kind::Blood;
        uint32_t id = 0;
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        float spin = 0.0f;
        float size = 1.0f;
        float grow = 1.0f;
        float age = 0.0f;
        int variant = 0;
    };
    struct WeaponState {
        bool owned = false;
        int clip = 0;
        int reserve = 0;
    };
    struct WeaponSpec {
        int clip_size;
        int start_reserve;
        int pickup_ammo;
        int max_reserve;
        float damage;
        int pellets;
        float spread;
        float interval;
        float reload;
        float range;
        float kick;
        float push;
        const char* name_key;
    };
    struct GunAnim {
        float slide = 0.0f;
        float hammer = 0.0f;
        float cylinder = 0.0f;
        float cylinder_to = 0.0f;
        float swing = 0.0f;
        float magazine = 0.0f;
        float tilt = 0.0f;
        float lift = 0.0f;
        float reload_at = 0.0f;
        float pump_timer = -1.0f;
        float lever = 0.0f;
        float hammer_fall = 0.0f;
        float open = 0.0f;
        bool pumped_after_reload = false;
        bool reload_cut = false;
        bool magazine_dropped = false;
        bool pumping = false;
        bool shell_pending = false;
        float trigger = -1.0f;
        bool cases_out = false;
        bool slide_locked = false;
        bool chambered = true;
        float since_shot = 10.0f;
    };

    void ResetRun();
    void StartCountdown(float seconds);
    void StartWave();
    void SpawnEnemy(SurvivalEnemyKind kind);
    Enemy* SpawnEnemyAt(SurvivalEnemyKind kind, const glm::vec3& at, float rise, uint8_t affix, bool counted);
    uint8_t PickAffix(SurvivalEnemyKind kind);
    void FindPerches();
    std::vector<glm::vec3> perches_;
    void BuildWays();
    void UpdateWays();
    bool WayOn(const glm::vec3& from, glm::vec3& dir) const;
    bool WayDown(const glm::vec3& from, glm::vec3& at) const;
    void DumpWays(const std::string& path) const;
    std::vector<uint8_t> way_links_;
    std::vector<uint16_t> way_steps_;
    std::vector<int> way_queue_;
    int way_from_ = -1;
    bool ways_built_ = false;
    std::vector<glm::vec3> splits_;
    bool perches_found_ = false;
    bool InBeam(const Enemy& e) const;
    void Scream(Enemy& e);
    SurvivalEnemyKind PickKind();
    uint32_t PlanEvents();
    void UpdateEnemies(float dt);
    void PoseEnemy(Enemy& e, float dt);
    void PoseLisa(Enemy& e, std::string_view motion, float rate, float dt);
    void UpdatePlayer(float dt, const InputState& input, const Camera& camera);
    void UpdateGun(float dt);
    void AutoPlay(uint32_t& held, uint32_t& pressed, const Camera& camera);
    std::string AgentState(uint64_t serial) const;
    void Fire(const Camera& camera);
    void FlashBurst(const Camera& camera);
    void StartReload();
    void FinishReloadStep(bool cut = false);
    bool HitEnemy(const glm::vec3& origin, const glm::vec3& dir, float max_distance, const Enemy* skip, Enemy*& hit, float& t, bool& head) const;
    void Damage(Enemy& e, float amount, const glm::vec3& from, const glm::vec3& at, const glm::vec3& dir, bool head, bool round = true);
    void Kill(Enemy& e, const glm::vec3& dir);
    void HurtPlayer(float amount, const glm::vec3& from, int cause = -1);
    void Catch(Enemy& lisa);
    void Die(int cause, bool caught = false);
    void Retry();
    void PlaceUprightOnStreet();
    void StartWake(const std::string& banner_key, int banner_value);
    void UpdatePickups(float dt);
    void UpdateEffects(float dt);
    void Drop(const glm::vec3& at, SurvivalPickupKind kind, SurvivalWeapon weapon);
    SurvivalWeapon AmmoNeeded();
    void Spray(const glm::vec3& at, const glm::vec3& dir, int count, float speed);
    void Bleed(Enemy& e, const glm::vec3& at, const glm::vec3& dir, bool head, float power);
    void AddWound(Enemy& e, const glm::vec3& at, const glm::vec3& dir, bool head);
    glm::mat4 EnemyTransform(const Enemy& e, bool sunk = true) const;
    void ApplyPhysical(Enemy& e, SurvivalPoser& poser, float dt);
    void StrikeBody(const Enemy& e, const glm::vec3& at, const glm::vec3& velocity, float radius);
    void Impact(const glm::vec3& at, const glm::vec3& normal);
    void Eject(Debris::Kind kind, const glm::mat4& gun, const glm::vec3& at);
    void SpawnHealEffect(const glm::vec3& feet);
    float GroundAt(const glm::vec3& p, float fallback) const;
    bool Blocked(const glm::vec3& from, const glm::vec3& dir, float distance) const;
    glm::vec3 SafeFlatSpot(const glm::vec3& desired, float radius, float max_delta) const;
    bool SpawnPoint(glm::vec3& out, float closest, float farthest);
    bool Crowded(const glm::vec3& at) const;
    bool Reaches(const Enemy& e, const glm::vec3& feet) const;
    void Banner(const std::string& key, int value, float seconds);
    void Omen(const char* key, float seconds);
    void Voice(std::string_view key, float delay = 0.3f);
    void Speak(const std::shared_ptr<const audio::Media>& media, std::string_view name, float delay = 0.3f);
    void UpdateVoice(float dt);
    void HushVoice();
    enum class Vision : uint8_t { None, Fuse, Radio, Photo, Lantern, Door };
    static Vision VisionOf(std::string_view key);
    void Notice(const std::string& key, std::string extra, float hold, Vision vision);
    void UpdateNotices(float dt);
    bool VisionFlash(int& index, float& k) const;
    void UpdateAccusations(float dt);
    void Accuse(int line);
    struct NoticeItem {
        std::string key;
        std::string extra;
        float hold = 0.0f;
        Vision vision = Vision::None;
    };
    std::deque<NoticeItem> notices_;
    NoticeItem notice_;
    bool notice_on_ = false;
    float notice_age_ = 0.0f;
    int notice_flash_ = -1;
    std::string notice_task_key_;
    int notice_task_count_ = 0;
    bool notice_task_over_ = false;
    float glitch_wait_ = 30.0f;
    bool offer_locked_ = false;
    float accuse_wait_ = 25.0f;
    int accuse_last_ = -1;
    void Note(const std::string& key, int value = 0, float hold = 2.5f);
    void EnsureArmoryAssets(int slot = 0);
    void EnsureAssets();
    void PlaySound(const std::shared_ptr<const audio::Media>& media, const glm::vec3* at, uint32_t* playing = nullptr);
    uint32_t PostEvent(std::string_view name, const glm::vec3* at);
    struct Cue {
        std::vector<survival_sounds::Take> takes;
        float gain_db = 0.0f;
        float vary_db = 0.0f;
        float speed = 1.0f;
        float vary_speed = 0.0f;
        size_t last = SIZE_MAX;
    };
    void Play(Cue& cue, const glm::vec3* at, float gain_db = 0.0f, float speed = 1.0f);
    void PlayShot(const glm::vec3& ear, const glm::vec3& forward, const glm::vec3& right);
    struct Delayed {
        float wait = 0.0f;
        Cue* cue = nullptr;
        float gain_db = 0.0f;
        float speed = 1.0f;
    };
    std::vector<Delayed> delayed_;
    void Toll(int count);
    void UpdateTolls(float dt);
    void Stomp(const glm::vec3& at, float gain_db = 0.0f);
    float SoundRand();
    void SelectWeapon(SurvivalWeapon weapon, bool from_road = false);
    void TakeWeapon(SurvivalWeapon weapon);
    void NextWeapon(int step);
    int AliveEnemies() const;
    size_t TakeSkinSlot();
    float ShellStep() const;
    struct ReloadPose {
        glm::vec3 at{0.0f};
        glm::vec3 turn{0.0f};
        float hand = 0.0f;
        glm::vec3 left{0.0f};
        glm::quat left_turn{1.0f, 0.0f, 0.0f, 0.0f};
        float away = 0.0f;
        float elbow = 1.0f;
        std::array<glm::vec4, 5> fingers{};
        float thumb = 0.0f;
        glm::vec4 right_thumb{0.0f};
        float item = 0.0f;
        float seat = 0.0f;
        float spent = 0.0f;
        float open = 0.0f;
    };
    bool ReloadNow(ReloadPose& pose) const;
    void AddReloadItems(std::vector<DrawItem>& out, const glm::mat4& gun, const glm::mat4& hand, bool first_person) const;
    glm::mat4 WeaponTransform(const Camera& camera) const;
    glm::mat4 DrawnWeaponTransform(const Camera& camera) const;
    glm::mat4 ThirdPersonRightHandLocal() const;
    glm::mat4 FirstPersonWeapon(const Camera& camera) const;
    float ThirdPersonRaise() const;
    bool BodyHoldsGun() const;
    uint8_t BodyViews() const;
    void AddGunDraws(std::vector<DrawItem>& out, const glm::mat4& gun, uint8_t hidden_views) const;
    void AddArmDraws(std::vector<DrawItem>& out, const Camera& camera, const glm::mat4& gun);
    void MakeArms();
    float LeftHandOnGun(glm::mat4& hand) const;
    glm::mat4 GripOnGun() const;
    glm::mat4 BarrelsTurn() const;
    float Rand();
    float PlanRand();
    uint32_t PlanSeed(int wave, uint32_t stream) const;
    float StreetLightLevel() const;
    float BlackoutLevel() const;
    float LampsLevel() const;
    float GlowLevel() const;
    float LampAlive(size_t i) const;
    void UpdateLamps(float dt);
    void UpdateLaterNights(float dt);
    int Night() const;
    static float NorthEndOf(int night);
    void Midnight();
    void UpdateMidnight(float dt);
    void HushMidnight();
    void DimStreetGlow(float level, const glm::vec3& tint);
    glm::vec3 HellTint() const;
    void MatchCrossStreet(bool on);
    void SettleStreetLayers(bool on);
    std::vector<std::pair<std::string, std::vector<Vertex>>> settled_own_;
    void DressBareThings(bool on);
    std::vector<std::pair<uint32_t, MaterialGpu>> bare_materials_;
    float Spread() const;
    static const WeaponSpec& Spec(SurvivalWeapon w);
    WeaponSpec Gun(SurvivalWeapon w) const;
    SurvivalBoons boons_;
    std::array<SurvivalBoon, 3> offer_{};
    int offer_count_ = 0;
    uint32_t radio_noise_ = 0;
    void OfferBoons();
    void TakeBoon(int index);
    bool last_breath_used_ = false;
    SurvivalStart start_ = SurvivalStart::Fresh;
    bool hud_daily_pending_ = false;
    int flares_ = 0;
    float melee_timer_ = -1.0f;
    float melee_wind_ = 0.0f;
    bool melee_whoosh_ = false;
    bool melee_pipe_ = false;
    bool melee_done_ = false;
    int pipe_hits_ = 0;
    float pipe_time_ = 0.0f;
    int pipe_hits_full_ = 14;
    float pipe_time_full_ = 100.0f;
    void StartMelee();
    void MeleeHit(const Camera& camera);
    void BreakPipe(const Camera& camera);
    void UpdateMelee(float dt, const Camera& camera);
    float ShoveOut() const;
    float ShoveWind() const;
    float SwingAway() const;
    bool PipeSwingFrame(const Camera& camera, glm::mat4& grip) const;
    glm::mat4 PipeInGrip() const;
    float PipeHold() const;
    Camera SwingCamera(const Camera& camera) const;
    std::array<glm::vec4, 5> PipeFist() const;
    std::array<glm::vec4, 5> ThrowFingers() const;
    float ThrowAway() const;
    static glm::mat4 FlareInHand();
    glm::vec3 hand_flare_at_{0.0f};
    bool hand_flare_lit_ = false;
    void AddCarriedDraws(std::vector<DrawItem>& out, const Camera& camera, bool first_person);
    struct BurningFlare {
        uint32_t id = 0;
        glm::vec3 position{0.0f};
        glm::vec3 velocity{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 spin{0.0f};
        float ground = 0.0f;
        float age = 0.0f;
        float life = 24.0f;
        float spark_wait = 0.0f;
        bool landed = false;
    };
    std::vector<BurningFlare> flares_out_;
    float throw_timer_ = -1.0f;
    bool thrown_ = false;
    float flash_click_timer_ = -1.0f;
    bool flash_click_toggled_ = false;
    void UpdateFlashClick(float dt);
    float FlashClickHand3P(glm::mat4& hand) const;
    float FlashClickHand1P(const Camera& camera, glm::mat4& hand) const;
    static std::array<glm::vec4, 5> FlashClickFingers();
    bool shove_told_ = false;
    mutable float rest_turn_ = 0.0f;
    mutable uint64_t rest_turn_frame_ = 0;
    void StartThrow();
    void UpdateThrow(float dt, const Camera& camera);
    float ThrowHand(const Camera& camera, glm::mat4& hand) const;
    void UpdateFlares(float dt);
    bool InFlareLight(const glm::vec3& p) const;
    int MaxFlares() const { return 2 + boons_[SurvivalBoon::FlarePouch]; }
    static float KindScaleOf(SurvivalEnemyKind k);
    static float KindWindup(SurvivalEnemyKind k);
    static float KindMassOf(SurvivalEnemyKind k);
    struct Task {
        SurvivalMission kind = SurvivalMission::None;
        bool done = false;
        bool lost = false;
        std::vector<glm::vec3> spots;
        std::vector<bool> taken;
        glm::vec3 facing{0.0f, 0.0f, 1.0f};
        float hold = 0.0f;
        float age = 0.0f;
        bool perched = false;
        bool at_house = false;
        bool tuned = false;
        float spoken = 0.0f;
        uint32_t voice = 0;
        uint32_t noise = 0;
        float door_wait = 0.0f;
        float open = 1.0f;
        bool offer_due = false;
    };
    Task task_;
    void StartTask();
    void UpdateTask(float dt, uint32_t held);
    void EndTask();
    void FinishTask();
    bool TaskShot(const glm::vec3& origin, const glm::vec3& dir, float reach);
    bool WallSpot(glm::vec3& at, glm::vec3& normal);
    bool PostSpot(glm::vec3& at, glm::vec3& normal);
    bool RoadSpot(glm::vec3& at, float closest, float farthest);
    float DoorSpawn(glm::vec3& at) const;
    void AddTaskDraws(std::vector<DrawItem>& out) const;
    void AddTaskLights(SceneLighting& lighting) const;
    struct TaskModels {
        const GpuMesh* fuse_box = nullptr;
        const GpuMesh* radio = nullptr;
        const GpuMesh* carton = nullptr;
        std::array<const GpuMesh*, 8> pieces{};
        const GpuMesh* lamp_lit = nullptr;
        size_t lamp_globe = kMaxHiddenMeshes;
        int32_t lamp_red = -1;
        const GpuMesh* lamp_out = nullptr;
        const GpuMesh* door = nullptr;
        int32_t door_wood = -1;
    };
    TaskModels task_models_;
    bool task_models_tried_ = false;
    void LoadTaskModels();
    float RadioLift() const;
    float LanternGlass() const;
    float fear_ = 0.0f;
    float phantom_wait_ = 8.0f;
    float whisper_wait_ = 20.0f;
    float false_left_ = 0.0f;
    struct Phantom {
        uint32_t id = 0;
        uint32_t of = 0;
        glm::vec3 position{0.0f};
        float yaw = 0.0f;
        float age = 0.0f;
        float life = 4.0f;
        float fade = -1.0f;
    };
    std::vector<Phantom> phantoms_;
    void UpdateFear(float dt, const Camera& camera);
    void Frighten(float amount);
    void AddPhantomDraws(std::vector<DrawItem>& out) const;
    float LanternShare() const;
    bool boss_due_ = false;
    float boss_flash_ = 0.0f;
    void SpawnBoss();
    void Blink(Enemy& e);
    void UpdateBosses(float dt);
    void Unlock(uint32_t bits);
    void ApplyLooks(bool on);
    std::vector<std::pair<uint32_t, glm::vec4>> metal_colours_;
    glm::vec3 torch_tint_{1.0f};
    uint32_t held_ = 0;
    int forced_mission_ = 0;
    bool NearLitLamp(const glm::vec3& p, float radius) const;
    std::vector<glm::vec2> LitLamps() const;
    float ToTheLight(const glm::vec3& p) const;
    void UpdateDark(float dt);
    void DarkCatch();
    float dark_time_ = 0.0f;
    bool StartSupply();
    void UpdateSupply();
    bool supply_on_ = false;
    glm::vec3 supply_at_{0.0f};
    std::vector<uint32_t> supply_ids_;
    int supply_next_ = 0;
    void SaveCheckpoint(bool quiet = false);
    void LoadCheckpoint(const SurvivalCheckpoint& c);
    void KeepRecord();

    Game& game_;
    bool active_ = false;
    SurvivalDifficulty difficulty_ = SurvivalDifficulty::Normal;
    SurvivalPhase phase_ = SurvivalPhase::Countdown;
    int wave_ = 0;
    int to_spawn_ = 0;
    int spawned_this_wave_ = 0;
    int wave_cap_ = 4;
    float spawn_interval_ = 1.5f;
    float spawn_timer_ = 0.0f;
    float countdown_ = 0.0f;
    int last_tick_ = -1;
    bool lisa_due_ = false;
    uint32_t events_ = 0;
    float fog_level_ = 0.0f;
    float siren_level_ = 0.0f;
    float hell_level_ = 0.0f;
    float time_ = 0.0f;
    float run_time_ = 0.0f;
    uint32_t next_id_ = 1;
    uint32_t rng_ = 0x5EED2014u;
    uint32_t plan_rng_ = 0x6E164Bu;
    uint32_t night_seed_ = 1u;
    int daily_mutator_ = 0;
    bool credits_paid_ = false;
    bool daily_goal_fresh_ = false;
    int missions_done_ = 0;
    int bosses_down_ = 0;
    int last_payout_total_ = 0;
    int last_payout_daily_ = 0;
    SurvivalWavePlan plan_;
    std::vector<Enemy> enemies_;
    std::vector<Pickup> pickups_;
    std::vector<Tracer> tracers_;
    std::vector<Debris> debris_;
    std::vector<Mark> marks_;
    std::array<WeaponState, static_cast<size_t>(SurvivalWeapon::Count)> weapons_{};
    SurvivalWeapon weapon_ = SurvivalWeapon::Flashlight;
    GunAnim gun_;
    float shot_timer_ = 0.0f;
    float fire_buffer_ = 0.0f;
    float reload_timer_ = 0.0f;
    float reload_length_ = 0.0f;
    bool reload_from_empty_ = false;
    float burst_timer_ = 0.0f;
    float muzzle_ = 0.0f;
    float muzzle_roll_ = 0.0f;
    float muzzle_size_ = 1.0f;
    glm::vec3 muzzle_at_{0.0f};
    float burst_light_ = 0.0f;
    float blackout_at_ = -1.0f;
    float blackout_length_ = 0.0f;
    float blackout_time_ = -1.0f;
    float wave_time_ = 0.0f;
    int lamps_out_ = 0;
    float lamp_failing_ = -1.0f;
    float lamp_fail_at_ = -1.0f;
    float north_end_ = 0.0f;
    float night_door_ = 0.0f;
    float midnight_ = -1.0f;
    uint32_t midnight_voice_ = 0;
    uint32_t midnight_noise_ = 0;
    uint32_t own_materials_begin_ = 0;
    uint32_t own_materials_end_ = 0;
    struct Dimmed {
        uint32_t index = 0;
        float strength = 0.0f;
        glm::vec4 colour{1.0f};
    };
    std::vector<Dimmed> dimmed_;
    uint32_t dimmed_scanned_ = 0;
    float dimmed_level_ = 1.0f;
    glm::vec3 dimmed_tint_{1.0f};
    static constexpr uint32_t kNoCrossStreet = 0xFFFFFFFFu;
    static constexpr float kStreetTile = 107.908722f / 17.0f;
    uint32_t cross_street_material_ = kNoCrossStreet;
    MaterialGpu cross_street_own_;
    uint32_t street_material_ = kNoCrossStreet;
    MaterialGpu street_own_;
    std::vector<std::pair<uint32_t, MaterialGpu>> street_fine_;
    std::vector<Vertex> road_own_;
    glm::vec3 burst_at_{0.0f};
    glm::vec3 burst_dir_{0.0f, 0.0f, 1.0f};
    float health_ = 100.0f;
    float max_health_ = 100.0f;
    float stamina_ = 1.0f;
    float stamina_wait_ = 0.0f;
    bool sprinting_ = false;
    uint32_t act_pressed_ = 0;
    bool winded_ = false;
    bool fire_held_ = false;
    bool trigger_dead_ = false;
    float aim_ = 0.0f;
    float aim_held_ = -1.0f;
    bool sights_ = false;
    float sights_w_ = 0.0f;
    float trigger_finger_ = 0.0f;
    glm::vec3 arm_shift_{0.0f};
    float arm_twist_ = 0.0f;
    float speed_scale_ = 1.0f;
    float fov_kick_ = 0.0f;
    float close_body_ = 0.0f;
    float close_body_at_ = 2.0f;
    float shake_ = 0.0f;
    float lean_ = 0.0f;
    glm::vec2 view_kick_{0.0f};
    glm::vec2 view_kick_speed_{0.0f};
    glm::vec2 gun_kick_{0.0f};
    glm::vec2 gun_kick_speed_{0.0f};
    float bloom_ = 0.0f;
    float bob_phase_ = 0.0f;
    float bob_weight_ = 0.0f;
    int bob_steps_ = 0;
    float swap_ = 0.0f;
    glm::vec2 sway_{0.0f};
    glm::vec2 sway_speed_{0.0f};
    float side_lean_ = 0.0f;
    float side_lean_speed_ = 0.0f;
    glm::vec2 look_rate_{0.0f};
    float last_yaw_ = 0.0f;
    float last_pitch_ = 0.0f;
    bool look_primed_ = false;
    float hit_pause_ = 0.0f;
    float dead_fall_ = 0.0f;
    float dead_fall_speed_ = 0.0f;
    glm::vec3 dead_eye_{0.0f};
    float dead_ground_ = 0.0f;
    uint32_t heartbeat_ = 0;
    bool dead_locked_ = false;
    bool catch_ready_ = false;
    bool caught_ = false;
    bool caught_lethal_ = true;
    float caught_damage_ = 0.0f;
    uint32_t caught_by_ = 0;
    glm::vec3 caught_at_{0.0f};
    float caught_yaw_ = 0.0f;
    bool wake_ready_ = false;
    bool waking_ = false;
    bool wake_third_ = false;
    bool wake_locked_ = false;
    bool wake_light_on_ = false;
    glm::vec3 wake_at_{0.0f};
    float wake_yaw_ = 0.0f;
    std::string wake_banner_key_;
    int wake_banner_value_ = 0;
    int best_wave_ = 0;
    int record_ = 0;
    int kills_ = 0;
    int headshots_ = 0;
    int score_ = 0;
    int chain_ = 0;
    float chain_timer_ = 0.0f;
    int best_chain_ = 0;
    float wave_hurt_ = 0.0f;
    int shots_ = 0;
    int hits_ = 0;
    void Score(const Enemy& e, bool head);
    float intensity_ = 0.0f;
    float hurt_recent_ = 0.0f;
    float hot_time_ = 0.0f;
    float calm_time_ = 0.0f;
    float relief_ = 0.0f;
    int surge_ = 0;
    uint32_t surge_noise_ = 0;
    float surge_noise_left_ = 0.0f;
    void UpdateDirector(float dt);
    bool auto_play_ = false;
    bool auto_passive_ = false;
    float heart_clock_ = 0.0f;
    float heart_phase_ = 0.0f;
    uint32_t low_heartbeat_ = 0;
    float ammo_idle_ = 0.0f;
    float since_swap_ = 10.0f;
    struct ScriptStep {
        float at = 0.0f;
        std::string action;
        bool done = false;
    };
    std::vector<ScriptStep> script_;
    uint32_t script_held_ = 0;
    bool frozen_ = false;
    bool aim_at_head_ = false;
    struct JacketSkin {
        glm::vec3 place{0.0f};
        glm::u8vec4 joints{0};
        glm::u8vec4 weights{0};
    };
    std::vector<JacketSkin> jacket_skin_;
    const GpuMesh* shirt_mesh_ = nullptr;
    std::vector<glm::vec3> hoodie_lip_;
    std::vector<glm::vec3> shirt_lip_;
    float body_run_ = 0.0f;
    float raise_ = 0.0f;
    float raise_speed_ = 0.0f;
    float body_recoil_ = 0.0f;
    float body_recoil_speed_ = 0.0f;
    float beam_start_ = 0.0f;
    float spawn_level_ = 0.0f;
    bool spawn_level_known_ = false;
    int kept_wave_ = 0;
    bool kept_break_ = false;
    float boss_wait_ = 0.0f;
    int boss_tries_ = 0;
    float reload_front_ = 0.0f;
    float reload_front_speed_ = 0.0f;
    bool stowing_ = false;
    SurvivalWeapon pending_weapon_ = SurvivalWeapon::Handgun;
    float draw_pick_ = 0.0f;
    float sprint_hold_ = 0.0f;
    int catch_cause_ = static_cast<int>(SurvivalEnemyKind::Lisa);
    int caught_cause_ = static_cast<int>(SurvivalEnemyKind::Lisa);
    bool quiet_ = false;
    bool unhurt_ = false;
    bool spawn_at_set_ = false;
    glm::vec2 spawn_at_{0.0f};
    void RunScript(uint32_t& held, uint32_t& pressed);
    void ScriptAction(const std::string& a, uint32_t& pressed);
    void Studio(int kind);
    bool studio_ = false;
    glm::vec3 studio_lamp_{0.0f};
    float studio_reach_ = 1.0f;
    SurvivalHud hud_;
    std::array<std::vector<glm::mat4>, kSkinSlots> skins_;
    std::array<float, kSkinSlots> slot_free_at_{};
    std::array<bool, kSkinSlots> slot_used_{};
    std::string lisa_model_;
    std::string lisa_parts_;
    const GpuMesh* lisa_mesh_ = nullptr;
    MeshMask lisa_hidden_;
    const anim::Skeleton* lisa_skeleton_ = nullptr;
    const GpuMesh* doll_mesh_ = nullptr;
    const anim::Skeleton* doll_skeleton_ = nullptr;
    struct HitBone {
        int from = -1, to = -1;
        float radius = 0.0f;
    };
    static std::vector<HitBone> HitBonesOf(const anim::Skeleton& skeleton);
    std::vector<HitBone> doll_hit_, lisa_hit_;
    std::unique_ptr<SurvivalPoser> poser_;
    float doll_walk_frames_ = 72.0f;
    float doll_walk_travel_ = 1.5f;
    std::map<std::string, float, std::less<>> root_speed_;
    const GpuMesh* arm_mesh_ = nullptr;
    const GpuMesh* left_arm_mesh_ = nullptr;
    std::array<std::array<const GpuMesh*, 2>, 6> held_arm_mesh_{};
    const GpuMesh* body_mesh_ = nullptr;
    bool arm_tried_ = false;
    std::vector<glm::mat4> arm_skin_;
    std::array<MeshData, 2> arm_dump_;
    bool armory_ready_ = false;
    bool assets_ready_ = false;
    SurvivalModels models_;
    StreetCity city_;
    std::shared_ptr<const audio::Media> empty_, reload_sound_, reload_tactical_, pump_sound_, shell_in_sound_, revolver_reload_, swap_sound_,
        tick_sound_, siren_sound_, lever_sound_, lever_round_sound_, double_reload_, take_one_sound_;
    struct VoiceLine {
        std::shared_ptr<const audio::Media> media;
        std::string name;
        float at = 0.0f;
    };
    std::map<std::string, std::shared_ptr<const audio::Media>, std::less<>> voices_;
    std::deque<VoiceLine> voice_queue_;
    uint32_t voice_playing_ = 0, voice_noise_ = 0;
    float voice_quiet_ = 0.0f, voice_duck_db_ = 0.0f;
    std::array<Cue, static_cast<size_t>(SurvivalWeapon::Count)> shot_;
    std::array<std::vector<survival_acoustics::Source>, static_cast<size_t>(SurvivalWeapon::Count)> shot_sources_;
    int shots_dumped_ = 0;
    Cue confirm_, confirm_head_, confirm_kill_;
    Cue pounce_, swing_;
    Cue hit_, gore_, hurt_, fall_, body_fall_, walker_step_, crawl_, rise_, stomp_, thud_, lisa_laugh_, lisa_breath_, lisa_run_, pickup_,
        heal_cue_, landing_, impact_, armor_, glass_, casing_, shell_, magazine_, lights_fail_, lights_down_, lights_on_, burst_, bell_;
    int tolls_left_ = 0;
    float toll_wait_ = 0.0f;
    glm::vec3 bell_at_{0.0f};
    uint32_t sound_rng_ = 0x51A7E5u;
    float beam_reach_ = 0.0f;
    bool physics_ = true;
    std::map<uint32_t, anim::PhysicalBody> bodies_;
    std::map<uint32_t, anim::Ragdoll> enemy_ragdolls_;
    std::vector<glm::vec3> joint_world_;
    anim::Ragdoll player_ragdoll_;
    bool player_fall_heard_ = false;
    glm::vec3 last_hit_from_{0.0f};
    bool has_last_hit_from_ = false;
    struct DroppedGun {
        bool active = false;
        SurvivalWeapon weapon = SurvivalWeapon::Handgun;
        glm::vec3 pos{0.0f};
        glm::vec3 vel{0.0f};
        glm::quat rot{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 spin{0.0f};
        float ground = 0.0f;
        int bounces = 0;
    };
    DroppedGun dropped_gun_;
    mutable float aim_cache_time_ = -1.0f;
    mutable glm::vec3 aim_cache_pos_{0.0f};
    mutable glm::vec3 aim_cache_dir_{0.0f, 0.0f, 1.0f};
    mutable glm::vec3 aim_cache_point_{0.0f};
};

}
