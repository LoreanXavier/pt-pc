#include "engine/ui/asset_browser.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace pt {
namespace {

bool ContainsNoCase(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
    return it != text.end();
}

}

void AssetBrowser::Draw() {
    if (!ImGui::Begin("Assets")) {
        ImGui::End();
        return;
    }
    ImGui::InputText("filter", filter_, sizeof(filter_));
    if (ImGui::BeginChild("archive", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.45f), ImGuiChildFlags_Borders)) {
        const auto& names = vfs_.Archive().Names();
        for (size_t i = 1; i < names.size(); ++i) {
            if (!ContainsNoCase(names[i], filter_)) {
                continue;
            }
            const bool selected = names[i] == selected_archive_;
            if (ImGui::Selectable(names[i].c_str(), selected)) {
                selected_archive_ = names[i];
                package_.reset();
                if (names[i].ends_with(".fpk") || names[i].ends_with(".fpkd")) {
                    package_ = vfs_.LoadPackage(names[i]);
                }
            }
        }
    }
    ImGui::EndChild();
    if (package_) {
        ImGui::Text("%s  %s  %zu files", package_->Name().c_str(), package_->Platform().c_str(), package_->Entries().size());
        ImGui::InputText("entry filter", entry_filter_, sizeof(entry_filter_));
        if (ImGui::BeginChild("entries", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.6f), ImGuiChildFlags_Borders)) {
            for (const auto& entry : package_->Entries()) {
                if (!ContainsNoCase(entry.path, entry_filter_)) {
                    continue;
                }
                if (ImGui::Selectable(entry.path.c_str(), entry.path == selected_entry_)) {
                    selected_entry_ = entry.path;
                    preview_ = package_->Read(entry);
                }
                ImGui::SameLine(ImGui::GetWindowWidth() - 110);
                ImGui::TextDisabled("%10u", entry.size);
            }
        }
        ImGui::EndChild();
    }
    if (!preview_.empty()) {
        ImGui::Text("%s (%zu bytes)", selected_entry_.c_str(), preview_.size());
        std::string hex;
        const size_t count = std::min<size_t>(preview_.size(), 256);
        for (size_t i = 0; i < count; ++i) {
            static const char digits[] = "0123456789ABCDEF";
            hex.push_back(digits[preview_[i] >> 4]);
            hex.push_back(digits[preview_[i] & 15]);
            hex.push_back((i % 16 == 15) ? '\n' : ' ');
        }
        ImGui::TextUnformatted(hex.c_str());
    }
    ImGui::End();
}

}
