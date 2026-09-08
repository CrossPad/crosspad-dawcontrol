#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace crosspad { class IMidiOutput; }
class DawControlPadLogic;

/**
 * The DAW controller wire protocol, device side.
 *
 * Frames are `F0 7D 1E <sub> <args…> F7`; the platform strips the framing and
 * hands `<sub> <args…>` to handleFrame(). The vocabulary is device-shaped on
 * purpose — "LED pad N", "text row N", "beat", never "pattern" or "channel
 * rack" — so one firmware serves FL Studio, Ableton and whatever comes next.
 * The catalog of record is crosspad-hil `knowledge/sysex.yaml`, section `daw`;
 * the numbers below and in each host script must match it.
 *
 * Everything here runs on the MIDI receive task: it writes atomics and small
 * mutex-guarded buffers, never LVGL. The app's timer drains ScreenState.
 */
namespace dawcontrol {

constexpr uint8_t kProtoVersion = 1;
constexpr uint8_t kPadCount = 16;
constexpr uint8_t kTextRows = 3;
constexpr size_t kTextCols = 24;

enum Sub : uint8_t {
    kSubHello = 0x01,
    kSubLed = 0x02,
    kSubLedAll = 0x03,
    kSubText = 0x04,
    kSubBeat = 0x05,
    kSubTransport = 0x06,
    kSubMode = 0x07,
    kSubConfig = 0x08,
    kSubBye = 0x7F,
};

enum DawId : uint8_t { kDawOther = 0, kDawFlStudio = 1, kDawAbleton = 2 };
enum PadMode : uint8_t { kModePads = 0, kModeSteps = 1, kModeClips = 2 };

// Capability bits in the device's hello reply.
constexpr uint8_t kCapRgb = 1 << 0;
constexpr uint8_t kCapText = 1 << 1;
constexpr uint8_t kCapEncoder = 1 << 2;
constexpr uint8_t kCapAftertouch = 1 << 3;

constexpr uint8_t kTransportPlaying = 1 << 0;
constexpr uint8_t kTransportRecording = 1 << 1;
constexpr uint8_t kTransportLoop = 1 << 2;

/// What the screen shows. Written by handleFrame(), read by the LVGL timer.
struct ScreenState {
    std::atomic<bool> dirty{false};
    std::atomic<bool> connected{false};
    std::atomic<uint8_t> dawId{kDawOther};
    std::atomic<uint8_t> beat{0};          // 0 off, 1 bar, 2 beat
    std::atomic<uint32_t> beatCount{0};    // every non-zero beat; the flash consumes `beat`
    std::atomic<uint8_t> transport{0};     // kTransport* bits
    std::atomic<uint8_t> mode{kModePads};

    void setRow(uint8_t row, const uint8_t* text, size_t len);
    void copyRow(uint8_t row, char* out, size_t outSize);
    void clearRows();

private:
    std::mutex textMutex_;
    char rows_[kTextRows][kTextCols + 1] = {};
};

/// Dispatch one host frame. `args[0]` is the sub byte, `len` counts it.
void handleFrame(const uint8_t* args, unsigned len, DawControlPadLogic& pads,
                 ScreenState& screen, crosspad::IMidiOutput& out);

/// Device → host hello: `F0 7D 1E 01 proto pads rows caps F7`.
void sendHello(crosspad::IMidiOutput& out);

} // namespace dawcontrol
