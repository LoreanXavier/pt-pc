#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <vector>

#include "engine/anim/skeleton.h"

namespace pt::anim {

class PhysicalBody {
public:
    using GroundFn = std::function<float(const glm::vec3&)>;
    using SweepFn = std::function<bool(const glm::vec3& from, const glm::vec3& to, glm::vec3& point, glm::vec3& normal)>;

    bool Build(const Skeleton& skeleton, float size = 1.0f);
    bool Valid() const { return !joints_.empty(); }
    void Follow(const std::vector<glm::vec3>& bone_world, const glm::quat& head_world);
    void Reset() { primed_ = false; asleep_ = false; }
    void Strike(const glm::vec3& at, const glm::vec3& velocity, float radius);
    void SetDrive(float drive) { drive_ = drive; }
    float Drive() const { return drive_; }
    void Step(float dt, const GroundFn& ground, const SweepFn& sweep);
    float Deviation() const { return deviation_; }
    bool Asleep() const { return asleep_; }
    bool Joint(int bone, glm::vec3& out) const;
    glm::vec3 HeadTop() const;
    glm::vec3 Waist() const;
    float TakeImpact();

private:
    struct Particle {
        int bone = -1;
        glm::vec3 x{0.0f};
        glm::vec3 v{0.0f};
        glm::vec3 target{0.0f};
        glm::vec3 last_target{0.0f};
        float inverse_mass = 1.0f;
        float radius = 0.05f;
        float frequency = 3.0f;
    };
    struct Link {
        int a = 0, b = 0;
        float rest = 0.0f;
        float stiffness = 1.0f;
        bool minimum = false;
        bool follow = true;
        bool maximum = false;
    };
    void Add(const Skeleton& skeleton, const char* name, float mass, float radius, float frequency);
    int ParticleOf(int bone) const;
    void SolveLinks(float h);

    std::vector<Particle> joints_;
    std::vector<Link> links_;
    std::vector<int> particle_of_bone_;
    int head_ = -1, top_ = -1, waist_ = -1;
    float drive_ = 1.0f;
    float deviation_ = 0.0f;
    float still_ = 0.0f;
    float impact_ = 0.0f;
    float size_ = 1.0f;
    bool primed_ = false;
    bool asleep_ = false;
};

}
