#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/log.h"
#include "engine/physics/collision_world.h"
#include "engine/render/camera.h"
#include "game/game.h"
#include "game/player.h"
#include "game/survival/survival.h"

namespace pt::game {

namespace {

constexpr float kPi = 3.14159265f;

float Smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

constexpr float kShoveTime = 0.4f;
constexpr float kShoveLands = 0.11f;
constexpr float kSwingTime = 0.56f;
constexpr float kSwingLands = 0.22f;
constexpr float kShoveWind = 0.15f;
constexpr float kShoveCost = 0.3f;
constexpr float kShoveBreath = 1.2f;
constexpr float kShoveBraced = 4.0f;
float MeleePace(bool pipe, float clock) {
    if (pipe) return clock < 0.15f ? 0.5f : clock < 0.24f ? 1.0f : 0.6f;
    return clock < kShoveLands ? 0.8f : 0.6f;
}

struct SwingKey {
    float t;
    glm::vec3 at;
    glm::vec3 dir;
    glm::vec3 face;
};
constexpr SwingKey kSwingView[] = {
    {0.0f, {-0.26f, -0.5f, 0.25f}, {-0.15f, -0.9f, -0.4f}, {0.0f, -0.4f, 0.9f}},
    {0.14f, {-0.25f, -0.03f, 0.17f}, {-0.45f, 0.55f, -0.7f}, {0.3f, 0.3f, 0.9f}},
    {0.18f, {-0.18f, -0.06f, 0.32f}, {-0.35f, 0.7f, 0.6f}, {0.6f, -0.3f, 0.6f}},
    {kSwingLands, {-0.04f, -0.13f, 0.42f}, {0.75f, 0.25f, 0.6f}, {0.9f, -0.4f, 0.0f}},
    {0.32f, {0.18f, -0.32f, 0.36f}, {0.85f, -0.25f, -0.45f}, {0.1f, -0.45f, -0.9f}},
    {kSwingTime, {0.26f, -0.65f, 0.22f}, {0.5f, -0.6f, -0.6f}, {-0.3f, -0.6f, -0.7f}},
};
constexpr SwingKey kSwingBody[] = {
    {0.0f, {-0.26f, -0.5f, 0.25f}, {-0.15f, -0.9f, -0.4f}, {0.0f, -0.4f, 0.9f}},
    {0.07f, {-0.3f, -0.25f, 0.08f}, {-0.25f, 0.1f, -0.95f}, {0.1f, -0.95f, 0.1f}},
    {0.15f, {-0.3f, 0.1f, -0.02f}, {-0.35f, 0.6f, -0.72f}, {0.4f, 0.6f, 0.5f}},
    {0.185f, {-0.22f, 0.04f, 0.34f}, {-0.6f, 0.65f, 0.45f}, {0.75f, 0.0f, 0.6f}},
    {kSwingLands, {-0.02f, -0.22f, 0.5f}, {0.3f, -0.05f, 0.95f}, {0.6f, -0.75f, -0.15f}},
    {0.32f, {0.22f, -0.46f, 0.4f}, {0.62f, -0.5f, -0.6f}, {0.2f, -0.7f, -0.6f}},
    {0.44f, {0.1f, -0.58f, 0.4f}, {0.3f, -0.75f, -0.3f}, {-0.1f, -0.6f, -0.7f}},
    {kSwingTime, {-0.12f, -0.58f, 0.34f}, {-0.05f, -0.9f, -0.3f}, {0.0f, -0.4f, 0.9f}},
};

glm::quat KeyTurn(const SwingKey& k) {
    const glm::vec3 y = glm::normalize(k.dir);
    const glm::vec3 z = glm::normalize(k.face - glm::dot(k.face, y) * y);
    return glm::quat_cast(glm::mat3(glm::cross(y, z), y, z));
}

}

void SurvivalNight::StartMelee() {
    if (melee_timer_ >= 0.0f || hud_.dead || swap_ > 0.3f || reload_timer_ > 0.0f) return;
    sights_ = false;
    if (pipe_hits_ > 0) {
        melee_pipe_ = true;
        melee_timer_ = 0.0f;
        melee_wind_ = 0.0f;
        melee_done_ = false;
        melee_whoosh_ = false;
        return;
    }
    if (weapon_ == SurvivalWeapon::Flashlight) return;
    const bool on_face = std::any_of(enemies_.begin(), enemies_.end(), [](const Enemy& e) { return e.kind == SurvivalEnemyKind::Crawler && e.state == Enemy::State::Striking && e.latched; });
    if (stamina_ < kShoveCost && !on_face) return;
    stamina_ = std::max(0.0f, stamina_ - kShoveCost);
    stamina_wait_ = kShoveBreath;
    LogInfo("survival: shove ({:.0f} % stamina left)", stamina_ * 100.0f);
    if (stamina_ < 0.15f && !winded_) {
        winded_ = true;
        LogInfo("survival: winded by a shove");
    }
    melee_pipe_ = false;
    melee_timer_ = 0.0f;
    melee_wind_ = kShoveWind;
    melee_done_ = false;
    melee_whoosh_ = false;
}

void SurvivalNight::MeleeHit(const Camera& camera) {
    const float reach = melee_pipe_ ? 2.4f : 1.9f;
    const float cone = std::cos((melee_pipe_ ? 45.0f : 35.0f) * kPi / 180.0f);
    glm::vec3 ahead = camera.Forward();
    ahead.y = 0.0f;
    ahead = glm::length(ahead) > 1e-3f ? glm::normalize(ahead) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 eye = camera.position;
    const glm::vec3 feet = game_.GetPlayer().Feet();
    const float power = 1.0f + 0.4f * static_cast<float>(boons_[SurvivalBoon::StoppingPower]);
    int struck = 0;
    std::vector<std::pair<float, Enemy*>> near_ones;
    for (Enemy& e : enemies_) {
        if (e.state == Enemy::State::Dying || e.state == Enemy::State::Rising) continue;
        if (!(e.kind == SurvivalEnemyKind::Crawler && e.state == Enemy::State::Striking && e.latched)) {
            if (std::abs(e.position.y - feet.y) > 1.6f) continue;
            const glm::vec3 from = feet + glm::vec3(0.0f, 1.3f, 0.0f);
            const glm::vec3 line = e.position + glm::vec3(0.0f, e.kind == SurvivalEnemyKind::Crawler ? 0.45f : 1.1f, 0.0f) - from;
            const float far_off = glm::length(line);
            RayHit wall;
            if (far_off > 0.6f && game_.Collision().Raycast(from, line / far_off, far_off - 0.3f, wall)) continue;
        }
        glm::vec3 to = e.position - feet;
        to.y = 0.0f;
        const float d = glm::length(to);
        const bool on_face = e.kind == SurvivalEnemyKind::Crawler && e.state == Enemy::State::Striking && e.latched;
        if (!on_face && (d > reach + 0.3f * KindScaleOf(e.kind) || (d > 0.3f && glm::dot(to / d, ahead) < cone))) continue;
        near_ones.emplace_back(d, &e);
    }
    std::sort(near_ones.begin(), near_ones.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [d, ep] : near_ones) {
        if (struck >= (melee_pipe_ ? 3 : 2)) break;
        Enemy& e = *ep;
        const glm::vec3 chest = e.position + glm::vec3(0.0f, e.kind == SurvivalEnemyKind::Crawler ? 0.45f : 1.2f, 0.0f);
        glm::vec3 dir = chest - eye;
        dir = glm::length(dir) > 1e-3f ? glm::normalize(dir) : ahead;
        if (e.kind == SurvivalEnemyKind::Crawler && e.state == Enemy::State::Striking) {
            const float fly_at = KindWindup(e.kind);
            if (e.timer >= fly_at) {
                e.latched = false;
                e.struck = true;
                e.timer = fly_at + e.fly + 0.001f;
                LogInfo("survival: a crawler thrown off");
            }
        }
        glm::vec3 back = e.position - feet;
        back.y = 0.0f;
        back = glm::length(back) > 1e-3f ? glm::normalize(back) : ahead;
        const float mass = KindMassOf(e.kind);
        const bool braced = !melee_pipe_ && e.shoved > 0.0f;
        const float give = melee_pipe_ ? 1.0f : (braced ? 0.35f : 1.0f) * (struck == 0 ? 1.0f : 0.6f);
        e.knock += back * std::min(6.0f, (melee_pipe_ ? 5.5f : 4.0f) * give * power / std::sqrt(mass));
        e.stagger = std::max(e.stagger, (melee_pipe_ ? 0.7f : 0.6f) * give * power / std::sqrt(mass));
        e.bend_speed += glm::vec2(0.0f, -(melee_pipe_ ? 12.0f : 8.0f) * give / mass);
        if (!melee_pipe_) {
            if (braced) LogInfo("survival: a shove on a braced body");
            e.shoved = kShoveBraced;
        }
        if (!braced && e.state == Enemy::State::Striking && e.kind != SurvivalEnemyKind::Giant && e.kind != SurvivalEnemyKind::Crawler) {
            e.state = Enemy::State::Chasing;
            e.timer = 0.0f;
            e.strike_cooldown = std::max(e.strike_cooldown, 0.8f);
        }
        const float amount = melee_pipe_ ? 230.0f : 12.0f;
        const bool head = melee_pipe_ && e.kind != SurvivalEnemyKind::Crawler && Rand() < 0.35f;
        Damage(e, amount, eye, chest, dir, head, false);
        Play(melee_pipe_ ? thud_ : hit_, &chest, melee_pipe_ ? 2.0f : 0.0f, melee_pipe_ ? 0.95f + 0.15f * SoundRand() : 1.2f);
        ++struck;
    }
    if (struck > 0) {
        shake_ = std::min(1.0f, shake_ + (melee_pipe_ ? 0.5f : 0.3f));
        hit_pause_ = std::max(hit_pause_, melee_pipe_ ? 0.12f : 0.09f);
        if (melee_pipe_) {
            --pipe_hits_;
            if (pipe_hits_ <= 0) BreakPipe(camera);
        }
    }
    LogInfo("survival: {} lands on {}", melee_pipe_ ? "the pipe" : "the shove", struck);
}

void SurvivalNight::BreakPipe(const Camera& camera) {
    pipe_hits_ = 0;
    pipe_time_ = 0.0f;
    for (int i = 0; i < 2; ++i) {
        Debris d;
        d.kind = i == 0 ? Debris::Kind::PipeHead : Debris::Kind::PipeTail;
        d.id = next_id_++;
        d.position = camera.position + camera.Forward() * 0.5f - camera.Up() * 0.25f + camera.Right() * (i == 0 ? 0.1f : -0.1f);
        d.velocity = camera.Forward() * 1.2f + camera.Right() * (i == 0 ? 0.8f : -0.6f) + glm::vec3(0.0f, 1.2f, 0.0f);
        d.spin = glm::vec3(Rand() - 0.5f, Rand() - 0.5f, Rand() - 0.5f) * 14.0f;
        d.ground = GroundAt(d.position, game_.GetPlayer().Feet().y);
        d.life = 20.0f;
        debris_.push_back(d);
    }
    Play(armor_, nullptr, 1.0f, 1.15f);
    Note("pc_survival_pipe_broke");
    LogInfo("survival: the pipe gives");
}

void SurvivalNight::UpdateMelee(float dt, const Camera& camera) {
    if (!shove_told_ && pipe_hits_ <= 0 && weapon_ != SurvivalWeapon::Flashlight && !hud_.dead && hud_.pickup_note <= 0.0f) {
        const glm::vec3 feet = game_.GetPlayer().Feet();
        for (const Enemy& e : enemies_) {
            if (e.state == Enemy::State::Dying || e.state == Enemy::State::Rising) continue;
            if (glm::distance(e.position, feet) < 3.5f) {
                shove_told_ = true;
                Note("pc_survival_hint_shove", 0, 5.0f);
                break;
            }
        }
    }
    if (pipe_hits_ > 0) {
        pipe_time_ -= dt;
        if (pipe_time_ <= 0.0f && melee_timer_ < 0.0f) BreakPipe(camera);
    }
    if (melee_timer_ < 0.0f) return;
    if (melee_wind_ > 0.0f) {
        melee_wind_ = std::max(0.0f, melee_wind_ - dt);
        if (melee_wind_ > 0.0f) return;
    }
    if (!melee_whoosh_ && (!melee_pipe_ || melee_timer_ >= 0.13f)) {
        melee_whoosh_ = true;
        if (melee_pipe_) {
            Play(swing_, nullptr, 0.0f, 0.9f + 0.2f * SoundRand());
        } else {
            Play(swing_, nullptr, -4.0f, 1.25f);
        }
    }
    melee_timer_ += dt * MeleePace(melee_pipe_, melee_timer_) * (melee_done_ && hit_pause_ > 0.0f ? 0.1f : 1.0f);
    if (!melee_done_ && melee_timer_ >= (melee_pipe_ ? kSwingLands : kShoveLands)) {
        melee_done_ = true;
        MeleeHit(camera);
    }
    if (melee_timer_ >= (melee_pipe_ ? kSwingTime : kShoveTime)) melee_timer_ = -1.0f;
}

float SurvivalNight::ShoveWind() const {
    if (melee_timer_ < 0.0f || melee_pipe_) return 0.0f;
    if (melee_wind_ > 0.0f) return Smooth(1.0f - melee_wind_ / kShoveWind);
    return 1.0f - Smooth(melee_timer_ / 0.06f);
}

float SurvivalNight::ShoveOut() const {
    if (melee_timer_ < 0.0f || melee_pipe_ || melee_wind_ > 0.0f) return 0.0f;
    return Smooth(melee_timer_ / 0.1f) * (1.0f - Smooth((melee_timer_ - 0.16f) / (kShoveTime - 0.16f)));
}

float SurvivalNight::SwingAway() const {
    if (melee_timer_ < 0.0f || !melee_pipe_) return 0.0f;
    return Smooth(melee_timer_ / 0.08f) * (1.0f - Smooth((melee_timer_ - (kSwingTime - 0.14f)) / 0.14f));
}

bool SurvivalNight::PipeSwingFrame(const Camera& camera, glm::mat4& grip) const {
    if (melee_timer_ < 0.0f || !melee_pipe_) return false;
    const float t = melee_timer_;
    const bool body = BodyHoldsGun();
    const SwingKey* keys = body ? kSwingBody : kSwingView;
    const size_t count = body ? std::size(kSwingBody) : std::size(kSwingView);
    size_t k = 0;
    while (k + 2 < count && t > keys[k + 1].t) ++k;
    const SwingKey& a = keys[k];
    const SwingKey& b = keys[k + 1];
    const float u = std::clamp((t - a.t) / (b.t - a.t), 0.0f, 1.0f);
    const float s = !body ? (k >= 1 && k <= 3 ? u * (2.0f - u) : Smooth(u))
                          : k == 1 || k == 4 ? u * (2.0f - u) : k == 2 ? u * u : k == 3 ? u : Smooth(u);
    glm::vec3 at = glm::mix(a.at, b.at, s);
    if (body && SurvivalLongGun(weapon_) && t > kSwingLands) {
        const float past = Smooth((t - kSwingLands) / 0.08f) * (1.0f - Smooth((t - 0.44f) / (kSwingTime - 0.44f)));
        at = glm::mix(at, glm::vec3(0.06f, -0.3f, 0.44f), 0.65f * past);
    }
    const glm::quat qa = KeyTurn(a);
    const glm::quat qb = KeyTurn(b);
    glm::mat4 view(1.0f);
    view[0] = glm::vec4(-camera.Right(), 0.0f);
    view[1] = glm::vec4(camera.Up(), 0.0f);
    view[2] = glm::vec4(camera.Forward(), 0.0f);
    view[3] = glm::vec4(camera.position, 1.0f);
    grip = view * glm::translate(glm::mat4(1.0f), at) * glm::mat4_cast(glm::slerp(qa, qb, s));
    return true;
}

glm::mat4 SurvivalNight::PipeInGrip() const {
    const SurvivalGunModel& hand = models_.guns[static_cast<size_t>(SurvivalWeapon::Handgun)];
    return glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.05f, -0.018f)) * glm::rotate(glm::mat4(1.0f), hand.rake - kPi * 0.5f, glm::vec3(1, 0, 0));
}

float SurvivalNight::PipeHold() const {
    if (melee_timer_ < 0.0f || !melee_pipe_) return 0.0f;
    return Smooth(melee_timer_ / 0.05f) * (1.0f - Smooth((melee_timer_ - (kSwingTime - 0.12f)) / 0.12f));
}

Camera SurvivalNight::SwingCamera(const Camera& camera) const {
    if (!BodyHoldsGun()) return camera;
    const Player& player = game_.GetPlayer();
    Camera own = player.MakeCamera();
    own.position += player.DrawnOffset();
    return own;
}

std::array<glm::vec4, 5> SurvivalNight::PipeFist() const {
    std::array<glm::vec4, 5> shape = models_.guns[static_cast<size_t>(SurvivalWeapon::Handgun)].fingers;
    shape[1] = shape[2];
    return shape;
}

void SurvivalNight::StartThrow() {
    if (throw_timer_ >= 0.0f || flares_ <= 0 || hud_.dead) return;
    throw_timer_ = 0.0f;
    thrown_ = false;
}

float SurvivalNight::ThrowAway() const {
    if (throw_timer_ < 0.0f) return 0.0f;
    return Smooth(throw_timer_ / 0.08f) * (1.0f - Smooth((throw_timer_ - 0.42f) / 0.13f));
}

std::array<glm::vec4, 5> SurvivalNight::ThrowFingers() const {
    constexpr std::array<glm::vec4, 5> kOpen{glm::vec4(0.25f, 0.15f, 0.2f, 0.1f), glm::vec4(-0.04f, 0.18f, 0.22f, 0.12f),
                                             glm::vec4(0.0f, 0.26f, 0.3f, 0.16f), glm::vec4(0.03f, 0.34f, 0.36f, 0.2f),
                                             glm::vec4(0.06f, 0.42f, 0.4f, 0.24f)};
    const std::array<glm::vec4, 5> fist = PipeFist();
    const float open = Smooth((throw_timer_ - 0.26f) / 0.07f);
    std::array<glm::vec4, 5> shape{};
    for (size_t i = 0; i < shape.size(); ++i) shape[i] = glm::mix(fist[i], kOpen[i], open);
    return shape;
}

float SurvivalNight::ThirdPersonLunge() const {
    if (melee_timer_ < 0.0f || hud_.dead) return 0.0f;
    if (melee_pipe_) {
        const float t = melee_timer_;
        const float wind = Smooth(t / 0.15f) * (1.0f - Smooth((t - 0.15f) / 0.07f));
        const float blow = Smooth((t - 0.15f) / 0.08f) * (1.0f - Smooth((t - 0.3f) / (kSwingTime - 0.3f)));
        return 0.24f * blow - 0.05f * wind;
    }
    return 0.2f * ShoveOut() - 0.05f * ShoveWind();
}

glm::vec3 SurvivalNight::MeleeBody() const {
    glm::vec3 lean(0.0f);
    if (melee_timer_ >= 0.0f && melee_pipe_) {
        const float t = melee_timer_;
        const float wind = Smooth(t / 0.15f) * (1.0f - Smooth((t - 0.15f) / 0.07f));
        const float blow = Smooth((t - 0.15f) / 0.08f) * (1.0f - Smooth((t - 0.32f) / (kSwingTime - 0.32f)));
        lean = glm::vec3(-0.1f, -0.7f, -0.1f) * wind + glm::vec3(0.38f, 0.8f, 0.14f) * blow;
    } else if (melee_timer_ >= 0.0f) {
        lean = (SurvivalLongGun(weapon_) ? glm::vec3(0.3f, 0.5f, 0.06f) : glm::vec3(0.26f, -0.42f, 0.0f)) * ShoveOut();
        lean += (SurvivalLongGun(weapon_) ? glm::vec3(-0.07f, -0.32f, 0.0f) : glm::vec3(-0.05f, 0.3f, 0.0f)) * ShoveWind();
    }
    if (throw_timer_ >= 0.0f) {
        const float t = throw_timer_;
        const float back = Smooth(t / 0.16f) * (1.0f - Smooth((t - 0.16f) / 0.1f));
        const float through = Smooth((t - 0.16f) / 0.12f) * (1.0f - Smooth((t - 0.3f) / 0.25f));
        lean += glm::vec3(0.04f, 0.22f, 0.0f) * back + glm::vec3(0.14f, -0.26f, -0.05f) * through;
    }
    lean += glm::vec3(0.08f + 0.42f * draw_pick_, -0.1f, 0.0f) * Smooth(swap_);
    return lean;
}

glm::mat4 SurvivalNight::FlareInHand() {
    return glm::translate(glm::mat4(1.0f), glm::vec3(0.065f, -0.028f, -0.005f)) * glm::rotate(glm::mat4(1.0f), 1.5708f, glm::vec3(0, 1, 0));
}

void SurvivalNight::UpdateThrow(float dt, const Camera& camera) {
    if (throw_timer_ < 0.0f) return;
    throw_timer_ += dt;
    if (!thrown_ && throw_timer_ >= 0.26f) {
        thrown_ = true;
        --flares_;
        BurningFlare f;
        f.id = next_id_++;
        const Camera own = SwingCamera(camera);
        f.position = BodyHoldsGun() && hand_flare_lit_ ? hand_flare_at_ : own.position + own.Forward() * 0.5f - own.Right() * 0.12f - own.Up() * 0.12f;
        f.velocity = own.Forward() * 9.0f + glm::vec3(0.0f, 4.2f, 0.0f);
        f.spin = glm::vec3(Rand() - 0.5f, 0.0f, Rand() - 0.5f) * 10.0f;
        f.ground = GroundAt(f.position + own.Forward() * 6.0f, game_.GetPlayer().Feet().y);
        flares_out_.push_back(f);
        Play(burst_, nullptr, -4.0f, 0.7f);
        LogInfo("survival: a flare thrown ({} left)", flares_);
    }
    if (throw_timer_ >= 0.55f) throw_timer_ = -1.0f;
}

float SurvivalNight::ThrowHand(const Camera& camera, glm::mat4& hand) const {
    if (throw_timer_ < 0.0f) return 0.0f;
    const float t = throw_timer_;
    const bool outside = BodyHoldsGun();
    const glm::vec3 back = outside ? glm::vec3(0.42f, -0.62f, -0.08f) : glm::vec3(0.34f, -0.66f, -0.22f), out(0.2f, -0.22f, 0.46f), low = outside ? glm::vec3(0.33f, -0.68f, 0.2f) : glm::vec3(0.27f, -0.68f, 0.18f);
    glm::vec3 at = t < 0.16f ? glm::mix(low, back, Smooth(t / 0.16f)) : t < 0.28f ? glm::mix(back, out, Smooth((t - 0.16f) / 0.12f))
                                                                                  : glm::mix(out, low, Smooth((t - 0.28f) / 0.14f));
    if (outside && t >= 0.16f && t < 0.28f) {
        const float u = (t - 0.16f) / 0.12f;
        at.x += 0.1f * 4.0f * u * (1.0f - u);
    }
    glm::mat4 view(1.0f);
    view[0] = glm::vec4(-camera.Right(), 0.0f);
    view[1] = glm::vec4(camera.Up(), 0.0f);
    view[2] = glm::vec4(camera.Forward(), 0.0f);
    view[3] = glm::vec4(camera.position, 1.0f);
    glm::mat4 up(1.0f), in(1.0f);
    up[0] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    up[1] = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
    up[2] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    in[0] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    in[1] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    in[2] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    const glm::quat turn = glm::slerp(glm::quat_cast(glm::mat3(up)), glm::quat_cast(glm::mat3(in)), Smooth((t - 0.27f) / 0.13f));
    hand = view * glm::translate(glm::mat4(1.0f), at) * glm::mat4_cast(turn);
    return ThrowAway();
}

void SurvivalNight::UpdateFlares(float dt) {
    for (BurningFlare& f : flares_out_) {
        f.age += dt;
        if (!f.landed) {
            f.velocity.y -= 9.8f * dt;
            f.position += f.velocity * dt;
            f.rotation = glm::normalize(f.rotation * glm::quat(f.spin * dt));
            if (f.position.y <= f.ground + 0.015f && f.velocity.y < 0.0f) {
                f.position.y = f.ground + 0.015f;
                if (std::abs(f.velocity.y) > 1.5f) {
                    f.velocity = glm::vec3(f.velocity.x * 0.35f, -f.velocity.y * 0.25f, f.velocity.z * 0.35f);
                    f.spin *= 0.4f;
                    Play(landing_, &f.position, -2.0f, 1.15f);
                } else {
                    f.landed = true;
                    const glm::vec3 along = glm::normalize(glm::vec3(f.rotation * glm::vec3(0.0f, 0.0f, 1.0f)) * glm::vec3(1.0f, 0.0f, 1.0f) +
                                                           glm::vec3(1e-3f, 0.0f, 0.0f));
                    f.rotation = glm::quatLookAt(-along, glm::vec3(0.0f, 1.0f, 0.0f));
                }
            }
        }
        f.spark_wait -= dt;
        if (f.age < f.life - 1.0f && f.spark_wait <= 0.0f) {
            f.spark_wait = 0.025f + 0.025f * Rand();
            const glm::vec3 nozzle_dir = glm::normalize(glm::vec3(f.rotation * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
            const glm::vec3 tip = f.position + nozzle_dir * 0.105f;
            const int burst = Rand() < 0.45f ? 3 : 2;
            for (int k = 0; k < burst; ++k) {
                Debris d;
                d.kind = Debris::Kind::Spark;
                d.id = next_id_++;
                d.position = tip + glm::vec3(0.0f, 0.01f, 0.0f);
                d.velocity = nozzle_dir * (1.2f + 1.8f * Rand()) + glm::vec3((Rand() - 0.5f) * 2.4f, 1.6f + 2.2f * Rand(), (Rand() - 0.5f) * 2.4f);
                d.ground = f.ground;
                d.life = 0.28f + 0.36f * Rand();
                debris_.push_back(d);
            }
        }
        if (f.age < f.life && f.landed) {
            for (Enemy& e : enemies_) {
                if (e.state == Enemy::State::Dying) continue;
                if (glm::distance(glm::vec2(e.position.x, e.position.z), glm::vec2(f.position.x, f.position.z)) > 1.3f) continue;
                e.burn += dt;
                if (e.burn >= 0.5f) {
                    e.burn = 0.0f;
                    Damage(e, 12.0f, f.position, e.position + glm::vec3(0.0f, 0.8f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), false, false);
                }
            }
        }
    }
    std::erase_if(flares_out_, [](const BurningFlare& f) { return f.age > f.life; });
}

bool SurvivalNight::InFlareLight(const glm::vec3& p) const {
    for (const BurningFlare& f : flares_out_) {
        if (f.age < f.life - 1.0f && glm::distance(f.position, p) < 12.0f) return true;
    }
    return false;
}

}
