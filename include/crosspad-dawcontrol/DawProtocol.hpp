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
constexpr uint32_t kHostTimeoutMs = 5000;    // silence after which the host is gone
constexpr uint32_t kHelloRetryMs = 2000;     // unsolicited hello while no host answers
constexpr uint32_t kHelloEchoGuardMs = 1000; // a host hello this soon after our own is the answer to it
constexpr float kTiltSpanDeg = 30.0f;        // roll that reaches full scale (64 ± 63)
constexpr uint8_t kTiltCentre = 64;
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
    kSubPad = 0x09,       // device -> host: pad state, control mode only
    kSubPing = 0x0A,      // host -> device: still here (no args, no reply)
    kSubTilt = 0x0B,      // device -> host: pad value — roll since that pad went down, control mode
    kSubBye = 0x7F,
};

enum DawId : uint8_t { kDawOther = 0, kDawFlStudio = 1, kDawAbleton = 2 };
// 0..2 are what the host shows; kModeControl is the board's own: pads stop
// playing notes and report presses as `pad` frames for the host to act on.
enum PadMode : uint8_t { kModePads = 0, kModeSteps = 1, kModeClips = 2, kModeControl = 3 };

// Capability bits in the device's hello reply.
constexpr uint8_t kCapRgb = 1 << 0;
constexpr uint8_t kCapText = 1 << 1;
constexpr uint8_t kCapEncoder = 1 << 2;
constexpr uint8_t kCapAftertouch = 1 << 3;
constexpr uint8_t kCapControl = 1 << 4;      // understands kModeControl and sends `pad`

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
    std::atomic<int64_t> lastHelloUs{0};   // when the device last sent its hello

    void setRow(uint8_t row, const uint8_t* text, size_t len);
    void copyRow(uint8_t row, char* out, size_t outSize);
    void clearRows();

private:
    std::mutex textMutex_;
    char rows_[kTextRows][kTextCols + 1] = {};
};

/// Dispatch one host frame. `args[0]` is the sub byte, `len` counts it.
/// Every frame, `ping` included, counts as the host being alive; the app
/// keeps the clock and drops the host after kHostTimeoutMs of silence.
/// `nowUs` is that clock, for the hello echo guard.
void handleFrame(const uint8_t* args, unsigned len, DawControlPadLogic& pads,
                 ScreenState& screen, crosspad::IMidiOutput& out, int64_t nowUs);

/// The host said bye, or went silent: back to "no host" — rows, colours,
/// transport cleared, so the screen never shows a DAW that is not there.
void hostGone(DawControlPadLogic& pads, ScreenState& screen);

/// Device → host hello: `F0 7D 1E 01 proto pads rows caps F7`, stamped in
/// `screen` so a host hello arriving within kHelloEchoGuardMs is recognised
/// as the answer to it and not answered again (the host answers every device
/// hello with its own; that is what stops the ping-pong). A hello from a
/// second host — the bench beside a running DAW — is answered.
void sendHello(crosspad::IMidiOutput& out, ScreenState& screen, int64_t nowUs);

/// Device → host `07 mode`: sent after every host hello and on every double
/// tap, so the host paints the layout the board is actually in.
void sendMode(crosspad::IMidiOutput& out, uint8_t mode);

/// Device → host `09 pad state`: a control-mode press (1) or release (0).
void sendPad(crosspad::IMidiOutput& out, uint8_t padIdx, bool pressed);

/// Device → host `0B tilt pad value`: the board's roll relative to where it
/// was when `pad` went down, kTiltCentre level, ±63 = ±kTiltSpanDeg. Sent
/// while a control-mode pad is held and the value changes; the host turns it
/// into whatever that pad controls (the FL script: tempo on the metronome pad).
void sendTilt(crosspad::IMidiOutput& out, uint8_t padIdx, uint8_t value);

} // namespace dawcontrol
