#pragma once

#include "engine/platform/input.h"

namespace pt::game {

inline bool EndingOutroInputBlocked(int controller_step) {
    return controller_step == 28;
}

inline InputState GateEndingOutroInput(int controller_step, const InputState& input) {
    return EndingOutroInputBlocked(controller_step) ? InputState{} : input;
}

}
