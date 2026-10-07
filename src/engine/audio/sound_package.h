#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/audio/wem.h"
#include "engine/audio/wwise_bank.h"

namespace pt {
class Vfs;
}

namespace pt::audio {

struct SbpEntry {
    std::string kind;
    uint32_t offset = 0;
    uint32_t size = 0;
};

bool ReadSbp(std::span<const uint8_t> data, std::vector<SbpEntry>& entries, std::string* error);

struct SalRecord {
    uint64_t key = 0;
    std::string kind;
    std::vector<uint8_t> params;
    std::string subtitle_id;
};

bool ReadSal(std::span<const uint8_t> data, std::vector<SalRecord>& records, std::string* error);

struct DemoStreamAudio {
    std::vector<uint8_t> wem;
    double start_time = 0.0;
    double end_time = 0.0;
};

std::optional<DemoStreamAudio> ExtractDemoStreamAudio(std::span<const uint8_t> fsm);

class SoundBankSet {
public:
    static constexpr const char* kInitBankPath = "/Assets/sh/sound/asset/Init.bnk";
    static const std::vector<std::string>& DefaultPackages();

    bool Load(Vfs& vfs, const std::vector<std::string>& packages, std::string* error);
    bool LoadBankBytes(std::string name, std::shared_ptr<const std::vector<uint8_t>> storage, size_t offset, size_t size, std::string* error);

    const std::vector<std::unique_ptr<Bank>>& Banks() const { return banks_; }
    const Bank* FindBank(std::string_view name) const;
    const std::vector<std::vector<uint8_t>>& SabTables() const { return sab_tables_; }
    const HircObject* Find(uint32_t id) const;
    std::shared_ptr<const Media> FindMedia(uint32_t id) const;
    const std::unordered_map<uint32_t, std::shared_ptr<const Media>>& AllMedia() const { return media_; }
    const std::unordered_map<uint32_t, std::string>& MediaBank() const { return media_bank_; }
    const std::vector<std::string>& MediaErrors() const { return media_errors_; }

private:
    std::vector<std::unique_ptr<Bank>> banks_;
    std::vector<std::vector<uint8_t>> sab_tables_;
    std::unordered_map<uint32_t, const HircObject*> objects_;
    std::unordered_map<uint32_t, std::shared_ptr<const Media>> media_;
    std::unordered_map<uint32_t, std::string> media_bank_;
    std::vector<std::string> media_errors_;
};

}
