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
#include "crosspad/event/EventData.hpp"
#include "crosspad/event/IEventBus.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/midi/IMidiOutput.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/platform/IClock.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/protocol/Stm32MessageHandler.hpp"
#include "crosspad/status/CrosspadStatus.hpp"

#include "lvgl.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <memory>

// Fed by the platform's HOST_INFO / FW_LATEST companion verbs.
extern crosspad::CrosspadStatus status;

#ifdef ESP_PLATFORM
extern "C" {
  extern lv_indev_t* indev_encoderTop;
}
#define DAWCONTROL_USE_ENCODER 1
#endif

namespace {

constexpr uint32_t kTickMs = 50;
constexpr uint32_t kBeatFlashTicks = 3;   // 150 ms of dot per beat
constexpr uint32_t kHostRowTicks = 1000 / kTickMs;   // host stats and clock, once a second
constexpr uint32_t kHelloRetryTicks = dawcontrol::kHelloRetryMs / kTickMs;
constexpr uint32_t kMbPerGb = 1024;
constexpr int64_t kLaunchWaitUs = 90LL * 1000 * 1000;   // FL under Wine can take a minute to come up

lv_obj_t* s_root = nullptr;
lv_obj_t* s_hostLabel = nullptr;
lv_obj_t* s_hostInfoLabel = nullptr;
lv_obj_t* s_fwLabel = nullptr;
uint32_t s_hostRowTicksLeft = 0;
lv_obj_t* s_rows[dawcontrol::kTextRows] = {};
lv_obj_t* s_transportLabel = nullptr;
lv_obj_t* s_beatDot = nullptr;
lv_timer_t* s_timer = nullptr;
uint32_t s_beatTicksLeft = 0;

std::shared_ptr<DawControlPadLogic> s_padLogic;
dawcontrol::ScreenState s_screen;
crosspad::SubscriptionHandle s_motionSub = nullptr;   // double tap flips keyboard/control
std::atomic<int64_t> s_lastHostUs{0};                 // stamped on every host frame
uint32_t s_helloRetryTicksLeft = 0;

// DAWs the hub can start, one button each, in daw_id order.
constexpr uint8_t kLaunchable[] = {dawcontrol::kDawFlStudio, dawcontrol::kDawAbleton,
                                   dawcontrol::kDawReaper};
constexpr uint8_t kLaunchableCount = sizeof(kLaunchable);
lv_obj_t* s_launchBar = nullptr;
lv_obj_t* s_launchBtns[kLaunchableCount] = {};
uint8_t s_launchingDaw = dawcontrol::kDawOther;   // pressed, not yet up
int64_t s_launchingSinceUs = 0;

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
    case dawcontrol::kDawReaper: return "REAPER";
    default: return "host";
    }
}

const char* mode_name(uint8_t mode)
{
    switch (mode) {
    case dawcontrol::kModeSteps: return "steps";
    case dawcontrol::kModeClips: return "clips";
    case dawcontrol::kModeControl: return "control";
    default: return "pads";
    }
}

void draw_launch();

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
        }
    }
    draw_launch();
    char buf[dawcontrol::kTextCols + 1];
    for (uint8_t r = 0; r < dawcontrol::kTextRows; ++r) {
        if (!s_rows[r]) continue;
        s_screen.copyRow(r, buf, sizeof(buf));
        lv_label_set_text(s_rows[r], buf);
    }
    if (s_transportLabel) {
        const uint8_t t = s_screen.transport.load();
        // The layout is named once, in the host row; without a host that row
        // says so instead, and control mode shows here.
        const bool control = s_padLogic && s_padLogic->controlMode();
        if (control) {
            lv_label_set_text_fmt(s_transportLabel, "%s %s %s%s",
                                  (t & dawcontrol::kTransportPlaying) ? LV_SYMBOL_PLAY : LV_SYMBOL_STOP,
                                  (t & dawcontrol::kTransportRecording) ? "REC" : "   ",
                                  (t & dawcontrol::kTransportLoop) ? LV_SYMBOL_LOOP : "  ",
                                  connected ? "" : "  |  control");
        } else {
            lv_label_set_text_fmt(s_transportLabel, "%s %s %s  |  note %d ch %d",
                                  (t & dawcontrol::kTransportPlaying) ? LV_SYMBOL_PLAY : LV_SYMBOL_STOP,
                                  (t & dawcontrol::kTransportRecording) ? "REC" : "   ",
                                  (t & dawcontrol::kTransportLoop) ? LV_SYMBOL_LOOP : "  ",
                                  s_padLogic ? s_padLogic->baseNote() : 0,
                                  s_padLogic ? s_padLogic->channel() + 1 : 0);
        }
    }
}

// GB with one decimal, from MB, without LVGL's float formatting.
void format_gb(char* out, size_t cap, uint32_t mb)
{
    const uint32_t tenths = mb * 10 / kMbPerGb;
    snprintf(out, cap, "%lu.%lu", static_cast<unsigned long>(tenths / 10),
             static_cast<unsigned long>(tenths % 10));
}

// What the host companion worker reports: its load and clock, and whether it
// knows of newer firmware. Separate from the DAW rows — the worker is not the
// DAW script and keeps reporting while the DAW shows nothing.
void draw_host_rows()
{
    if (s_hostInfoLabel) {
        if (status.hostAlive(crosspad::getClock().getTimeUs())) {
            char used[12], total[12], clock[8] = "--:--";
            format_gb(used, sizeof(used), status.hostRamUsedMb);
            format_gb(total, sizeof(total), status.hostRamTotalMb);
            const time_t now = time(nullptr);
            struct tm local;
            if (localtime_r(&now, &local)) strftime(clock, sizeof(clock), "%H:%M", &local);
            lv_label_set_text_fmt(s_hostInfoLabel, "PC  CPU %u%%  RAM %s/%s GB  %s",
                                  static_cast<unsigned>(status.hostCpuPct), used, total, clock);
            lv_obj_set_style_text_color(s_hostInfoLabel, kAccent, 0);
        } else {
            lv_label_set_text(s_hostInfoLabel, "PC companion not running");
            lv_obj_set_style_text_color(s_hostInfoLabel, kDim, 0);
        }
    }
    if (s_fwLabel) {
        if (status.fwUpdateAvailable) {
            lv_label_set_text_fmt(s_fwLabel, "v%s available - crosspad.app/device", status.fwLatest);
            lv_obj_set_style_text_color(s_fwLabel, kBeatBar, 0);
        } else if (status.fwLatest[0]) {
            lv_label_set_text(s_fwLabel, "Firmware up to date");
            lv_obj_set_style_text_color(s_fwLabel, kDim, 0);
        } else {
            lv_label_set_text(s_fwLabel, "");
        }
    }
}

bool daw_bit(uint8_t mask, uint8_t dawId) { return (mask >> dawId) & 1; }

// No DAW script answering: what the hub can do about it. The hub reports
// which DAWs it can start and which already run (HOST_INFO `daws=`); a
// running one needs its script, not a second start, so it is named instead.
void draw_launch()
{
    if (!s_hostLabel) return;
    const bool connected = s_screen.connected.load();
    const bool hub = status.hostAlive(crosspad::getClock().getTimeUs());
    const uint8_t installed = hub ? status.hostDawsInstalled : 0;
    const uint8_t running = hub ? status.hostDawsRunning : 0;

    if (s_launchingDaw != dawcontrol::kDawOther &&
        (connected || !hub || daw_bit(running, s_launchingDaw) ||
         crosspad::getClock().getTimeUs() - s_launchingSinceUs > kLaunchWaitUs)) {
        s_launchingDaw = dawcontrol::kDawOther;
    }

    bool anyButton = false;
    for (uint8_t i = 0; i < kLaunchableCount; ++i) {
        if (!s_launchBtns[i]) continue;
        const uint8_t id = kLaunchable[i];
        const bool show = !connected && s_launchingDaw == dawcontrol::kDawOther &&
                          daw_bit(installed, id) && !daw_bit(running, id);
        if (show) {
            lv_obj_remove_flag(s_launchBtns[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_launchBtns[i], LV_OBJ_FLAG_HIDDEN);
        }
        anyButton |= show;
    }
    // The bar takes the DAW text rows' place: they are empty without a DAW,
    // and the screen has no height for both.
    if (s_launchBar) {
        if (anyButton) {
            lv_obj_remove_flag(s_launchBar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_launchBar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (auto* row : s_rows) {
        if (!row) continue;
        if (anyButton) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (connected) return;

    uint8_t runningId = dawcontrol::kDawOther;
    for (uint8_t id : kLaunchable) {
        if (daw_bit(running, id)) { runningId = id; break; }
    }
    if (s_launchingDaw != dawcontrol::kDawOther) {
        lv_label_set_text_fmt(s_hostLabel, "Starting %s...", daw_name(s_launchingDaw));
        lv_obj_set_style_text_color(s_hostLabel, kBeatBar, 0);
    } else if (runningId != dawcontrol::kDawOther) {
        lv_label_set_text_fmt(s_hostLabel, "%s running - script not answering", daw_name(runningId));
        lv_obj_set_style_text_color(s_hostLabel, kBeatBar, 0);
    } else if (anyButton) {
        lv_label_set_text(s_hostLabel, "no DAW running");
        lv_obj_set_style_text_color(s_hostLabel, kDim, 0);
    } else {
        lv_label_set_text(s_hostLabel, "no host - install the DAW script");
        lv_obj_set_style_text_color(s_hostLabel, kDim, 0);
    }
}

void launch_cb(lv_event_t* e)
{
    const auto id = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    // The encoder can still press a button that just hid; ask the hub only
    // for what it last said it can start.
    const bool hub = status.hostAlive(crosspad::getClock().getTimeUs());
    if (!hub || s_screen.connected.load() || s_launchingDaw != dawcontrol::kDawOther ||
        !daw_bit(status.hostDawsInstalled, id) || daw_bit(status.hostDawsRunning, id)) {
        return;
    }
    dawcontrol::sendLaunch(crosspad::getMidiOutput(), id);
    s_launchingDaw = id;
    s_launchingSinceUs = crosspad::getClock().getTimeUs();
    draw_launch();
}

// A DAW that was closed, or a script that was reloaded, sends no bye. The
// host pings once a second; after kHostTimeoutMs without any frame the
// screen goes back to "no host", and hello goes out every kHelloRetryMs
// until a host answers — that is the reconnect, no app restart needed.
void watch_host()
{
    const int64_t now = crosspad::getClock().getTimeUs();
    if (s_screen.connected.load()) {
        if (now - s_lastHostUs.load() > (int64_t)dawcontrol::kHostTimeoutMs * 1000) {
            if (s_padLogic) dawcontrol::hostGone(*s_padLogic, s_screen);
            s_helloRetryTicksLeft = kHelloRetryTicks;
        }
        return;
    }
    if (s_helloRetryTicksLeft == 0 || --s_helloRetryTicksLeft == 0) {
        s_helloRetryTicksLeft = kHelloRetryTicks;
        dawcontrol::sendHello(crosspad::getMidiOutput(), s_screen, now);
    }
}

void tick_cb(lv_timer_t*)
{
    watch_host();
    if (s_screen.dirty.exchange(false)) redraw();
    if (s_hostRowTicksLeft == 0 || --s_hostRowTicksLeft == 0) {
        s_hostRowTicksLeft = kHostRowTicks;
        draw_host_rows();
        draw_launch();
    }

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
    const int64_t now = crosspad::getClock().getTimeUs();
    s_lastHostUs.store(now);
    dawcontrol::handleFrame(args, len, *s_padLogic, s_screen, crosspad::getMidiOutput(), now);
}

// Keyboard <-> control, from the board's double tap. Arrives on the event bus
// off the LVGL thread, so the flip is deferred to it; the host learns the
// new mode from a `mode` frame and repaints the pads for it.
void toggle_control_mode(void*)
{
    if (!s_root || !s_padLogic) return;
    const bool control = !s_padLogic->controlMode();
    s_padLogic->setControlMode(control);
    s_screen.mode.store(control ? (uint8_t)dawcontrol::kModeControl
                                : (uint8_t)dawcontrol::kModePads);
    dawcontrol::sendMode(crosspad::getMidiOutput(), s_screen.mode.load());
    s_screen.dirty.store(true);
}

void motion_gesture_cb(const void* data, crosspad::EventType, void*)
{
    auto* d = static_cast<const crosspad::MotionGestureData*>(data);
    if (!d || d->gesture != crosspad::MotionGesture::DoubleTap) return;
    lv_async_call(toggle_control_mode, nullptr);
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

void make_launch_bar(lv_obj_t* parent)
{
    s_launchBar = lv_obj_create(parent);
    lv_obj_set_width(s_launchBar, LV_PCT(100));
    lv_obj_set_height(s_launchBar, 30);
    lv_obj_set_style_bg_opa(s_launchBar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_launchBar, 0, 0);
    lv_obj_set_style_pad_all(s_launchBar, 0, 0);
    lv_obj_set_style_pad_column(s_launchBar, 4, 0);
    lv_obj_set_flex_flow(s_launchBar, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(s_launchBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_launchBar, LV_OBJ_FLAG_HIDDEN);
    char text[24];
    for (uint8_t i = 0; i < kLaunchableCount; ++i) {
        snprintf(text, sizeof(text), LV_SYMBOL_PLAY " %s", daw_name(kLaunchable[i]));
        s_launchBtns[i] = make_small_button(s_launchBar, text, launch_cb,
                                            reinterpret_cast<void*>(static_cast<uintptr_t>(kLaunchable[i])));
        lv_obj_add_flag(s_launchBtns[i], LV_OBJ_FLAG_HIDDEN);
    }
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
    make_launch_bar(s_root);
    for (uint8_t r = 0; r < dawcontrol::kTextRows; ++r) {
        s_rows[r] = make_row(s_root, "", lv_color_hex(0xe0e6ec));
        lv_obj_set_style_text_font(s_rows[r], &lv_font_montserrat_18, 0);
    }
    s_transportLabel = make_row(s_root, "", kDim);
    s_hostInfoLabel = make_row(s_root, "", kDim);
    s_fwLabel = make_row(s_root, "", kDim);
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
    if (!s_motionSub) {
        s_motionSub = crosspad::getEventBus().subscribe(crosspad::EventType::MotionGesture,
                                                        motion_gesture_cb, nullptr);
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
            dawcontrol::sendHello(crosspad::getMidiOutput(), s_screen,
                                  crosspad::getClock().getTimeUs());
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
    if (s_motionSub) {
        crosspad::getEventBus().unsubscribe(s_motionSub);
        s_motionSub = nullptr;
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
    s_screen.mode.store(dawcontrol::kModePads);   // every start is a keyboard
    if (s_padLogic) {
        s_padLogic->clearHostColors();
        s_padLogic->setControlMode(false);
    }

    s_hostLabel = nullptr;
    for (auto& r : s_rows) r = nullptr;
    s_transportLabel = nullptr;
    s_hostInfoLabel = nullptr;
    s_fwLabel = nullptr;
    s_hostRowTicksLeft = 0;
    s_helloRetryTicksLeft = 0;
    s_beatDot = nullptr;
    for (auto& b : s_inputBtns) b = nullptr;
    s_usbBtn = nullptr;
    s_launchBar = nullptr;
    for (auto& b : s_launchBtns) b = nullptr;
    s_launchingDaw = dawcontrol::kDawOther;
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
