#include "crosspad-dawcontrol/DawProtocol.hpp"
#include "crosspad-dawcontrol/DawControlPadLogic.hpp"

#include "crosspad/midi/IMidiOutput.hpp"
#include "crosspad/protocol/SysExProtocol.h"

#include <cstring>

namespace dawcontrol {

void ScreenState::setRow(uint8_t row, const uint8_t* text, size_t len)
{
    if (row >= kTextRows) return;
    if (len > kTextCols) len = kTextCols;
    std::lock_guard<std::mutex> lock(textMutex_);
    size_t n = 0;
    for (size_t i = 0; i < len; ++i) {
        // 7-bit payload; anything outside printable ASCII becomes a space
        // rather than a control character on the screen.
        const uint8_t c = text[i];
        rows_[row][n++] = (c >= 0x20 && c <= 0x7E) ? (char)c : ' ';
    }
    rows_[row][n] = '\0';
}

void ScreenState::copyRow(uint8_t row, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    if (row >= kTextRows) { out[0] = '\0'; return; }
    std::lock_guard<std::mutex> lock(textMutex_);
    std::strncpy(out, rows_[row], outSize - 1);
    out[outSize - 1] = '\0';
}

void ScreenState::clearRows()
{
    std::lock_guard<std::mutex> lock(textMutex_);
    for (auto& r : rows_) r[0] = '\0';
}

void sendHello(crosspad::IMidiOutput& out, ScreenState& screen, int64_t nowUs)
{
    screen.lastHelloUs.store(nowUs);
    const uint8_t frame[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER, CROSSPAD_CMD_DAW, kSubHello,
        kProtoVersion, kPadCount, kTextRows,
        (uint8_t)(kCapRgb | kCapText | kCapEncoder | kCapAftertouch | kCapControl),
        CROSSPAD_SYSEX_END,
    };
    out.sendSysEx(frame, sizeof(frame));
}

void sendMode(crosspad::IMidiOutput& out, uint8_t mode)
{
    const uint8_t frame[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER, CROSSPAD_CMD_DAW, kSubMode,
        (uint8_t)(mode & 0x7F), CROSSPAD_SYSEX_END,
    };
    out.sendSysEx(frame, sizeof(frame));
}

void sendTilt(crosspad::IMidiOutput& out, uint8_t padIdx, uint8_t value)
{
    const uint8_t frame[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER, CROSSPAD_CMD_DAW, kSubTilt,
        (uint8_t)(padIdx & 0x7F), (uint8_t)(value & 0x7F), CROSSPAD_SYSEX_END,
    };
    out.sendSysEx(frame, sizeof(frame));
}

void sendPad(crosspad::IMidiOutput& out, uint8_t padIdx, bool pressed)
{
    const uint8_t frame[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER, CROSSPAD_CMD_DAW, kSubPad,
        (uint8_t)(padIdx & 0x7F), (uint8_t)(pressed ? 1 : 0), CROSSPAD_SYSEX_END,
    };
    out.sendSysEx(frame, sizeof(frame));
}

// Host colours travel as 7-bit r g b; the LSB is lost on the way and the LED
// cannot show it anyway.
static uint8_t to8(uint8_t v7) { return (uint8_t)((v7 & 0x7F) << 1); }

void handleFrame(const uint8_t* args, unsigned len, DawControlPadLogic& pads,
                 ScreenState& screen, crosspad::IMidiOutput& out, int64_t nowUs)
{
    if (!args || len < 1) return;
    const uint8_t sub = args[0];
    const uint8_t* a = args + 1;
    const unsigned n = len - 1;

    switch (sub) {
    case kSubHello:
        screen.dawId.store(n >= 1 ? a[0] : (uint8_t)kDawOther);
        screen.connected.store(true);
        // The host answers every device hello with its own; a host hello
        // right after ours is that answer and gets no reply (see sendHello).
        if (nowUs - screen.lastHelloUs.load() > (int64_t)kHelloEchoGuardMs * 1000) {
            sendHello(out, screen, nowUs);
        }
        // The host's re-sync paints the keyboard layout; the board's mode is
        // the board's, so it follows every host hello and the host repaints.
        sendMode(out, screen.mode.load());
        break;

    case kSubLed:
        if (n >= 4 && a[0] < kPadCount) {
            pads.setHostColor(a[0], to8(a[1]), to8(a[2]), to8(a[3]));
        }
        break;

    case kSubLedAll:
        if (n >= 3u * kPadCount) {
            for (uint8_t i = 0; i < kPadCount; ++i) {
                pads.setHostColor(i, to8(a[3 * i]), to8(a[3 * i + 1]), to8(a[3 * i + 2]));
            }
        }
        break;

    case kSubText:
        if (n >= 1) screen.setRow(a[0], a + 1, n - 1);
        break;

    case kSubBeat:
        if (n >= 1) {
            screen.beat.store(a[0] > 2 ? 2 : a[0]);
            if (a[0] != 0) screen.beatCount.fetch_add(1);
        }
        break;

    case kSubTransport:
        if (n >= 1) screen.transport.store(a[0]);
        break;

    case kSubMode:
        // Host-shown modes only: control is entered from the board, never
        // asked for by the host.
        if (n >= 1 && a[0] <= kModeClips) {
            screen.mode.store(a[0]);
            pads.setControlMode(false);
        }
        break;

    case kSubConfig:
        if (n >= 2) pads.setLayout(a[0], a[1]);
        break;

    case kSubPing:
        return;   // liveness only; the app stamped the clock before dispatch

    case kSubBye:
        hostGone(pads, screen);
        break;

    default:
        return;   // unknown sub: no screen change, nothing to redraw
    }
    screen.dirty.store(true);
}

void hostGone(DawControlPadLogic& pads, ScreenState& screen)
{
    screen.connected.store(false);
    screen.dawId.store(kDawOther);
    screen.beat.store(0);
    screen.transport.store(0);
    screen.clearRows();
    pads.clearHostColors();
    screen.dirty.store(true);
}

} // namespace dawcontrol
