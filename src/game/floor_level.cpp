#include "game/floor_level.h"

#include <algorithm>

#include "engine/core/log.h"
#include "game/game.h"

namespace pt::game {
namespace {

constexpr const char* kFloorNames[] = {"f000", "f010", "f005", "f020", "f030", "f040", "f060", "f050",
                                       "f070", "f080", "f090", "f100", "f110", "f120", "f160", "ending"};

constexpr uint32_t kRandomSfx[] = {0x52681170, 0x84541725, 0x5EDD03D2, 0xABE1690F, 0x9D850999, 0x4B98D114, 0xF028C99A, 0x688DB123};

struct FloorSetup {
    const char* floor;
    int ocho_state;
    int ceil_lamp;
    int freezer;
    int baby;
    bool mirror;
    int light_row;
};

constexpr FloorSetup kFloorSetups[] = {
    {"f000", 0, -1, -1, -1, false, 0},  {"f005", 0, 1, 0, 1, false, 1},   {"f010", 0, 1, 0, 1, false, 2},   {"f020", 0, 1, 0, 1, false, 3},
    {"f030", 0, 1, 0, 1, true, 4},      {"f040", 0, 1, 0, 1, true, 5},    {"f050", 2, 1, 0, 1, true, 6},    {"f060", 0, 1, 0, 1, true, 7},
    {"f070", 0, 1, 0, 1, false, 8},     {"f080", 0, 0, 1, 1, false, 9},   {"f090", 1, 0, 1, 1, true, 10},   {"f100", 0, 1, 0, 1, true, 11},
    {"f110", 0, 1, 0, 1, true, 12},     {"f120", 0, 1, 0, 1, true, 13},   {"f160", 1, 1, 0, 1, true, 14},   {"ending", 0, 0, 0, 0, false, -1},
};

}

FloorLevel::FloorLevel(Game& game) : game_(game) {
    table_.fill("ERROR");
    for (size_t i = 0; i < std::size(kFloorNames); ++i) {
        table_[i] = kFloorNames[i];
    }
}

void FloorLevel::ResetLoopCount() {
    loop_count_ = 1;
}

void FloorLevel::ProcessPending() {
    if (pending_ & 1) {
        NextFloor();
    }
    if (pending_ & 0x10) {
        ResetBaby();
    }
    pending_ = 0;
}

void FloorLevel::Update(float dt) {
    if (!IsCurrentFloorName("ending") && game_.GetPlayer().spawned && game_.GetPlayer().Feet().y < kFallSafetyY) {
        game_.Objects().GimmickLogicControl(GimmickType::Ocho, "Kill");
    }
    RandomEvents(dt);
}

uint32_t FloorLevel::NextRandom() {
    uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 5;
    rng_ = x;
    return x;
}

void FloorLevel::RandomEvents(float dt) {
    if (!IsCurrentFloorName("f160")) {
        return;
    }
    const double chance = dt <= 1.0f / 60.0f + 1e-6f ? kRandomEventChance60 : kRandomEventChance30;
    const double u = NextRandom() / 4294967296.0;
    if (u >= chance) {
        return;
    }
    const uint32_t a = NextRandom();
    const uint32_t b = NextRandom();
    const double r = b / 4294967296.0;
    if (2.0 * r - 1.0 > 0.0) {
        LogInfo("floor: f160 random event: image {}", a % 10);
        game_.Effects().ShowSubliminalImage(static_cast<int>(a % 10));
    } else {
        LogInfo("floor: f160 random event: sfx {:#x}", kRandomSfx[a & 7]);
        game_.PostSoundId(kRandomSfx[a & 7]);
    }
}

void FloorLevel::AddFloorLevel() {
    index_ = std::min(index_ + 1, kTableSize - 1);
}

void FloorLevel::SubFloorLevel() {
    index_ = std::max(index_ - 1, 0);
}

void FloorLevel::GoNextFloor(bool save) {
    pending_ = 1;
    if (save) {
        save_floor_name_ = CurrentFloorName();
        game_.RequestSave();
    }
    LogInfo("floor: GoNextFloor from {} (save {}, save floor {})", CurrentFloorName(), save, save_floor_name_);
}

void FloorLevel::NextFloor() {
    const std::string current = CurrentFloorName();
    game_.SpeedrunFloorLeft(current, loop_count_);
    game_.ClearPendingSubtitles();
    bool advance = true;
    if (current == "f050") {
        advance = game_.Nazo().IsCleared(NazoId::XMark);
    } else if (current == "f090") {
        advance = game_.Nazo().IsCleared(NazoId::Hello);
    } else if (current == "f160") {
        advance = game_.Nazo().IsCleared(NazoId::TrueEnd);
    }
    if (advance) {
        AddFloorLevel();
        loop_count_ = 1;
    } else {
        ++loop_count_;
    }
    LogInfo("floor: NextFloor {} -> {} (loop {})", current, CurrentFloorName(), loop_count_);
    if (ModLua* mods = game_.ModScripts()) {
        mods->FloorEnter(CurrentFloorName(), loop_count_);
    }
    game_.ApplyStageTransform();
    game_.OnFloorReached(CurrentFloorName(), loop_count_);
    game_.OnBrowseFloorEntered();
    RelocateGimmicks();
    ResetBaby();
}

void FloorLevel::OnStartGame() {
    game_.Objects().SetGimmickEnabled(GimmickType::Bag, true);
    ResetBaby();
}

void FloorLevel::ResetBaby() {
    game_.Objects().ResetToLocators();
}

void FloorLevel::ResetOcho() {
    game_.Objects().GimmickLogicControl(GimmickType::Ocho, "None");
    if (IsCurrentFloorName("f050")) {
        game_.SendControllerMessage("Clearf050");
    }
}

void FloorLevel::RelocateGimmicks() {
    GameObjects& objects = game_.Objects();
    NazoManager& nazo = game_.Nazo();
    game_.FloorEnvironment();
    objects.SetGimmickEnabled(GimmickType::Bag, false);
    const std::string& floor = CurrentFloorName();
    const FloorSetup* setup = nullptr;
    for (const FloorSetup& s : kFloorSetups) {
        if (floor == s.floor) {
            setup = &s;
            break;
        }
    }
    if (floor == "f000" || floor == "f010") {
        for (NazoId id : {NazoId::XMark, NazoId::Photo, NazoId::Peephole, NazoId::Hello, NazoId::TrueEnd}) {
            nazo.Deactivate(id);
        }
    } else if (floor == "f020" || floor == "f030" || floor == "f040" || floor == "f060" || floor == "f120") {
        nazo.Activate(NazoId::Photo);
    } else if (floor == "f050") {
        nazo.Activate(NazoId::Photo);
        nazo.Activate(NazoId::XMark);
    } else if (floor == "f070" || floor == "f080") {
        nazo.Activate(NazoId::Photo);
        nazo.Prepare(NazoId::Hello);
    } else if (floor == "f090") {
        nazo.Activate(NazoId::Photo);
        nazo.Activate(NazoId::Hello);
    } else if (floor == "f100") {
        nazo.Activate(NazoId::Photo);
        nazo.ForceClear(NazoId::Hello);
    } else if (floor == "f110") {
        nazo.Activate(NazoId::Photo);
        nazo.Activate(NazoId::Peephole);
    } else if (floor == "f160") {
        nazo.Activate(NazoId::Photo);
        nazo.Deactivate(NazoId::TrueEnd);
        nazo.ApplyVisuals(NazoId::TrueEnd);
        nazo.StartVoiceRecognition();
    } else if (floor == "ending") {
        nazo.StopVoiceRecognition();
    }
    if (setup) {
        int ocho = setup->ocho_state;
        if (floor == "f050") {
            ocho = loop_count_ < 2 ? 2 : 3;
        }
        static constexpr const char* kOchoStates[] = {"None", "Warp", "Chase", "KillChase", "Dash", "Kill"};
        objects.GimmickLogicControl(GimmickType::Ocho, kOchoStates[ocho]);
        if (setup->ceil_lamp >= 0) {
            objects.SetGimmickEnabled(GimmickType::CeilLamp, setup->ceil_lamp != 0);
        }
        if (setup->freezer >= 0) {
            objects.SetGimmickEnabled(GimmickType::Freezer, setup->freezer != 0);
        }
        if (setup->baby >= 0) {
            objects.SetGimmickEnabled(GimmickType::Baby, setup->baby != 0);
        }
        if (floor == "f080") {
            objects.PlayGimmickMotion(GimmickType::Freezer, "Freezer", false);
        } else if (floor == "f090") {
            objects.PlayGimmickMotion(GimmickType::Freezer, "FreezerStrong", false);
        } else if (floor == "f100" || floor == "f110") {
            objects.PlayGimmickMotion(GimmickType::CeilLamp, "CeilLampStrong", false);
        } else if (floor == "f120") {
            objects.PlayGimmickMotion(GimmickType::CeilLamp, "CeilLamp", true);
            // issue #43: on PS4 the fake crash loop's sink is empty. Played through, the original's ResetToLocators finds the
            // baby's maze B locator first (the f110 stage is still in its list) and the record is hidden once that stage
            // unloads; the port placed it in the new hallway. Kept off on f120 whatever the way in (the loop browser too)
            objects.SetGimmickEnabled(GimmickType::Baby, false);
        } else if (floor == "ending") {
            for (GimmickType type : {GimmickType::Ocho, GimmickType::Baby, GimmickType::CeilLamp, GimmickType::Freezer, GimmickType::Bag}) {
                objects.SetGimmickEnabled(type, false);
            }
        }
        game_.Effects().mirror_capture = setup->mirror;
        if (setup->light_row >= 0) {
            game_.SetFloorLighting(setup->light_row);
        }
    }
    nazo.ApplyVisuals(NazoId::Photo);
    nazo.ApplyVisuals(NazoId::XMark);
    nazo.ApplyVisuals(NazoId::Hello);
    LogInfo("floor: RelocateGimmicks on {} (loop {})", floor, loop_count_);
}

bool FloorLevel::IsCurrentFloorName(std::string_view name) const {
    return table_[index_] == name;
}

void FloorLevel::SetIndex(int index) {
    index_ = std::clamp(index, 0, kTableSize - 1);
}

int FloorLevel::IndexOf(std::string_view name) const {
    for (int i = 0; i < kTableSize - 1; ++i) {
        if (table_[i] == name) {
            return i;
        }
    }
    return -1;
}

bool FloorLevel::SetFloorLevel(std::string_view name) {
    const int index = IndexOf(name);
    index_ = index < 0 ? 0 : index;
    LogInfo("floor: SetFloorLevel {} -> index {}", name, index_);
    return index >= 0;
}

void FloorLevel::SetFloorLevelAndName(int index, std::string_view name) {
    if (index >= 0 && index < kTableSize) {
        table_[index] = std::string(name);
    }
}

}
