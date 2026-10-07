#include "engine/render/scene_lighting.h"

#include <cmath>

namespace pt::light_math {
namespace {

constexpr double kPi = 3.14159265358979323846;

glm::dvec2 PlanckXy(double t) {
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double x = t >= 4000.0 ? -3.0258468e9 / t3 + 2107038.0 / t2 + 222.6347 / t + 0.24039
                                 : -2.661239e8 / t3 - 234358.0 / t2 + 877.6956 / t + 0.17991;
    const double x2 = x * x;
    const double x3 = x2 * x;
    double y = 0.0;
    if (t >= 4000.0) {
        y = 3.081758 * x3 - 5.873387 * x2 + 3.7511299 * x - 0.37001482;
    } else if (t >= 2222.0) {
        y = -0.9549476 * x3 - 1.3741859 * x2 + 2.09137 * x - 0.16748866;
    } else {
        y = -1.1063814 * x3 - 1.3481102 * x2 + 2.1855583 * x - 0.20219684;
    }
    return {x, y};
}

glm::dvec2 XyToUv(const glm::dvec2& xy) {
    const double d = -2.0 * xy.x + 12.0 * xy.y + 3.0;
    return {4.0 * xy.x / d, 6.0 * xy.y / d};
}

}

glm::vec3 ColorFromTemperature(float temperature, float deflection, float lumen, const glm::vec3& color) {
    const double t = std::max(1000.0, static_cast<double>(temperature));
    const glm::dvec2 uv0 = XyToUv(PlanckXy(t));
    const glm::dvec2 uv1 = XyToUv(PlanckXy(t + 1.0));
    const glm::dvec2 step = uv1 - uv0;
    const double length = std::sqrt(step.x * step.x + step.y * step.y);
    glm::dvec2 uv = uv0;
    if (length > 0.0) {
        uv += static_cast<double>(deflection) * glm::dvec2(uv1.y - uv0.y, uv0.x - uv1.x) / length;
    }
    const double d = 2.0 * uv.x - 8.0 * uv.y + 4.0;
    const double x = 3.0 * uv.x / d;
    const double y = 2.0 * uv.y / d;
    const double big_y = lumen;
    const double big_x = lumen * x / y;
    const double big_z = lumen * (1.0 - x - y) / y;
    const double r = 0.94354063 * (3.240479 * big_x - 1.53715 * big_y - 0.498535 * big_z);
    const double g = -0.969256 * big_x + 1.875991 * big_y + 0.041556 * big_z;
    const double b = 0.95035714 * (0.055648 * big_x - 0.204043 * big_y + 1.057311 * big_z);
    return glm::vec3(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)) * color;
}

float SpotSolidAngle(float umbra_degrees, float penumbra_degrees, float exponent) {
    const double cos_umbra = std::cos(umbra_degrees * kPi / 360.0);
    const double cos_penumbra = std::cos(penumbra_degrees * kPi / 360.0);
    const double omega = 2.0 * kPi * ((1.0 - cos_penumbra) + (cos_penumbra - cos_umbra) / (static_cast<double>(exponent) + 1.0));
    return static_cast<float>(std::max(omega, 1.0e-4));
}

float SourceRadius(float light_size) {
    return std::sin(std::atan(light_size * 0.5f));
}

std::array<float, 9> ShBasis(const glm::vec3& n) {
    return {0.2820948f,
            0.4886025f * n.y,
            0.4886025f * n.z,
            0.4886025f * n.x,
            1.0925484f * n.x * n.y,
            1.0925484f * n.y * n.z,
            0.3153916f * (3.0f * n.z * n.z - 1.0f),
            1.0925484f * n.x * n.z,
            0.5462742f * (n.x * n.x - n.y * n.y)};
}

}
