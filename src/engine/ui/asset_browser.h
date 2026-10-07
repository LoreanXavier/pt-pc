#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/fs/vfs.h"

namespace pt {

class AssetBrowser {
public:
    explicit AssetBrowser(Vfs& vfs) : vfs_(vfs) {}
    void Draw();

private:
    Vfs& vfs_;
    char filter_[128] = "";
    char entry_filter_[128] = "";
    std::string selected_archive_;
    std::shared_ptr<FoxPackage> package_;
    std::string selected_entry_;
    std::vector<uint8_t> preview_;
};

}
