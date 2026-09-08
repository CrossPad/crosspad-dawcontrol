/**
 * @file DawControlApp.cpp
 * @brief DAW Control — the screen half of the DAW controller surface.
 *
 * The pads and LEDs are DawControlPadLogic, the wire protocol is
 * DawProtocol; this file only draws what they leave in ScreenState and
 * wires the host SysEx callback for the lifetime of the app. Every LVGL
 * call happens on the LVGL task, from create/destroy or the 50 ms timer.
 */

#include "crosspad-dawcontrol/DawControlApp.hpp"
#include "crosspad-dawcontrol/DawControlPadLogic.hpp"
#include "crosspad-dawcontrol/DawControlState.hpp"
#include "crosspad-dawcontrol/DawProtocol.hpp"

#if __has_include("pc_stubs/PcApp.hpp")
  #include "pc_stubs/PcApp.hpp"
#else
  #include "crosspad-gui/app/App.hpp"
#endif

#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/midi/IMidiOutput.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/protocol/Stm32MessageHandler.hpp"

#include "lvgl.h"

#include <cstdio>
#include <memory>

#ifdef ESP_PLATFORM
extern "C" {
  extern lv_indev_t* indev_encoderTop;
}
#define DAWCONTROL_USE_ENCODER 1
#endif

namespace {

constexpr uint32_t kTickMs = 50;
constexpr uint32_t kBeatFlashTicks = 3;   // 150 ms of dot per beat

lv_obj_t* s_root = nullptr;
lv_obj_t* s_hostLabel = nullptr;
lv_obj_t* s_rows[dawcontrol::kTextRows] = {};
lv_obj_t* s_transportLabel = nullptr;
lv_obj_t* s_beatDot = nullptr;
lv_timer_t* s_timer = nullptr;
uint32_t s_beatTicksLeft = 0;

std::shared_ptr<DawControlPadLogic> s_padLogic;
dawcontrol::ScreenState s_screen;

#ifdef DAWCONTROL_USE_ENCODER
lv_group_t* s_encGroup = nullptr;
lv_group_t* s_prevEncGroup = nullptr;
#endif

const lv_color_t kAccent = lv_color_hex(0x7ec8e3);
const lv_color_t kDim = lv_color_hex(0x506070);
const lv_color_t kBeatBar = lv_color_hex(0xffb347);
const lv_color_t kBeatBeat = lv_color_hex(0x60a0ff);
const lv_color_t kBeatOff = lv_color_hex(0x203040);

const char* daw_name(uint8_t id)
{
    switch (id) {
    case dawcontrol::kDawFlStudio: return "FL Studio";
    case dawcontrol::kDawAbleton: return "Ableton Live";
    default: return "host";
    }
}

const char* mode_name(uint8_t mode)
{
    switch (mode) {
    case dawcontrol::kModeSteps: return "steps";
    case dawcontrol::kModeClips: return "clips";
    default: return "pads";
    }
}

void redraw()
{
    if (!s_root) return;
    const bool connected = s_screen.connected.load();
    if (s_hostLabel) {
        if (connected) {
            lv_label_set_text_fmt(s_hostLabel, "%s  |  %s",
                                  daw_name(s_screen.dawId.load()),
                                  mode_name(s_screen.mode.load()));
            lv_obj_set_style_text_color(s_hostLabel, kAccent, 0);
        } else {
            lv_label_set_text(s_hostLabel, "no host - install the DAW script");
            lv_obj_set_style_text_color(s_hostLabel, kDim, 0);
        }
    }
    char buf[dawcontrol::kTextCols + 1];
    for (uint8_t r = 0; r < dawcontrol::kTextRows; ++r) {
        if (!s_rows[r]) continue;
        s_screen.copyRow(r, buf, sizeof(buf));
        lv_label_set_text(s_rows[r], buf);
    }
    if (s_transportLabel) {
        const uint8_t t = s_screen.transport.load();
        lv_label_set_text_fmt(s_transportLabel, "%s %s %s  |  note %d ch %d",
                              (t & dawcontrol::kTransportPlaying) ? LV_SYMBOL_PLAY : LV_SYMBOL_STOP,
                              (t & dawcontrol::kTransportRecording) ? "REC" : "   ",
                              (t & dawcontrol::kTransportLoop) ? LV_SYMBOL_LOOP : "  ",
                              s_padLogic ? s_padLogic->baseNote() : 0,
                              s_padLogic ? s_padLogic->channel() + 1 : 0);
    }
}

void tick_cb(lv_timer_t*)
{
    if (s_screen.dirty.exchange(false)) redraw();

    // The beat value is a level, not an edge: the host sends 1/2 on every
    // beat and 0 in between, so a flash is triggered on any non-zero write.
    const uint8_t beat = s_screen.beat.exchange(0);
    if (beat != 0) {
        s_beatTicksLeft = kBeatFlashTicks;
        if (s_beatDot) {
            lv_obj_set_style_bg_color(s_beatDot, beat == 1 ? kBeatBar : kBeatBeat, 0);
        }
    } else if (s_beatTicksLeft > 0 && --s_beatTicksLeft == 0 && s_beatDot) {
        lv_obj_set_style_bg_color(s_beatDot, kBeatOff, 0);
    }
}

void back_cb(lv_event_t* e)
{
    auto* app = static_cast<App*>(lv_event_get_user_data(e));
    if (app) app->destroyAppAsync();
}

void host_frame(const uint8_t* args, unsigned len)
{
    if (!s_padLogic) return;
    dawcontrol::handleFrame(args, len, *s_padLogic, s_screen, crosspad::getMidiOutput());
}

lv_obj_t* make_row(lv_obj_t* parent, const char* text, lv_color_t color)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

// -- audio interface bar: what the DAW records from, and whether USB audio is on --
//
// The host can only pick "[Default input]"; which physical input that carries
// is the board's decision. Presets are the platform's route table (Settings ->
// USB uses the same one), so the two never disagree.
constexpr uint8_t kInputPresetCount = 5;
const char* const kInputPresetNames[kInputPresetCount] = {"Mics", "Line in", "Spk bus", "HP bus", "Mix"};
lv_obj_t* s_inputBtns[kInputPresetCount] = {};
lv_obj_t* s_usbBtn = nullptr;

void paint_audio_bar()
{
    crosspad_gui::AudioRouteState st{};
    const bool have = crosspad_gui::getGuiPlatform().getAudioRoute(st);
    for (uint8_t i = 0; i < kInputPresetCount; ++i) {
        if (!s_inputBtns[i]) continue;
        const bool on = have && st.preset == i;
        lv_obj_set_style_bg_color(s_inputBtns[i], on ? kAccent : lv_color_hex(0x203040), 0);
        lv_obj_set_style_text_color(s_inputBtns[i], on ? lv_color_hex(0x0a0e14) : kDim, 0);
    }
    if (s_usbBtn) {
        auto* cfg = crosspad::CrosspadSettings::getInstance();
        const bool on = cfg && cfg->usbAudioEnabled;
        lv_obj_set_style_bg_color(s_usbBtn, on ? kBeatBar : lv_color_hex(0x203040), 0);
        lv_obj_set_style_text_color(s_usbBtn, on ? lv_color_hex(0x0a0e14) : kDim, 0);
    }
}

void input_preset_cb(lv_event_t* e)
{
    const auto idx = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    crosspad_gui::getGuiPlatform().setAudioRoutePreset(idx);
    paint_audio_bar();
}

void usb_audio_cb(lv_event_t*)
{
    auto* cfg = crosspad::CrosspadSettings::getInstance();
    const bool on = cfg && cfg->usbAudioEnabled;
    // Re-enumerates USB; the DAW keeps its MIDI through the host's hub port.
    crosspad_gui::getGuiPlatform().setUsbAudioEnabled(!on);
    paint_audio_bar();
}

lv_obj_t* make_small_button(lv_obj_t* parent, const char* text, lv_event_cb_t cb, void* user)
{
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_height(btn, 24);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_pad_all(btn, 2, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    lv_obj_t* l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_center(l);
#ifdef DAWCONTROL_USE_ENCODER
    if (s_encGroup) lv_group_add_obj(s_encGroup, btn);
#endif
    return btn;
}

void make_audio_bar(lv_obj_t* parent)
{
    auto& platform = crosspad_gui::getGuiPlatform();
    if (!platform.audioRouteSupported() && !platform.usbAudioSupported()) return;
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, 30);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    if (platform.audioRouteSupported()) {
        for (uint8_t i = 0; i < kInputPresetCount; ++i) {
            s_inputBtns[i] = make_small_button(bar, kInputPresetNames[i], input_preset_cb,
                                               reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        }
    }
    if (platform.usbAudioSupported()) {
        s_usbBtn = make_small_button(bar, "USB audio", usb_audio_cb, nullptr);
    }
    paint_audio_bar();
}

} // namespace

void dawcontrol::snapshot(StateSnapshot& out)
{
    out.running = s_root != nullptr;
    out.connected = s_screen.connected.load();
    out.dawId = s_screen.dawId.load();
    out.beat = s_screen.beat.load();
    out.beatCount = s_screen.beatCount.load();
    out.transport = s_screen.transport.load();
    out.mode = s_screen.mode.load();
    for (uint8_t r = 0; r < kTextRows; ++r) {
        s_screen.copyRow(r, out.rows[r], sizeof(out.rows[r]));
    }
}

lv_obj_t* DawControl_create(lv_obj_t* parent, App* app)
{
#ifdef DAWCONTROL_USE_ENCODER
    if (indev_encoderTop) s_prevEncGroup = lv_indev_get_group(indev_encoderTop);
    if (!s_encGroup) s_encGroup = lv_group_create();
    if (indev_encoderTop) lv_indev_set_group(indev_encoderTop, s_encGroup);
#endif

    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x0a0e14), 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 6, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_root, 4, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    // -- title bar: Back, name, beat dot --
    lv_obj_t* bar = lv_obj_create(s_root);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, 30);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* backBtn = lv_button_create(bar);
    lv_obj_set_size(backBtn, 62, 26);
    lv_obj_add_event_cb(backBtn, back_cb, LV_EVENT_CLICKED, app);
    lv_obj_t* backLabel = lv_label_create(backBtn);
    lv_label_set_text(backLabel, LV_SYMBOL_LEFT " Back");
    lv_obj_center(backLabel);

    lv_obj_t* title = lv_label_create(bar);
    lv_label_set_text(title, "DAW Control");
    lv_obj_set_style_text_color(title, kAccent, 0);
    lv_obj_set_flex_grow(title, 1);

    s_beatDot = lv_obj_create(bar);
    lv_obj_set_size(s_beatDot, 18, 18);
    lv_obj_set_style_radius(s_beatDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_beatDot, kBeatOff, 0);
    lv_obj_set_style_border_width(s_beatDot, 0, 0);
    lv_obj_remove_flag(s_beatDot, LV_OBJ_FLAG_SCROLLABLE);

#ifdef DAWCONTROL_USE_ENCODER
    if (s_encGroup) {
        lv_group_add_obj(s_encGroup, backBtn);
        lv_group_focus_obj(backBtn);
    }
#endif

    // -- what the host says --
    s_hostLabel = make_row(s_root, "", kDim);
    for (uint8_t r = 0; r < dawcontrol::kTextRows; ++r) {
        s_rows[r] = make_row(s_root, "", lv_color_hex(0xe0e6ec));
        lv_obj_set_style_text_font(s_rows[r], &lv_font_montserrat_18, 0);
    }
    s_transportLabel = make_row(s_root, "", kDim);
    make_audio_bar(s_root);

    // -- pad logic: registration only, activation is framework-owned --
    if (!s_padLogic) {
        s_padLogic = std::make_shared<DawControlPadLogic>();
        crosspad::getPadManager().registerPadLogic("DawControl", s_padLogic);
    }

    // Host SysEx arrives through the platform's catalog decoder. A platform
    // without one (the PC simulator has no USB host) runs the surface
    // host-less: pads still send notes, the screen says so.
    if (auto* h = crosspad::getPlatformServices().stm32MessageHandler) {
        h->setDawCallback(host_frame);
    }

    s_screen.dirty.store(true);
    s_timer = lv_timer_create(tick_cb, kTickMs, nullptr);

    if (app) {
        app->setOnShow([](lv_obj_t*) {
            if (s_timer) lv_timer_resume(s_timer);
#ifdef DAWCONTROL_USE_ENCODER
            if (indev_encoderTop && s_encGroup) {
                lv_indev_set_group(indev_encoderTop, s_encGroup);
            }
#endif
            // A host script that was already running re-syncs on this:
            // it resends colours, text and transport on any hello.
            dawcontrol::sendHello(crosspad::getMidiOutput());
        });
        app->setOnHide([](lv_obj_t*) {
            if (s_timer) lv_timer_pause(s_timer);
#ifdef DAWCONTROL_USE_ENCODER
            if (indev_encoderTop && s_prevEncGroup) {
                lv_indev_set_group(indev_encoderTop, s_prevEncGroup);
            }
#endif
        });
    }

    return s_root;
}

void DawControl_destroy(lv_obj_t* root)
{
    if (auto* h = crosspad::getPlatformServices().stm32MessageHandler) {
        h->setDawCallback(nullptr);
    }
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    // The host's picture of the surface dies with the app: colours and text
    // are cleared so a restart starts as "no host" until the next hello.
    s_screen.connected.store(false);
    s_screen.dawId.store(dawcontrol::kDawOther);
    s_screen.beat.store(0);
    s_screen.transport.store(0);
    s_screen.clearRows();
    if (s_padLogic) s_padLogic->clearHostColors();

    s_hostLabel = nullptr;
    for (auto& r : s_rows) r = nullptr;
    s_transportLabel = nullptr;
    s_beatDot = nullptr;
    for (auto& b : s_inputBtns) b = nullptr;
    s_usbBtn = nullptr;
    s_root = nullptr;
    s_beatTicksLeft = 0;

#ifdef DAWCONTROL_USE_ENCODER
    if (indev_encoderTop && s_prevEncGroup) {
        lv_indev_set_group(indev_encoderTop, s_prevEncGroup);
    }
    if (s_encGroup) {
        lv_group_delete(s_encGroup);
        s_encGroup = nullptr;
    }
    s_prevEncGroup = nullptr;
#endif

    lv_obj_delete_async(root);
}

REGISTER_APP_PL(DawControl, nullptr, "dawcontrol.png",
                DawControl_create, DawControl_destroy,
                nullptr, nullptr, nullptr, 0, "DawControl")
