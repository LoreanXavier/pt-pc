#include "game/ui/port_credits.h"

#include <algorithm>

namespace pt::game::port_credits {
namespace {

constexpr float kFps = 60.0f;
// a short pause on the bare fade between two cards
constexpr float kGap = 0.5f;
constexpr float kEndScale = 1.2f;

struct Setin {
    float in, hold, end;
};
constexpr Setin kShort{29.0f, 149.0f, 239.0f};
constexpr Setin kLong{59.0f, 209.0f, 329.0f};

std::vector<Card> MakeCards() {
    std::vector<Card> cards;
    cards.push_back({{"P.T. PC Port", "pc_credits_by"}, true, 1.4f});
#if defined(PT_WITH_DLSS) || defined(PT_WITH_STREAMLINE)
    // the NVIDIA RTX SDKs licence, exhibit 7.1 (b) and (c): the use of the SDK attributed and the NVIDIA Marks in the credits
    cards.push_back({{"NVIDIA RTX", "pc_credits_dlss",
#if defined(PT_WITH_STREAMLINE)
                      "NVIDIA Streamline, NVIDIA DLSS Frame Generation and NVIDIA Reflex",
#endif
                      "", "NVIDIA, NVIDIA RTX, GeForce RTX, DLSS and Reflex are trademarks", "and/or registered trademarks of NVIDIA Corporation."},
                     false, 1.0f});
#endif
#if defined(PT_WITH_FSR) || defined(PT_WITH_XESS)
    // the MIT notice of the FidelityFX SDK and the copyright notice the Intel Simplified Software License asks to reproduce
    Card upscalers{{}, false, 1.0f};
#if defined(PT_WITH_FSR)
    upscalers.lines.insert(upscalers.lines.end(), {"AMD FidelityFX Super Resolution 3 (FidelityFX SDK 1.1.4)",
                                                   "Copyright (C) 2024 Advanced Micro Devices, Inc. MIT License.", ""});
#endif
#if defined(PT_WITH_XESS)
    upscalers.lines.insert(upscalers.lines.end(), {"Intel(R) Xe Super Sampling (XeSS) SDK 3.0.2",
                                                   "Copyright (C) 2025 Intel Corporation. Intel Simplified Software License.", ""});
#endif
    upscalers.lines.push_back("AMD and FidelityFX are trademarks of Advanced Micro Devices, Inc.");
    upscalers.lines.push_back("Intel and XeSS are trademarks of Intel Corporation.");
    cards.push_back(std::move(upscalers));
#endif
    cards.push_back({{"whisper.cpp and ggml, Copyright (c) 2023-2026 The ggml authors. MIT License.",
                      "Whisper, Copyright (c) 2022 OpenAI. MIT License.",
                      "Silero VAD, Copyright (c) 2020-present Silero Team. MIT License.",
                      "Noto Sans, Noto Sans SC, Noto Kufi Arabic and Noto Naskh Arabic: SIL Open Font License 1.1.",
                      "Installer: LibOrbisPkg, GNU LGPL.", "", "pc_credits_licenses"},
                     false, 1.0f});
    cards.push_back({{"pc_credits_konami", "", "pc_credits_fan"}, false, 1.0f});
    return cards;
}

const Setin& SetinOf(const Card& card) { return card.short_setin ? kShort : kLong; }

}

std::span<const Card> Cards() {
    static const std::vector<Card> cards = MakeCards();
    return cards;
}

float Duration() {
    float total = 0.0f;
    for (const Card& card : Cards()) total += SetinOf(card).end / kFps + kGap;
    return total;
}

View At(float seconds) {
    const std::span<const Card> cards = Cards();
    float start = 0.0f;
    for (size_t i = 0; i < cards.size(); ++i) {
        const Setin& s = SetinOf(cards[i]);
        const float frame = (seconds - start) * kFps;
        if (frame >= 0.0f && frame < s.end) {
            View view;
            view.card = static_cast<int>(i);
            view.alpha = frame < s.in ? frame / s.in : frame < s.hold ? 1.0f : 1.0f - (frame - s.hold) / (s.end - s.hold);
            view.alpha = std::clamp(view.alpha, 0.0f, 1.0f);
            view.scale = 1.0f + (kEndScale - 1.0f) * frame / s.end;
            return view;
        }
        start += s.end / kFps + kGap;
    }
    return {};
}

}
