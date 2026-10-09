#include "engine/audio/subtitles.h"

#include <cctype>
#include <algorithm>
#include <cstring>
#include <format>

#include "engine/audio/sound_package.h"
#include "engine/core/log.h"
#include "engine/core/strcode.h"
#include "engine/core/localized_text.h"
#include "engine/fs/vfs.h"

namespace pt::audio {
namespace {

std::string NormalizeLanguage(std::string_view language) {
    std::string lang(language);
    if (lang.size() > 4 && lang.ends_with("Text")) {
        lang.resize(lang.size() - 4);
    }
    for (size_t i = 0; i < lang.size(); ++i) {
        lang[i] = static_cast<char>(i == 0 ? std::toupper(static_cast<unsigned char>(lang[i])) : std::tolower(static_cast<unsigned char>(lang[i])));
    }
    return lang;
}

uint16_t U16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

}

std::string SubtitleTable::SubpPath(std::string_view language) {
    return std::format("/Assets/sh/ui/Subtitles/subp/EngVoice/{}Text/trial.subp", NormalizeLanguage(language));
}

std::string SubtitleTable::PackagePath(std::string_view language) {
    return std::format("/Assets/sh/level/ui/subtitles/EngVoice/{}Text/subtitle.fpk", NormalizeLanguage(language));
}

uint32_t SubtitleTable::SubtitleKey(std::string_view subtitle_id) {
    std::string lower(subtitle_id);
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return static_cast<uint32_t>(StrCode64(lower) & 0xFFFFFFFFull);
}

uint64_t SubtitleTable::MarkerKey(std::string_view label) {
    return StrCode64(label);
}

bool SubtitleTable::Load(Vfs& vfs, std::string_view language, std::span<const std::vector<uint8_t>> sab_tables, std::string* error) {
    const std::string lang = NormalizeLanguage(language);
    std::string source = (lang == "Tur" || lang == "Zhs" || lang == "Ara" || lang == "Rus" || lang == "Ukr" || lang == "Ces") ? "Eng" : lang;
    if (source != "Eng" && !vfs.Archive().Contains(Vfs::ToArchivePath(PackagePath(source)))) {
        LogWarn("subtitles: {} missing, falling back to English", PackagePath(source));
        source = "Eng";
    }
    if (!vfs.LoadPackage(PackagePath(source))) {
        if (error) {
            *error = std::format("subtitle package {} not found", PackagePath(lang));
        }
        return false;
    }
    auto subp = vfs.ReadFile(SubpPath(source));
    if (!subp) {
        if (error) {
            *error = std::format("{} not found", SubpPath(lang));
        }
        return false;
    }
    if (!Parse(*subp, sab_tables, error)) {
        return false;
    }
    const int translation_language = lang == "Tur" ? 7 : lang == "Zhs" ? 8 : lang == "Ara" ? 9 : lang == "Rus" ? 10 : lang == "Ukr" ? 11 : lang == "Ces" ? 12 : 0;
    if (translation_language) {
        for (SubtitleEntry& entry : entries_) {
            bool found = false;
            auto translate = [&](const auto& table) {
                for (const auto& t : table) if (t.key == entry.key && t.lines.size() == entry.lines.size()) {
                    size_t i=0; for (auto text : t.lines) { auto& line=entry.lines[i++].text; line=text; std::replace(line.begin(),line.end(),'|','\n'); } found = true; break;
                }
            };
            switch(translation_language) {case 7:translate(turkish::kSubtitles);break;case 8:translate(chinese::kSubtitles);break;case 9:translate(arabic::kSubtitles);break;case 10:translate(russian::kSubtitles);break;case 11:translate(ukrainian::kSubtitles);break;case 12:translate(czech::kSubtitles);break;}
            if (!found) { if (error) *error = std::format("{} subtitle {:08x} missing or line count differs", lang, entry.key); return false; }
        }
    }
    language_ = lang;
    return true;
}

bool SubtitleTable::Parse(std::span<const uint8_t> data, std::span<const std::vector<uint8_t>> sab_tables, std::string* error) {
    entries_.clear();
    by_key_.clear();
    marker_links_.clear();
    if (data.size() < 4) {
        if (error) {
            *error = "subp too small";
        }
        return false;
    }
    const uint16_t count = U16(data.data() + 2);
    if (4 + static_cast<size_t>(count) * 8 > data.size()) {
        if (error) {
            *error = "subp index runs past the file";
        }
        return false;
    }
    for (uint16_t i = 0; i < count; ++i) {
        const uint32_t key = U32(data.data() + 4 + i * 8);
        const uint32_t offset = U32(data.data() + 8 + i * 8);
        if (offset + 12 > data.size()) {
            continue;
        }
        const uint8_t* header = data.data() + offset;
        const uint8_t line_count = header[2];
        const uint16_t text_size = U16(header + 4);
        const size_t text_start = offset + 12 + static_cast<size_t>(line_count) * 4;
        if (text_start + text_size > data.size()) {
            continue;
        }
        std::string text(reinterpret_cast<const char*>(data.data() + text_start), text_size);
        while (!text.empty() && text.back() == '\0') {
            text.pop_back();
        }
        std::vector<std::string> pieces;
        size_t start = 0;
        while (true) {
            const size_t end = text.find('$', start);
            pieces.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        SubtitleEntry entry;
        entry.key = key;
        entry.category = header[3];
        entry.character = U16(header + 8);
        entry.range = header[10];
        for (uint8_t l = 0; l < line_count; ++l) {
            SubtitleLine line;
            line.start_seconds = U16(header + 12 + l * 4) / 100.0f;
            line.end_seconds = U16(header + 14 + l * 4) / 100.0f;
            if (l < pieces.size()) {
                std::string& piece = pieces[l];
                std::string cleaned;
                for (size_t c = 0; c < piece.size(); ++c) {
                    if (piece[c] == '\r') {
                        continue;
                    }
                    cleaned.push_back(piece[c]);
                }
                while (!cleaned.empty() && (cleaned.front() == '\n' || cleaned.front() == ' ')) {
                    cleaned.erase(cleaned.begin());
                }
                while (!cleaned.empty() && cleaned.back() == '\n') {
                    cleaned.pop_back();
                }
                line.text = std::move(cleaned);
            }
            entry.lines.push_back(std::move(line));
        }
        by_key_[key] = entries_.size();
        entries_.push_back(std::move(entry));
    }
    for (const auto& table : sab_tables) {
        std::vector<SalRecord> records;
        if (!ReadSal(table, records, error)) {
            return false;
        }
        for (const auto& record : records) {
            marker_links_[record.key] = record.subtitle_id;
            auto it = by_key_.find(SubtitleKey(record.subtitle_id));
            if (it != by_key_.end() && entries_[it->second].id.empty()) {
                entries_[it->second].id = record.subtitle_id;
            }
        }
    }
    return true;
}

const SubtitleEntry* SubtitleTable::FindByKey(uint32_t key) const {
    auto it = by_key_.find(key);
    return it == by_key_.end() ? nullptr : &entries_[it->second];
}

const SubtitleEntry* SubtitleTable::FindById(std::string_view subtitle_id) const {
    return FindByKey(SubtitleKey(subtitle_id));
}

std::string SubtitleTable::SubtitleIdForMarker(std::string_view label) const {
    auto it = marker_links_.find(MarkerKey(label));
    return it == marker_links_.end() ? std::string() : it->second;
}

const SubtitleEntry* SubtitleTable::FindByMarker(std::string_view label) const {
    const std::string id = SubtitleIdForMarker(label);
    return id.empty() ? nullptr : FindById(id);
}

}
