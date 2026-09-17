#include "crosspad-dawcontrol/DawControlPadLogic.hpp"
#include "crosspad-dawcontrol/DawProtocol.hpp"

#include "crosspad/midi/IMidiOutput.hpp"
#include "crosspad/pad/PadManager.hpp"

namespace {

constexpr uint32_t kIdleColor = 0x101830;   // dim blue: surface alive, no host
constexpr uint32_t kIdleFlash = 0x60A0FF;   // pressed with no host colour

// A press shows as the host colour pushed towards white, so the feedback
// reads on dark and saturated colours alike.
uint32_t brighten(uint32_t rgb)
{
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        uint32_t c = (rgb >> shift) & 0xFF;
        c = c + ((255 - c) * 3) / 4;
        out |= c << shift;
    }
    return out;
}

} // namespace

void DawControlPadLogic::onActivate(crosspad::PadManager& padManager)
{
    padManager_ = &padManager;
    for (uint8_t i = 0; i < 16; ++i) {
        held_[i].store(false);
        paint(i, false);
    }
}

void DawControlPadLogic::onDeactivate(crosspad::PadManager& padManager)
{
    releaseHeldNotes();
    for (uint8_t i = 0; i < 16; ++i) padManager.setPadColor(i, 0, 0, 0);
    padManager_ = nullptr;
}

// Every note-on this handler sent gets its note-off; a control-mode press
// gets its release. Called on deactivate, layout change and mode change.
void DawControlPadLogic::releaseHeldNotes()
{
    auto& out = crosspad::getMidiOutput();
    const bool control = control_.load();
    for (uint8_t i = 0; i < 16; ++i) {
        if (!held_[i].exchange(false)) continue;
        if (control) dawcontrol::sendPad(out, i, false);
        else         out.sendNoteOff(baseNote_.load() + i, 0, channel_.load());
    }
}

void DawControlPadLogic::setControlMode(bool control)
{
    if (control_.load() == control) return;
    releaseHeldNotes();
    control_.store(control);
}

void DawControlPadLogic::onPadPress(crosspad::PadManager&, uint8_t padIdx,
                                    uint8_t velocity)
{
    if (padIdx >= 16) return;
    held_[padIdx].store(true);
    if (control_.load()) {
        dawcontrol::sendPad(crosspad::getMidiOutput(), padIdx, true);
    } else {
        crosspad::getMidiOutput().sendNoteOn(baseNote_.load() + padIdx,
                                             velocity ? velocity : 1, channel_.load());
    }
    paint(padIdx, true);
}

void DawControlPadLogic::onPadRelease(crosspad::PadManager&, uint8_t padIdx)
{
    if (padIdx >= 16) return;
    // A press that went out as a note comes back as a note-off even if the
    // mode flipped in between: the flip released it already, so nothing here.
    if (!held_[padIdx].exchange(false)) { paint(padIdx, false); return; }
    if (control_.load()) {
        dawcontrol::sendPad(crosspad::getMidiOutput(), padIdx, false);
    } else {
        crosspad::getMidiOutput().sendNoteOff(baseNote_.load() + padIdx, 0, channel_.load());
    }
    paint(padIdx, false);
}

void DawControlPadLogic::onPadPressure(crosspad::PadManager&, uint8_t padIdx,
                                       uint8_t pressure)
{
    if (padIdx >= 16 || control_.load()) return;
    crosspad::getMidiOutput().sendAftertouch(baseNote_.load() + padIdx,
                                             pressure > 127 ? 127 : pressure,
                                             channel_.load());
}

void DawControlPadLogic::setHostColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b)
{
    if (padIdx >= 16) return;
    hostColor_[padIdx].store(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    if (!held_[padIdx].load()) paint(padIdx, false);
}

void DawControlPadLogic::clearHostColors()
{
    for (uint8_t i = 0; i < 16; ++i) {
        hostColor_[i].store(0);
        if (!held_[i].load()) paint(i, false);
    }
}

void DawControlPadLogic::setLayout(uint8_t baseNote, uint8_t channel)
{
    // A layout change under a held pad would orphan its note-off, so flush
    // first.
    releaseHeldNotes();
    baseNote_.store(baseNote > 127 - 15 ? 127 - 15 : baseNote);
    channel_.store(channel & 0x0F);
}

uint32_t DawControlPadLogic::restingColor(uint8_t padIdx) const
{
    return hostColor_[padIdx].load();
}

void DawControlPadLogic::paint(uint8_t padIdx, bool held)
{
    if (!padManager_) return;
    const uint32_t host = restingColor(padIdx);
    uint32_t rgb;
    if (held) {
        rgb = host ? brighten(host) : kIdleFlash;
    } else {
        rgb = host ? host : kIdleColor;
    }
    padManager_->setPadColor(padIdx, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}
