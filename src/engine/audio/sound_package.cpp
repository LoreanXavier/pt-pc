#include "engine/audio/sound_package.h"

#include <cstring>
#include <format>

#include "engine/core/log.h"
#include "engine/fs/mods.h"
#include "engine/fs/vfs.h"

namespace pt::audio {
namespace {

std::shared_ptr<Media> ModMedia(uint32_t id) {
    if (!mods::Active()) {
        return nullptr;
    }
    for (const char* extension : {".wem", ".wav"}) {
        const std::string path = std::format("/Assets/sh/sound/wem/{}{}", id, extension);
        auto bytes = mods::ReadOverride(path);
        if (!bytes) {
            continue;
        }
        auto storage = std::make_shared<const std::vector<uint8_t>>(std::move(*bytes));
        std::string error;
        if (auto media = Media::Create(id, storage, 0, storage->size(), &error)) {
            LogInfo("mods: sound media {} from {}", id, path);
            return media;
        }
        LogWarn("mods: {} ignored: {}", path, error);
    }
    return nullptr;
}

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

uint64_t U64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

double F64(const uint8_t* p) {
    double v;
    std::memcpy(&v, p, 8);
    return v;
}

}

bool ReadSbp(std::span<const uint8_t> data, std::vector<SbpEntry>& entries, std::string* error) {
    entries.clear();
    if (data.size() < 8 || std::memcmp(data.data(), "SBPL", 4) != 0) {
        if (error) {
            *error = "not a Fox sound bank package";
        }
        return false;
    }
    const uint8_t count = data[4];
    uint16_t header_size;
    std::memcpy(&header_size, data.data() + 5, 2);
    if (header_size != 8 + 12 * count || data.size() < header_size) {
        if (error) {
            *error = std::format("unexpected header size {} for {} entries", header_size, count);
        }
        return false;
    }
    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t* e = data.data() + 8 + i * 12;
        SbpEntry entry;
        entry.kind.assign(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 4));
        entry.offset = U32(e + 4);
        entry.size = U32(e + 8);
        if (static_cast<size_t>(entry.offset) + entry.size > data.size()) {
            if (error) {
                *error = std::format("entry {} runs past the end of the package", i);
            }
            return false;
        }
        entries.push_back(std::move(entry));
    }
    return true;
}

bool ReadSal(std::span<const uint8_t> data, std::vector<SalRecord>& records, std::string* error) {
    records.clear();
    if (data.size() < 8 || std::memcmp(data.data(), "SAL3", 4) != 0) {
        if (error) {
            *error = "not a SAL3 table";
        }
        return false;
    }
    const uint32_t count = U32(data.data() + 4);
    if (8 + static_cast<size_t>(count) * 16 > data.size()) {
        if (error) {
            *error = "SAL3 index runs past the table";
        }
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* index = data.data() + 8 + i * 16;
        SalRecord record;
        record.key = U64(index);
        const uint64_t offset = U64(index + 8);
        if (offset + 12 > data.size()) {
            continue;
        }
        const uint8_t* r = data.data() + offset;
        record.kind.assign(reinterpret_cast<const char*>(r + 4), strnlen(reinterpret_cast<const char*>(r + 4), 4));
        const uint32_t param_size = U32(r + 8);
        const size_t text_start = offset + 12 + param_size + 6;
        if (text_start >= data.size()) {
            continue;
        }
        record.params.assign(r + 12, r + 12 + param_size);
        const char* text = reinterpret_cast<const char*>(data.data() + text_start);
        record.subtitle_id.assign(text, strnlen(text, data.size() - text_start));
        records.push_back(std::move(record));
    }
    return true;
}

std::optional<DemoStreamAudio> ExtractDemoStreamAudio(std::span<const uint8_t> fsm) {
    DemoStreamAudio audio;
    uint32_t total_size = 0;
    bool first = true;
    size_t pos = 0;
    while (pos + 8 <= fsm.size()) {
        const uint8_t* chunk = fsm.data() + pos;
        const uint32_t size = U32(chunk + 4);
        if (size == 0) {
            break;
        }
        if (size < 8 || pos + size > fsm.size()) {
            return std::nullopt;
        }
        if (std::memcmp(chunk, "SND ", 4) == 0 && size >= 0x10) {
            if (first) {
                if (size < 0x20) {
                    return std::nullopt;
                }
                audio.start_time = F64(chunk + 8);
                total_size = U32(chunk + 0x10);
                audio.wem.insert(audio.wem.end(), chunk + 0x20, chunk + size);
                first = false;
            } else {
                audio.wem.insert(audio.wem.end(), chunk + 0x10, chunk + size);
            }
        } else if (std::memcmp(chunk, "END ", 4) == 0 && size >= 0x10) {
            audio.end_time = F64(chunk + 8);
        }
        pos += size;
    }
    if (first || audio.wem.size() < total_size) {
        return std::nullopt;
    }
    audio.wem.resize(total_size);
    return audio;
}

const std::vector<std::string>& SoundBankSet::DefaultPackages() {
    static const std::vector<std::string> packages = {
        "/Assets/sh/sound/asset/sys_resident.sbp",
        "/Assets/sh/sound/asset/bg_common.sbp",
        "/Assets/sh/sound/asset/sfx_common.sbp",
        "/Assets/sh/sound/asset/bgm_common.sbp",
    };
    return packages;
}

bool SoundBankSet::LoadBankBytes(std::string name, std::shared_ptr<const std::vector<uint8_t>> storage, size_t offset, size_t size,
                                 std::string* error) {
    auto bank = std::make_unique<Bank>();
    if (!bank->Load(std::move(name), storage, offset, size, error)) {
        return false;
    }
    for (const auto& object : bank->Objects()) {
        objects_[object->id] = object.get();
    }
    for (const auto& entry : bank->Media()) {
        std::string media_error;
        auto media = ModMedia(entry.id);
        if (!media) {
            media = Media::Create(entry.id, storage, offset + entry.offset, entry.size, &media_error);
        }
        if (!media) {
            media_errors_.push_back(std::format("{}/{}: {}", bank->Name(), entry.id, media_error));
            continue;
        }
        media_[entry.id] = std::move(media);
        media_bank_[entry.id] = bank->Name();
    }
    banks_.push_back(std::move(bank));
    return true;
}

bool SoundBankSet::Load(Vfs& vfs, const std::vector<std::string>& packages, std::string* error) {
    auto init = vfs.ReadFile(kInitBankPath);
    if (!init) {
        if (error) {
            *error = std::format("{} not found", kInitBankPath);
        }
        return false;
    }
    auto init_storage = std::make_shared<const std::vector<uint8_t>>(std::move(*init));
    if (!LoadBankBytes("Init", init_storage, 0, init_storage->size(), error)) {
        return false;
    }
    for (const auto& path : packages) {
        auto data = vfs.ReadFile(path);
        if (!data) {
            if (error) {
                *error = std::format("{} not found", path);
            }
            return false;
        }
        auto storage = std::make_shared<const std::vector<uint8_t>>(std::move(*data));
        std::vector<SbpEntry> entries;
        if (!ReadSbp(*storage, entries, error)) {
            return false;
        }
        std::string name = path.substr(path.find_last_of('/') + 1);
        name = name.substr(0, name.find('.'));
        for (const auto& entry : entries) {
            if (entry.kind == "bnk") {
                if (!LoadBankBytes(name, storage, entry.offset, entry.size, error)) {
                    return false;
                }
            } else if (entry.kind == "sab") {
                sab_tables_.emplace_back(storage->begin() + entry.offset, storage->begin() + entry.offset + entry.size);
            }
        }
    }
    for (const auto& message : media_errors_) {
        LogWarn("audio: media {}", message);
    }
    return true;
}

const Bank* SoundBankSet::FindBank(std::string_view name) const {
    for (const auto& bank : banks_) {
        if (bank->Name() == name) {
            return bank.get();
        }
    }
    return nullptr;
}

const HircObject* SoundBankSet::Find(uint32_t id) const {
    auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : it->second;
}

std::shared_ptr<const Media> SoundBankSet::FindMedia(uint32_t id) const {
    auto it = media_.find(id);
    return it == media_.end() ? nullptr : it->second;
}

}
