#pragma once
/*
 * A copy of what DAW Control is showing, for a bench that cannot see the LCD.
 *
 * Read-only and platform-free: the HIL verb `DAW_STATE` prints it, and a test
 * compares the rows/transport/beat against what the host script just sent.
 */
#include <cstddef>
#include <cstdint>

#include "crosspad-dawcontrol/DawProtocol.hpp"

namespace dawcontrol {

struct StateSnapshot {
    bool running = false;      // the app has a screen right now
    bool connected = false;
    uint8_t dawId = kDawOther;
    uint8_t beat = 0;
    uint32_t beatCount = 0;
    uint8_t transport = 0;
    uint8_t mode = kModePads;
    char rows[kTextRows][kTextCols + 1] = {};
};

/// Fill `out` from the live app; `running` stays false when it is not open.
void snapshot(StateSnapshot& out);

} // namespace dawcontrol
