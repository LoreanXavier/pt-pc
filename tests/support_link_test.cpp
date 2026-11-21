#include <cstdio>
#include <string>

#include "game/support_link.h"

namespace {
std::string g_opened;
int g_calls = 0;
bool Record(const char* url) {
    g_opened = url ? url : "";
    ++g_calls;
    return true;
}
bool Fail(const char*) { return false; }
}

int main() {
    int failures = 0;
    const bool opened = pt::game::support::OpenSupportPage(Record);
    if (!opened || g_calls != 1 || g_opened != "https://www.patreon.com/loreanxavier") {
        std::printf("FAIL: opener got '%s' (%d calls)\n", g_opened.c_str(), g_calls);
        ++failures;
    }
    if (pt::game::support::OpenSupportPage(Fail)) {
        std::printf("FAIL: a failed opener counted as opened\n");
        ++failures;
    }
    if (pt::game::support::OpenSupportPage(nullptr)) {
        std::printf("FAIL: no opener counted as opened\n");
        ++failures;
    }
    std::printf("support link: opened '%s', %d failures\n", g_opened.c_str(), failures);
    return failures ? 1 : 0;
}
