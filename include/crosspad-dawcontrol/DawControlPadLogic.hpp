#pragma once

#include "crosspad/pad/IPadLogicHandler.hpp"

#include <atomic>
#include <cstdint>

/**
 * Pad handler for DAW Control.
 *
 * A pad is a MIDI note to the host (`baseNote + pad` on `channel`, velocity
 * from the pad, pressure as polyphonic aftertouch) and an LED the host owns:
 * the resting colour comes from the host's `led`/`led_all` frames, a press
 * flashes a brighter shade of it, a release restores it. With no host yet
 * the pads wear a dim default so the surface is visibly alive.
 *
 * With an active handler PadManager does not send its own notes, which is
 * the point: the layout the host asked for (`config`) is applied here.
 *
 * Runs on the pad thread and the MIDI receive task; nothing here touches LVGL.
 */
class DawControlPadLogic : public crosspad::IPadLogicHandler {
public:
    static constexpr uint8_t kDefaultBaseNote = 36;   // FPC / GM drum layout
    static constexpr uint8_t kDefaultChannel = 9;     // channel 10, 0-based

    void onActivate(crosspad::PadManager& padManager) override;
    void onDeactivate(crosspad::PadManager& padManager) override;
    void onPadPress(crosspad::PadManager& padManager, uint8_t padIdx,
                    uint8_t velocity) override;
    void onPadRelease(crosspad::PadManager& padManager, uint8_t padIdx) override;
    void onPadPressure(crosspad::PadManager& padManager, uint8_t padIdx,
                       uint8_t pressure) override;

    /// Host-assigned resting colour; repaints the pad unless it is held.
    void setHostColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b);
    /// Back to the no-host palette (host said bye, or the app is leaving).
    void clearHostColors();

    void setLayout(uint8_t baseNote, uint8_t channel);
    uint8_t baseNote() const { return baseNote_.load(); }
    uint8_t channel() const { return channel_.load(); }

private:
    void paint(uint8_t padIdx, bool held);
    uint32_t restingColor(uint8_t padIdx) const;

    std::atomic<uint32_t> hostColor_[16] = {};   // 0xRRGGBB, 0 = none
    std::atomic<bool> held_[16] = {};
    std::atomic<uint8_t> baseNote_{kDefaultBaseNote};
    std::atomic<uint8_t> channel_{kDefaultChannel};
    crosspad::PadManager* padManager_ = nullptr;
};
