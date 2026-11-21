#pragma once

#include <cstdlib>
#include <istream>
#include <string>

namespace pt {

struct FgStartLoss {
    bool found = false;
    std::string owner;
    double seconds = 0;
};

inline FgStartLoss FindFrameGenStartLoss(std::istream& in, double window = 20.0) {
    FgStartLoss result;
    std::string owner;
    double since = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 12 || line[0] != '[') continue;
        const double t = std::atof(line.c_str() + 1);
        const size_t made = line.find(" on DirectX 12, the swapchain is made again");
        const size_t head = line.find("frame generation: ");
        if (made != std::string::npos && head != std::string::npos && head < made) {
            owner = line.substr(head + 18, made - head - 18);
            if (owner == "off") owner.clear();
            since = t;
        } else if (!owner.empty() && line.find("device lost at") != std::string::npos) {
            if (t - since <= window) {
                result.found = true;
                result.owner = owner;
                result.seconds = t - since;
            }
            return result;
        }
    }
    return result;
}

}
