#pragma once

#include <string>
#include <string_view>

namespace pt::game::support {

inline constexpr const char* kPatreonUrl = "https://www.patreon.com/loreanxavier";

using UrlOpener = bool (*)(const char* url);

inline bool OpenSupportPage(UrlOpener opener) {
    return opener && opener(kPatreonUrl);
}

}
