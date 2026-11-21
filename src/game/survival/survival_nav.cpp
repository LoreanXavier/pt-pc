#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "engine/core/log.h"
#include "engine/physics/collision_world.h"
#include "game/game.h"
#include "game/player.h"
#include "game/survival/survival.h"

namespace pt::game {

namespace {

constexpr glm::vec2 kWayMin(-18.5f, 23.0f);
constexpr int kWayColumns = 31;
constexpr int kWayRows = 167;
constexpr float kWayCell = 1.0f;
constexpr uint16_t kNoWay = 0xFFFF;
constexpr float kWayStep = 0.6f;
constexpr float kWayWidth = 0.3f;
constexpr int kWayDirs[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};

glm::vec3 CellMiddle(int column, int row, float y) {
    return glm::vec3(kWayMin.x + (static_cast<float>(column) + 0.5f) * kWayCell, y, kWayMin.y + (static_cast<float>(row) + 0.5f) * kWayCell);
}

int CellOf(const glm::vec3& p) {
    const int column = static_cast<int>(std::floor((p.x - kWayMin.x) / kWayCell));
    const int row = static_cast<int>(std::floor((p.z - kWayMin.y) / kWayCell));
    if (column < 0 || column >= kWayColumns || row < 0 || row >= kWayRows) return -1;
    return row * kWayColumns + column;
}

}

void SurvivalNight::BuildWays() {
    if (ways_built_) return;
    ways_built_ = true;
    const auto start = std::chrono::steady_clock::now();
    const float road = game_.GetPlayer().Feet().y;
    std::vector<float> ground(static_cast<size_t>(kWayColumns * kWayRows));
    for (int row = 0; row < kWayRows; ++row) {
        for (int column = 0; column < kWayColumns; ++column) {
            ground[static_cast<size_t>(row * kWayColumns + column)] = GroundAt(CellMiddle(column, row, road), road);
        }
    }
    if (!ground.empty()) {
        std::vector<float> heights = ground;
        std::nth_element(heights.begin(), heights.begin() + static_cast<std::ptrdiff_t>(heights.size() / 2), heights.end());
        spawn_level_ = heights[heights.size() / 2];
        spawn_level_known_ = true;
    }
    way_links_.assign(ground.size(), 0);
    way_steps_.assign(ground.size(), kNoWay);
    int links = 0;
    for (int row = 0; row < kWayRows; ++row) {
        for (int column = 0; column < kWayColumns; ++column) {
            const size_t cell = static_cast<size_t>(row * kWayColumns + column);
            for (int d = 0; d < 4; ++d) {
                const int c = column + kWayDirs[d][0], r = row + kWayDirs[d][1];
                if (c < 0 || c >= kWayColumns || r < 0 || r >= kWayRows) continue;
                const size_t other = static_cast<size_t>(r * kWayColumns + c);
                if (std::abs(ground[cell] - ground[other]) > kWayStep) continue;
                const glm::vec3 from = CellMiddle(column, row, ground[cell]);
                const glm::vec3 to = CellMiddle(c, r, ground[cell]);
                const glm::vec3 dir = glm::normalize(to - from);
                const float length = glm::distance(from, to);
                const glm::vec3 side = glm::vec3(dir.z, 0.0f, -dir.x) * kWayWidth;
                if (Blocked(from, dir, length) || Blocked(from + side, dir, length) || Blocked(from - side, dir, length)) continue;
                way_links_[cell] |= static_cast<uint8_t>(1u << d);
                way_links_[other] |= static_cast<uint8_t>(1u << (d + 4));
                ++links;
            }
        }
    }
    LogInfo("survival: the street's ways, {} links ({:.0f} ms)", links,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

void SurvivalNight::DumpWays(const std::string& path) const {
    if (way_links_.empty()) return;
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return;
    for (int row = kWayRows - 1; row >= 0; --row) {
        std::fprintf(out, "%6.1f ", kWayMin.y + static_cast<float>(row) * kWayCell);
        for (int column = 0; column < kWayColumns; ++column) {
            const size_t cell = static_cast<size_t>(row * kWayColumns + column);
            std::fputc(way_links_[cell] == 0 ? '#' : way_steps_[cell] != kNoWay ? '.' : '+', out);
        }
        std::fputc('\n', out);
    }
    for (int row = 0; row < kWayRows; ++row) {
        std::fprintf(out, "links %d ", row);
        for (int column = 0; column < kWayColumns; ++column) std::fprintf(out, "%02x", way_links_[static_cast<size_t>(row * kWayColumns + column)]);
        std::fputc('\n', out);
    }
    for (float z = 21.0f; z < 190.0f; z += 0.25f) {
        for (float x = -19.5f; x < 13.5f; x += 0.25f) {
            RayHit hit;
            if (!game_.Collision().Raycast(glm::vec3(x, 1.2f, z), glm::vec3(0.0f, -1.0f, 0.0f), 8.0f, hit)) {
                std::fprintf(out, "hole %.2f %.2f\n", x, z);
            } else if (hit.point.y < -0.25f) {
                std::fprintf(out, "low %.2f %.2f %.2f\n", x, z, hit.point.y);
            }
        }
    }
    std::fclose(out);
}

void SurvivalNight::UpdateWays() {
    if (!ways_built_) return;
    glm::vec3 feet = game_.GetPlayer().Feet();
    feet.x = std::clamp(feet.x, kWayMin.x + 0.1f, kWayMin.x + static_cast<float>(kWayColumns) * kWayCell - 0.1f);
    int from = CellOf(feet);
    if (from < 0 || from == way_from_) return;
    way_from_ = from;
    if (way_links_[static_cast<size_t>(from)] == 0) {
        const int column = from % kWayColumns, row = from / kWayColumns;
        int found = -1;
        for (int ring = 1; ring <= 3 && found < 0; ++ring) {
            for (int r = row - ring; r <= row + ring && found < 0; ++r) {
                for (int c = column - ring; c <= column + ring; ++c) {
                    if (c < 0 || c >= kWayColumns || r < 0 || r >= kWayRows || way_links_[static_cast<size_t>(r * kWayColumns + c)] == 0) continue;
                    found = r * kWayColumns + c;
                    break;
                }
            }
        }
        if (found < 0) return;
        from = found;
    }
    std::fill(way_steps_.begin(), way_steps_.end(), kNoWay);
    way_queue_.clear();
    way_queue_.push_back(from);
    way_steps_[static_cast<size_t>(from)] = 0;
    for (size_t at = 0; at < way_queue_.size(); ++at) {
        const int cell = way_queue_[at];
        const int column = cell % kWayColumns, row = cell / kWayColumns;
        const uint8_t out = way_links_[static_cast<size_t>(cell)];
        for (int d = 0; d < 8; ++d) {
            if (!(out & (1u << d))) continue;
            const size_t next = static_cast<size_t>((row + kWayDirs[d][1]) * kWayColumns + column + kWayDirs[d][0]);
            if (way_steps_[next] != kNoWay) continue;
            way_steps_[next] = static_cast<uint16_t>(way_steps_[static_cast<size_t>(cell)] + 1);
            way_queue_.push_back(static_cast<int>(next));
        }
    }
}

bool SurvivalNight::WayDown(const glm::vec3& from, glm::vec3& at) const {
    const int cell = CellOf(from);
    if (cell < 0 || way_steps_.empty()) return false;
    const int column = cell % kWayColumns, row = cell / kWayColumns;
    for (int ring = 2; ring <= 5; ++ring) {
        for (int r = row - ring; r <= row + ring; ++r) {
            for (int c = column - ring; c <= column + ring; ++c) {
                if (c < 0 || c >= kWayColumns || r < 0 || r >= kWayRows || std::max(std::abs(c - column), std::abs(r - row)) != ring) continue;
                if (way_steps_[static_cast<size_t>(r * kWayColumns + c)] == kNoWay) continue;
                const glm::vec3 p = CellMiddle(c, r, game_.GetPlayer().Feet().y);
                at = glm::vec3(p.x, GroundAt(p, p.y), p.z);
                return true;
            }
        }
    }
    return false;
}

bool SurvivalNight::WayOn(const glm::vec3& from, glm::vec3& dir) const {
    if (!ways_built_) return false;
    const int cell = CellOf(from);
    if (cell < 0) return false;
    const int column = cell % kWayColumns, row = cell / kWayColumns;
    const uint8_t out = way_links_[static_cast<size_t>(cell)];
    uint16_t best = way_steps_[static_cast<size_t>(cell)];
    int to = -1;
    for (int d = 0; d < 8; ++d) {
        if (!(out & (1u << d))) continue;
        const int next = (row + kWayDirs[d][1]) * kWayColumns + column + kWayDirs[d][0];
        if (way_steps_[static_cast<size_t>(next)] < best) {
            best = way_steps_[static_cast<size_t>(next)];
            to = next;
        }
    }
    if (to < 0) return false;
    glm::vec3 d = CellMiddle(to % kWayColumns, to / kWayColumns, from.y) - from;
    d.y = 0.0f;
    const float length = glm::length(d);
    if (length < 1e-3f) return false;
    dir = d / length;
    return true;
}

}
