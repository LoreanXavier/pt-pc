#include "game/nazo.h"

#include <algorithm>
#include <format>

#include "engine/core/log.h"
#include "game/game.h"

namespace pt::game {
namespace {

constexpr std::string_view kNazoNames[] = {"XMark", "Hello", "Peephole", "Photo", "TrueEnd"};

constexpr uint32_t kSoundClear = 0xF27DAC95;
constexpr uint32_t kSoundXMark = 0xA5FDFC86;
constexpr uint32_t kSoundHello = 0x5252493F;
constexpr uint32_t kSoundPeepholeState = 0xDFBA4D10;
constexpr uint32_t kSoundPeepholeStop = 0x3810E4B6;
constexpr uint32_t kSoundPeepholeEndState = 0xF7F83992;
constexpr uint32_t kSoundPhoto = 0xE4B855B2;
constexpr uint32_t kSoundPhoneStop = 0x8FEA05E0;
constexpr uint32_t kSoundPhoneAnswer = 0x80C17CFC;
constexpr uint32_t kSoundPhoneRing = 0x70FEA2B2;
constexpr uint32_t kSoundTrueEndMark = 0xBA6F9BBE;
constexpr uint32_t kSoundTrueEndWait = 0x8D85EE16;
constexpr uint32_t kSoundTrueEndHold = 0xDE941744;

constexpr uint32_t kCaptionXMark = 0x4AF4F3D4;
constexpr uint32_t kCaptionHello = 0x4FA7C4BD;
constexpr uint32_t kCaptionHole = 0xF124B2D3;
constexpr uint32_t kCaptionPhoto = 0xCE86DEBA;

struct PhotoPiece {
    const char* name;
    const char* frame;
    uint32_t bit;
    int image;
};

constexpr PhotoPiece kPieces[] = {
    {"PhotoLisa", "FrameLisa", 0x08, 0}, {"PhotoTree", "FrameTree", 0x10, 1}, {"PhotoGap", "FrameGap", 0x20, 2},
    {"PhotoBath", "FrameBath", 0x40, 3}, {"PhotoStair", "FrameStair", 0x80, 0},
};

}

std::string_view NazoName(NazoId id) {
    return kNazoNames[static_cast<int>(id)];
}

NazoState NazoManager::State(NazoId id) const {
    if (IsCleared(id)) {
        return NazoState::Cleared;
    }
    return IsActive(id) ? NazoState::Active : NazoState::Inactive;
}

void NazoManager::RegisterStage(Stage& stage) {
    for (const auto& file : stage.files) {
        const fox2::DataSetFile& f = *file->file;
        for (const fox2::Entity& e : f.Entities()) {
            if (e.class_name != "NazoManageData") {
                continue;
            }
            for (const auto& [name, target] : f.GetEntityMap(e, "controlAsset")) {
                if (target) {
                    assets_[name].push_back({stage.id, target});
                }
            }
            LogInfo("nazo: NazoManageData of stage {} ({}) registered", stage.id, stage.label);
        }
    }
    const uint32_t xmark = word_[Index(NazoId::XMark)];
    if (xmark != 0) {
        ApplyVisuals(NazoId::XMark);
        LogInfo("nazo: XMark state {:#x} applied to the registered stage {}", xmark, stage.id);
    }
    const uint32_t photo = word_[Index(NazoId::Photo)];
    if (photo != 0) {
        ApplyVisuals(NazoId::Photo);
        LogInfo("nazo: Photo state {:#x} applied to the registered stage {}", photo, stage.id);
    }
    const uint32_t hello = word_[Index(NazoId::Hello)];
    const bool hello_floor = game_.Floor().IsCurrentFloorName("f070") || game_.Floor().IsCurrentFloorName("f080") ||
                             game_.Floor().IsCurrentFloorName("f090") || game_.Floor().IsCurrentFloorName("f100");
    if (hello_floor && (hello == 2 || hello == 4 || hello == 0x200)) {
        ApplyVisuals(NazoId::Hello);
        LogInfo("nazo: Hello state {:#x} applied to the registered stage {}", hello, stage.id);
    }
}

void NazoManager::ForgetStage(const Stage& stage) {
    for (auto& [name, refs] : assets_) {
        std::erase_if(refs, [&](const AssetRef& r) { return r.stage_id == stage.id; });
    }
}

void NazoManager::RestoreCompletedPeephole(Stage& stage) {
    if (!game_.Floor().IsCurrentFloorName("f110") || !IsCleared(NazoId::Peephole)) {
        return;
    }
    const bool has_exit_scripts = std::ranges::any_of(stage.files, [](const auto& file) {
        return std::ranges::any_of(file->message_scripts, [](const MessageScript& script) { return script.message_name == "f110_GoNextFloor"; });
    });
    if (!has_exit_scripts) {
        return;
    }
    const auto hole_text = assets_.find("HoleText");
    if (hole_text == assets_.end() || !std::ranges::any_of(hole_text->second, [&](const AssetRef& ref) { return ref.stage_id == stage.id; })) {
        return;
    }
    /* Load order: the save restores the word before maze B exists, so its scene effects are applied here, once the maze's message scripts have registered. */
    ApplyVisuals(NazoId::Peephole);
    game_.SendControllerMessage("f110_GoNextFloor");
    LogInfo("nazo: restored completed peephole in stage {} ({})", stage.id, stage.label);
}

void NazoManager::SetAssetEnable(std::string_view name, bool on) {
    auto it = assets_.find(name);
    if (it == assets_.end()) {
        return;
    }
    for (const AssetRef& ref : it->second) {
        Stage* stage = game_.Stages().FindById(ref.stage_id);
        if (!stage) {
            continue;
        }
        BodyState& body = stage->Body(ref.entity);
        if (ref.entity->class_name == "StaticModel") {
            if (body.visible != on || body.geom_active != on) {
                body.visible = on;
                body.geom_active = on;
                game_.Stages().MarkVisualsDirty();
                game_.Stages().MarkGeomDirty();
            }
        } else if (ref.entity->class_name == "FxLocatorData") {
            body.visible = on;
            game_.Stages().MarkVisualsDirty();
        } else {
            body.enable = on;
        }
    }
}

bool NazoManager::AssetPosition(std::string_view name, glm::vec3& out) const {
    auto it = assets_.find(name);
    if (it == assets_.end()) {
        return false;
    }
    for (const AssetRef& ref : it->second) {
        Stage* stage = game_.Stages().FindById(ref.stage_id);
        if (!stage || stage->label != "current") {
            continue;
        }
        if (const StageData* file = stage->FileOf(ref.entity)) {
            out = glm::vec3(stage->ToWorld(file->file->WorldTransform(*ref.entity))[3]);
            return true;
        }
    }
    for (const AssetRef& ref : it->second) {
        Stage* stage = game_.Stages().FindById(ref.stage_id);
        if (const StageData* file = stage ? stage->FileOf(ref.entity) : nullptr) {
            out = glm::vec3(stage->ToWorld(file->file->WorldTransform(*ref.entity))[3]);
            return true;
        }
    }
    return false;
}

void NazoManager::Sound2D(uint32_t id) {
    game_.PostSoundId(id);
}

void NazoManager::SoundAt(uint32_t id, std::string_view asset) {
    glm::vec3 position(0.0f);
    if (AssetPosition(asset, position)) {
        game_.PostSoundIdAt(id, position);
    } else {
        game_.PostSoundId(id);
    }
}

void NazoManager::Deactivate(NazoId id) {
    const int n = Index(id);
    active_mask_ &= ~(1u << n);
    word_[n] = 1;
}

void NazoManager::Prepare(NazoId id) {
    const int n = Index(id);
    word_[n] = (id == NazoId::Hello || id == NazoId::Photo) ? 2 : 0;
}

void NazoManager::Activate(NazoId id) {
    const int n = Index(id);
    if (id == NazoId::Peephole) peephole_eye_valid_ = false;
    if (cleared_[n]) {
        return;
    }
    if (id == NazoId::Photo) {
        word_[n] = (word_[n] & ~3u) | 4;
    } else if (id == NazoId::Peephole || id == NazoId::TrueEnd) {
        word_[n] = 1;
    } else {
        word_[n] = 4;
    }
    active_mask_ |= 1u << n;
    hold_timer_ = 0.0f;
    true_end_timer_ = 0.0f;
    peephole_timer_ = 0.0f;
    action_armed_ = false;
    clear_sound_pending_ = false;
    if (id == NazoId::TrueEnd) {
        true_end_steps_ = 0;
    }
    LogInfo("nazo: {} active (word {:#x})", NazoName(id), word_[n]);
}

void NazoManager::ForceClear(NazoId id) {
    static constexpr uint32_t kFinal[] = {8, 0x200, 0x10, 0x1F8, 0x100};
    const int n = Index(id);
    word_[n] = kFinal[n];
    ApplyVisuals(id);
    if (!cleared_[n]) {
        OnClear(id);
    }
}

void NazoManager::ResetState(NazoId id) {
    const int n = Index(id);
    if (id == NazoId::Peephole) peephole_eye_valid_ = false;
    word_[n] = 0;
    cleared_[n] = 0;
    active_mask_ &= ~(1u << n);
    hold_timer_ = 0.0f;
    true_end_timer_ = 0.0f;
    peephole_timer_ = 0.0f;
}

void NazoManager::ResetSession() {
    ResetAllStates();
    clear_sound_timer_ = 0.0f;
    frames_since_hide_ = 0;
    armed_state_ = 0;
    armed_flag_ = 0;
    true_end_steps_ = 0;
    hide_effect_next_frame_ = false;
    jack_heard_ = false;
    peephole_sound_ = 0;
    peephole_eye_ = glm::vec3(0.0f);
    last_target_ = glm::vec3(0.0f);
}

std::string NazoManager::DescribeSession() const {
    std::string text = std::format("active {:#x} armed {} state {} flag {:#x} steps {} hide {} voice {} jack {} words", active_mask_, action_armed_,
                                   armed_state_, armed_flag_, true_end_steps_, frames_since_hide_, voice_active_, jack_heard_);
    for (int i = 0; i < kCount; ++i) {
        text += std::format(" {:#x}/{}", word_[i], cleared_[i]);
    }
    return text;
}

void NazoManager::ResetAllStates() {
    for (int i = 0; i < kCount; ++i) {
        ResetState(static_cast<NazoId>(i));
    }
    action_armed_ = false;
    clear_sound_pending_ = false;
    LogInfo("nazo: all states reset");
}

void NazoManager::OnClear(NazoId id) {
    cleared_[Index(id)] = 1;
    LogInfo("nazo: {} cleared", NazoName(id));
    game_.Objects().GimmickLogicControl(GimmickType::Ocho, "None");
    if (game_.Floor().IsCurrentFloorName("f050")) {
        game_.SendControllerMessage("Clearf050");
    }
    clear_sound_pending_ = true;
    clear_sound_timer_ = 0.0f;
}

void NazoManager::ApplyVisuals(NazoId id) {
    const uint32_t w = word_[Index(id)];
    switch (id) {
    case NazoId::XMark:
        if (w & 1) {
            SetAssetEnable("XMarkInit", true);
            for (const char* name : {"XMark", "XMarkPic1", "XMarkText", "XMarkText1", "XMarkPicEf"}) {
                SetAssetEnable(name, false);
            }
        } else if (w & 4) {
            SetAssetEnable("XMarkInit", false);
            SetAssetEnable("XMark", true);
            for (const char* name : {"XMarkPic1", "XMarkText", "XMarkText1", "XMarkPicEf"}) {
                SetAssetEnable(name, false);
            }
        } else if (w & 8) {
            SetAssetEnable("XMarkInit", false);
            SetAssetEnable("XMark", false);
            for (const char* name : {"XMarkPic1", "XMarkText", "XMarkText1", "XMarkPicEf"}) {
                SetAssetEnable(name, true);
            }
        }
        break;
    case NazoId::Hello: {
        static constexpr const char* kHello[] = {"L1", "E", "H", "L2", "O", "Exclamation"};
        static constexpr const char* kHell[] = {"HELL_L1", "HELL_E", "HELL_H", "HELL_L2"};
        auto set = [&](bool hello, bool hell, bool ican) {
            for (const char* name : kHello) {
                SetAssetEnable(name, hello);
            }
            for (const char* name : kHell) {
                SetAssetEnable(name, hell);
            }
            SetAssetEnable("HELL_Ican", ican);
        };
        if (w & 1) {
            set(false, false, false);
        } else if (w & 2) {
            set(false, false, true);
        } else if (w & 4) {
            set(true, false, true);
        } else if (w & 0x200) {
            set(false, true, true);
        }
        break;
    }
    case NazoId::Peephole:
        SetAssetEnable("HoleText", (w & 0x10) != 0);
        Sound2D(kSoundPeepholeStop);
        Sound2D(kSoundPeepholeEndState);
        break;
    case NazoId::Photo:
        if (w & 1) {
            SetAssetEnable("FrameWall", true);
            SetAssetEnable("FrameGround", false);
            for (const PhotoPiece& p : kPieces) {
                SetAssetEnable(p.name, false);
            }
        } else if (w & 2) {
            SetAssetEnable("FrameWall", false);
            SetAssetEnable("FrameGround", true);
            for (const PhotoPiece& p : kPieces) {
                SetAssetEnable(p.name, false);
            }
        } else if (w & 4) {
            SetAssetEnable("FrameWall", true);
            SetAssetEnable("FrameGround", false);
            for (const PhotoPiece& p : kPieces) {
                const bool collected = (w & p.bit) != 0;
                SetAssetEnable(p.name, !collected);
                SetAssetEnable(p.frame, collected);
            }
            SetAssetEnable("FrameOption", (w & 0x100) != 0);
        } else if (w & 0x1F8) {
            SetAssetEnable("FrameWall", true);
            SetAssetEnable("FrameGround", false);
            for (const PhotoPiece& p : kPieces) {
                SetAssetEnable(p.name, false);
                SetAssetEnable(p.frame, true);
            }
            SetAssetEnable("FrameOption", true);
        }
        break;
    case NazoId::TrueEnd:
        if (w & 1) {
            Sound2D(kSoundPhoneStop);
        }
        break;
    }
}

void NazoManager::Arm(int state, uint32_t flag) {
    if (action_armed_) {
        return;
    }
    action_armed_ = true;
    armed_state_ = state;
    armed_flag_ = flag;
    hold_timer_ = 0.0f;
    LogInfo("nazo: action armed (state {}, flag {:#x})", state, flag);
}

void NazoManager::HelloStep() {
    uint32_t& w = word_[1];
    switch (w) {
    case 4: SetAssetEnable("L1", false); SetAssetEnable("HELL_L1", true); w = 8; break;
    case 8: SetAssetEnable("E", false); SetAssetEnable("HELL_E", true); w = 0x10; break;
    case 0x10: SetAssetEnable("H", false); SetAssetEnable("HELL_H", true); w = 0x20; break;
    case 0x20: SetAssetEnable("L2", false); SetAssetEnable("HELL_L2", true); w = 0x40; break;
    case 0x40: w = 0x80; break;
    case 0x80:
        for (const char* name : {"O", "Exclamation", "HELL_L1", "HELL_E", "HELL_H", "HELL_L2"}) {
            SetAssetEnable(name, false);
        }
        w = 0x100;
        break;
    default: break;
    }
    LogInfo("nazo: Hello step -> {:#x}", w);
}

void NazoManager::SetCondition(std::string_view c, const glm::vec3& target) {
    last_target_ = target;
    uint32_t* w = word_.data();
    if (c == "XMark") {
        if (IsActive(NazoId::XMark)) {
            w[0] = 8;
        }
    } else if (c == "XMarkTerop") {
        if (w[0] & 8) {
            game_.ShowCaption(kCaptionXMark);
        }
    } else if (c == "Hello") {
        {
            const glm::vec3 feet = game_.GetPlayer().Feet();
            const glm::vec3 eye = game_.GetCamera().position;
            LogInfo("nazo: Hello condition (active {}, word {:#x}) feet ({:.2f} {:.2f} {:.2f}) camera ({:.2f} {:.2f} {:.2f}) target ({:.2f} {:.2f} {:.2f})",
                    IsActive(NazoId::Hello), w[1], feet.x, feet.y, feet.z, eye.x, eye.y, eye.z, target.x, target.y, target.z);
        }
        if (IsActive(NazoId::Hello)) {
            HelloStep();
        }
    } else if (c == "Hell") {
        if (IsActive(NazoId::Hello) && (w[1] & 0xC0)) {
            w[1] = 0x200;
        }
    } else if (c == "HelloTerop") {
        if (w[1] & 0x200) {
            game_.ShowCaption(kCaptionHello);
        }
    } else if (c == "Peephole") {
        if (IsActive(NazoId::Peephole) && !(w[2] & 4)) {
            w[2] |= 4;
            peephole_timer_ = 0.0f;
            Arm(2, 0);
        }
    } else if (c == "HoleTerop") {
        if (w[2] & 0x10) {
            game_.ShowCaption(kCaptionHole);
        }
    } else if (c == "PhotoPrepare") {
        if (!(w[3] & 2)) {
            Prepare(NazoId::Photo);
            ApplyVisuals(NazoId::Photo);
        }
    } else if (c == "PhotoOption") {
        if (IsActive(NazoId::Photo) && !(w[3] & 0x100)) {
            w[3] |= 0x100;
            game_.NoteArchive("photo:PhotoOption");
            SetAssetEnable("FrameOption", true);
            game_.Effects().ShowSubliminalImage(1, true, false);
            Sound2D(kSoundPhoto);
        }
    } else if (c == "PhotoWallTerop") {
        if (!(w[3] & 2)) {
            game_.ShowCaption(kCaptionPhoto);
        }
    } else if (c == "PhotoGroundTerop") {
        if (w[3] & 2) {
            game_.ShowCaption(kCaptionPhoto);
        }
    } else if (c == "EndPhone") {
        if (IsActive(NazoId::TrueEnd) && (w[4] & 0x80) && !(w[4] & 0x100)) {
            Arm(4, 0x100);
        }
    } else if (c == "EndJack") {
        if (IsActive(NazoId::TrueEnd) && (w[4] & 1) && !(w[4] & 4)) {
            w[4] |= 4;
            Sound2D(kSoundTrueEndMark);
            LogInfo("nazo: TrueEnd heard Jack (word {:#x})", w[4]);
        }
    } else {
        for (const PhotoPiece& p : kPieces) {
            if (c == p.name) {
                if (IsActive(NazoId::Photo) && !(w[3] & p.bit)) {
                    Arm(3, p.bit);
                }
                return;
            }
        }
        LogDebug("nazo: condition {} ignored", c);
    }
}

void NazoManager::CountFootstep() {
    if (IsActive(NazoId::TrueEnd) && (word_[4] & 1)) {
        ++true_end_steps_;
    }
}

void NazoManager::Commit() {
    if (armed_state_ == 3) {
        for (const PhotoPiece& p : kPieces) {
            if (p.bit == armed_flag_) {
                SetAssetEnable(p.name, false);
                SetAssetEnable(p.frame, true);
                game_.Effects().ShowSubliminalImage(p.image, true, false);
                Sound2D(kSoundPhoto);
                word_[3] |= p.bit;
                game_.NoteArchive("photo:" + std::string(p.name));
                LogInfo("nazo: photo piece {} collected (word {:#x})", p.name, word_[3]);
            }
        }
    } else if (armed_state_ == 4) {
        Sound2D(kSoundPhoneStop);
        SoundAt(kSoundPhoneAnswer, "Phone");
        word_[4] = 0x100;
        LogInfo("nazo: phone answered");
    }
}

void NazoManager::UpdatePeephole(float dt) {
    if (!IsActive(NazoId::Peephole)) {
        return;
    }
    uint32_t& w = word_[2];
    if (w & (4 | 8)) {
        peephole_timer_ += dt;
    }
    if ((w & 8) && peephole_timer_ > 0.3f) {
        const bool playing = peephole_sound_ == 0 || game_.IsSoundPlaying(peephole_sound_);
        if (!(playing && peephole_timer_ <= kPeepholeHoldLimit)) {
            w = 0x10;
        }
    }
    if ((w & 4) && peephole_timer_ > 0.3f && !(w & 8)) {
        peephole_eye_valid_ = false;
        w |= 8;
        game_.Effects().overlay_texture = "holl_002_alp";
        game_.SetMirrorViewportBit(1, true);
        game_.Effects().full_screen_blur = false;
        Sound2D(kSoundPeepholeState);
        peephole_sound_ = game_.PostSound("Play_Peephole_Theater", glm::vec3(0.0f), false);
        glm::vec3 hole(0.0f);
        const bool placed = PeepholePosition(hole);
        const glm::vec3 eye = game_.GetCamera().position;
        peephole_eye_ = eye;
        peephole_eye_valid_ = placed;
        LogInfo("nazo: peephole theater started, view from ({:.2f} {:.2f} {:.2f}){}", eye.x, eye.y, eye.z, placed ? "" : " (no Peephole asset)");
    }
    if (w & 0x10) {
        ApplyVisuals(NazoId::Peephole);
        OnClear(NazoId::Peephole);
        active_mask_ &= ~(1u << 2);
        game_.SendControllerMessage("f110_GoNextFloor");
        hide_effect_next_frame_ = true;
        frames_since_hide_ = 0;
        hide_ticks_ = 0;
        LogInfo("nazo: peephole finished ({} aborts before)", abort_calls_);
    }
}

void NazoManager::AbortPeephole() {
    if (!IsActive(NazoId::Peephole) || (word_[2] & 0x10)) {
        return;
    }
    const bool theater = (word_[2] & 8) != 0;
    peephole_eye_valid_ = false;
    game_.SetMirrorViewportBit(1, false);
    game_.Effects().full_screen_blur = true;
    word_[2] = 0;
    cleared_[2] = 0;
    peephole_timer_ = 0.0f;
    clear_sound_timer_ = 0.0f;
    hold_timer_ = 0.0f;
    true_end_timer_ = 0.0f;
    armed_state_ = 0;
    armed_flag_ = 0;
    true_end_steps_ = 0;
    clear_sound_pending_ = false;
    action_armed_ = false;
    hide_effect_next_frame_ = true;
    frames_since_hide_ = 0;
    hide_ticks_ = 0;
    Sound2D(kSoundPeepholeStop);
    Sound2D(kSoundPeepholeEndState);
    ++abort_calls_;
    if (theater) {
        LogInfo("nazo: peephole aborted (zoom released during the theater)");
    }
}

void NazoManager::UpdateTrueEnd(float dt) {
    if (!IsActive(NazoId::TrueEnd)) {
        return;
    }
    uint32_t& w = word_[4];
    const bool still = game_.GetPlayer().Standing();
    true_end_timer_ += dt;
    if (w & 1) {
        if (still) {
            if (true_end_steps_ == 10 && !(w & 2)) {
                w |= 2;
                Sound2D(kSoundTrueEndMark);
                LogInfo("nazo: TrueEnd ten steps");
            }
            true_end_steps_ = 0;
        }
        if ((w & 2) && (w & 4)) {
            w = 8;
            true_end_timer_ = 0.0f;
            LogInfo("nazo: TrueEnd phase 2");
        }
    } else if (w == 8) {
        if (true_end_timer_ >= 10.0f) {
            Sound2D(kSoundTrueEndWait);
            w = 0x10;
            true_end_timer_ = 0.0f;
        }
    } else if (w == 0x10) {
        if (!still && true_end_timer_ < 15.0f) {
            Sound2D(kSoundTrueEndHold);
            w = 0x20;
            LogInfo("nazo: TrueEnd failed (moved)");
        } else if (true_end_timer_ >= 15.0f && still) {
            Sound2D(kSoundTrueEndHold);
            Sound2D(kSoundTrueEndMark);
            w = 0x40;
            true_end_timer_ = 0.0f;
        }
    } else if (w == 0x40) {
        if (true_end_timer_ > 3.0f) {
            SoundAt(kSoundPhoneRing, "Phone");
            w = 0x80;
            LogInfo("nazo: TrueEnd phone ringing");
        }
    } else if (w == 0x100) {
        OnClear(NazoId::TrueEnd);
        active_mask_ &= ~(1u << 4);
    }
}

void NazoManager::Update(float dt) {
    if (hide_effect_next_frame_) {
        ++hide_ticks_;
    }
    if (hide_effect_next_frame_ && game_.GameFrameTick()) {
        if (++frames_since_hide_ >= 1) {
            LogInfo("nazo: peephole overlay removed next game frame ({} ticks after abort/clear)", hide_ticks_);
            game_.Effects().overlay_texture.clear();
            game_.SetMirrorViewportBit(1, false);
            hide_effect_next_frame_ = false;
        }
    }
    if (IsActive(NazoId::XMark) && word_[0] == 8) {
        ApplyVisuals(NazoId::XMark);
        SoundAt(kSoundXMark, "XMark");
        OnClear(NazoId::XMark);
        active_mask_ &= ~1u;
    }
    if (IsActive(NazoId::Hello) && word_[1] == 0x200) {
        ApplyVisuals(NazoId::Hello);
        Sound2D(kSoundHello);
        OnClear(NazoId::Hello);
        active_mask_ &= ~2u;
    }
    UpdatePeephole(dt);
    if ((word_[3] & 0x1F8) == 0x1F8) {
        game_.NoteArchive("photo:complete");
    }
    if (IsActive(NazoId::Photo) && word_[3] == 0x1F8) {
        OnClear(NazoId::Photo);
        active_mask_ &= ~8u;
    }
    UpdateTrueEnd(dt);
    if (clear_sound_pending_) {
        clear_sound_timer_ += dt;
        if (clear_sound_timer_ >= 3.0f) {
            Sound2D(kSoundClear);
            clear_sound_pending_ = false;
        }
    }
    if (action_armed_) {
        hold_timer_ += dt;
        const bool holding = (game_.GetPlayer().HeldButtons() & kPadZoom) != 0;
        const float limit = armed_state_ == 2 ? kPeepholeHoldLimit : kHoldLimit;
        if (!holding) {
            action_armed_ = false;
        } else if (hold_timer_ > limit) {
            Commit();
            action_armed_ = false;
        }
    }
    if (jack_heard_) {
        jack_heard_ = false;
        SetCondition("EndJack");
    }
}

void NazoManager::StartVoiceRecognition() {
    if (!voice_active_) {
        LogInfo("nazo: voice recognition on");
    }
    voice_active_ = true;
    game_.SetVoiceListening(true);
}

void NazoManager::StopVoiceRecognition() {
    if (voice_active_) {
        LogInfo("nazo: voice recognition off");
    }
    voice_active_ = false;
    game_.SetVoiceListening(false);
}

void NazoManager::OnVoiceKeyword(std::string_view keyword) {
    LogInfo("nazo: heard '{}' (TrueEnd word {:#x})", keyword, word_[4]);
    if (keyword == "jack" || keyword == "Jack") {
        jack_heard_ = true;
    }
}

}
