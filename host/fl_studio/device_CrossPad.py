# name=CrossPad
# url=https://github.com/CrossPad/crosspad-dawcontrol
"""
FL Studio MIDI Controller Script for CrossPad running the DAW Control app.

Install: copy this directory (as `CrossPad/`) into
  <Documents>/Image-Line/FL Studio/Settings/Hardware/
(under Wine: ~/.wine/drive_c/users/<you>/Documents/Image-Line/FL Studio/Settings/Hardware/CrossPad/),
then in FL: Options -> MIDI Settings -> select the CrossPad port for both input
and output, controller type "CrossPad", and enable it.

The device side is generic: it plays notes and shows what it is told. This
script is the FL-specific half — it reads the selected channel, FPC pad
colours, pattern name, tempo, transport and the beat, and paints the pads and
LCD rows with them. Frames follow the `daw` section of crosspad-hil
`knowledge/sysex.yaml`; keep the constants below in step with it.

The board has two layouts, switched by a double tap on its case: keyboard
(pads play the selected channel) and control, where a pad is a button the
board reports as a `pad` frame and this script acts on — transport, pattern,
metronome, undo, and mutes for channel rack channels 1–8 whose LEDs glow as
loud as their mixer track is playing right now. The board says which layout
it is in with `mode`.

This file must stay self-contained: FL's embedded Python has no pip.
"""

import time

import channels
import device
import general
import midi
import mixer
import patterns
import plugins
import transport
import ui

# --- protocol constants (mirror of sysex.yaml `daw`) -------------------------

MANUFACTURER = 0x7D
CMD_DAW = 0x1E
SUB_HELLO = 0x01
SUB_LED = 0x02
SUB_LED_ALL = 0x03
SUB_TEXT = 0x04
SUB_BEAT = 0x05
SUB_TRANSPORT = 0x06
SUB_MODE = 0x07
SUB_CONFIG = 0x08
SUB_PAD = 0x09             # device -> host: pad state, control mode only
SUB_PING = 0x0A            # host -> device: still here, once a second
SUB_TILT = 0x0B            # device -> host: pad value — roll since that pad went down
SUB_BYE = 0x7F

PING_PERIOD_S = 1.0        # the board forgets a host silent for 5 s
TILT_CENTRE = 64

MODE_PADS = 0
MODE_CONTROL = 3

DAW_ID_FL_STUDIO = 1
PROTO_VER = 1
PAD_COUNT = 16
TEXT_COLS = 24

# What the pads play: FPC's default layout, C1 upwards on channel 10.
BASE_NOTE = 36
MIDI_CHANNEL = 10          # 1-based, as FL shows it

# The firmware emits the navigation encoder as CC 1 (absolute detent 0..15,
# wrapping) and its switch as CC 64 on channel 1, independent of the app.
CC_ENCODER = 1
CC_ENCODER_BUTTON = 64
ENCODER_DETENTS = 16

FPC_PLUGIN_NAME = "FPC"
FPC_PARAM_COLOR = 2        # plugins.getPadInfo paramIndex: 0 count, 1 semitone, 2 colour

# --- control layout: pad index = row * 4 + column, row 0 at the top ----------

CTRL_PLAY, CTRL_STOP, CTRL_RECORD, CTRL_LOOP_MODE = 0, 1, 2, 3
CTRL_PATTERN_PREV, CTRL_PATTERN_NEXT, CTRL_METRONOME, CTRL_UNDO = 4, 5, 6, 7
CTRL_MUTE_FIRST = 8        # pads 8..15 mute channel rack channels 0..7
CTRL_MUTE_COUNT = 8

FPT_METRONOME = 110        # transport.globalTransport command (midi.FPT_Metronome)
LOOP_MODE_SONG = 1         # transport.getLoopMode(): 0 pattern, 1 song

# Control-mode LED colours as 0xRRGGBB: (off/idle, on/active).
CTRL_COLORS = {
    CTRL_PLAY: (0x104020, 0x30FF60),
    CTRL_STOP: (0x401818, 0xFF4040),
    CTRL_RECORD: (0x401010, 0xFF2020),
    CTRL_LOOP_MODE: (0x403010, 0xFFC020),
    CTRL_PATTERN_PREV: (0x2060FF, 0x2060FF),
    CTRL_PATTERN_NEXT: (0x2060FF, 0x2060FF),
    CTRL_METRONOME: (0x303030, 0xE0E0E0),
    CTRL_UNDO: (0x8040FF, 0x8040FF),
}
PEAK_MODE_BOTH = 2         # mixer.getTrackPeaks mode: 0 left, 1 right, 2 the louder of the two
PEAK_STEPS = 8             # brightness is quantised so the LEDs update on level changes, not every tick
BEAT_BAR_COLOR = 0xFFB347  # metronome pad on the bar, the screen's beat dot colours
BEAT_BEAT_COLOR = 0xE0E0FF
BEAT_FLASH_S = 0.12        # how long the metronome pad holds the flash — FL's own 0 comes too soon to see
MUTED_BRIGHTNESS = 0.06    # a muted channel keeps its hue, barely lit
MIN_LIT_BRIGHTNESS = 0.12  # a silent, unmuted channel is still visibly there
LED_SINGLE_MAX = 4         # up to this many changed pads go out as `led`, more as one `led_all`

# Tempo from the metronome pad: hold it and lean the board left or right.
# Full tilt (±30 deg on the board) is ±TEMPO_TILT_SPAN BPM around the tempo at
# the press; values within TEMPO_SNAP_PULL of a multiple of TEMPO_SNAP_STEP
# snap to it, everything else is whole BPM.
TEMPO_TILT_SPAN = 20.0
TEMPO_SNAP_STEP = 5
TEMPO_SNAP_PULL = 1.5
TEMPO_MIN, TEMPO_MAX = 20.0, 999.0
TILT_DEAD_ZONE = 3         # tilt units either side of centre that still count as "not moved"

# --- state -------------------------------------------------------------------

_last_rows = ["", "", ""]
_last_colors = None
_last_transport = None
_last_encoder = None
_connected = False
_mode = MODE_PADS
_last_ping = 0.0
_metro_flash_until = 0.0   # the metronome pad shows the beat colour until then
_metro_hold = None         # {"tempo0": BPM at the press, "moved": bool} while the metronome pad is down


# --- frame helpers -----------------------------------------------------------

def _send(sub, payload=()):
    frame = bytes([0xF0, MANUFACTURER, CMD_DAW, sub] + [b & 0x7F for b in payload] + [0xF7])
    device.midiOutSysex(frame)


def _rgb7(color):
    """FL colour int (0xRRGGBB, sign-extended) -> 3 x 7-bit r g b."""
    c = color & 0xFFFFFF
    r = (c >> 16) & 0xFF
    g = (c >> 8) & 0xFF
    b = c & 0xFF
    return (r >> 1, g >> 1, b >> 1)


def _text(row, text):
    s = "".join(ch if 0x20 <= ord(ch) <= 0x7E else " " for ch in str(text))[:TEXT_COLS]
    if _last_rows[row] == s:
        return
    _last_rows[row] = s
    _send(SUB_TEXT, [row] + [ord(ch) for ch in s])


# --- what the DAW shows ------------------------------------------------------

def _pad_colors():
    """16 colours: FPC pad colours when the selected channel hosts FPC, else the channel colour."""
    chan = channels.selectedChannel()
    if chan < 0:
        return [(0, 0, 0)] * PAD_COUNT
    colors = None
    try:
        if plugins.isValid(chan) and plugins.getPluginName(chan) == FPC_PLUGIN_NAME:
            colors = [_rgb7(plugins.getPadInfo(chan, -1, FPC_PARAM_COLOR, pad))
                      for pad in range(PAD_COUNT)]
    except Exception:
        colors = None
    if colors is None:
        colors = [_rgb7(channels.getChannelColor(chan))] * PAD_COUNT
    return colors


def _scale(color, brightness):
    """FL colour int scaled to `brightness` (0..1), as 7-bit r g b."""
    r, g, b = _rgb7(color)
    k = max(0.0, min(1.0, brightness))
    return (int(r * k), int(g * k), int(b * k))


def _mute_pad_color(chan):
    """Channel colour, as bright as its mixer track's meter right now; muted = a glimmer."""
    if chan >= channels.channelCount():
        return (0, 0, 0)
    color = channels.getChannelColor(chan)
    if channels.isChannelMuted(chan):
        return _scale(color, MUTED_BRIGHTNESS)
    track = channels.getTargetFxTrack(chan)
    peak = min(1.0, max(0.0, mixer.getTrackPeaks(track, PEAK_MODE_BOTH)))
    level = round(peak * PEAK_STEPS) / PEAK_STEPS
    return _scale(color, max(MIN_LIT_BRIGHTNESS, level))


def _control_colors():
    """16 colours for the control layout: state-lit buttons, then the eight mutes."""
    active = {
        CTRL_PLAY: transport.isPlaying(),
        CTRL_STOP: not transport.isPlaying(),
        CTRL_RECORD: transport.isRecording(),
        CTRL_LOOP_MODE: transport.getLoopMode() == LOOP_MODE_SONG,
        CTRL_PATTERN_PREV: True,
        CTRL_PATTERN_NEXT: True,
        CTRL_METRONOME: ui.isMetronomeEnabled(),
        CTRL_UNDO: True,
    }
    colors = [_rgb7(CTRL_COLORS[pad][1 if active[pad] else 0]) for pad in range(CTRL_MUTE_FIRST)]
    for i in range(CTRL_MUTE_COUNT):
        colors.append(_mute_pad_color(i))
    return colors


def _push_colors(force=False):
    global _last_colors
    colors = _control_colors() if _mode == MODE_CONTROL else _pad_colors()
    if not force and colors == _last_colors:
        return
    if not force and _last_colors is not None:
        changed = [pad for pad in range(PAD_COUNT) if colors[pad] != _last_colors[pad]]
        if len(changed) <= LED_SINGLE_MAX:
            # Meters move one or two pads at a time; a full repaint per tick
            # would be 48 bytes of SysEx for every flicker.
            for pad in changed:
                r, g, b = colors[pad]
                _send(SUB_LED, [pad, r, g, b])
            _last_colors = colors
            return
    _last_colors = colors
    payload = []
    for r, g, b in colors:
        payload += [r, g, b]
    _send(SUB_LED_ALL, payload)


def _push_text():
    chan = channels.selectedChannel()
    name = channels.getChannelName(chan) if chan >= 0 else "-"
    pat = patterns.getPatternName(patterns.patternNumber())
    bpm = mixer.getCurrentTempo() / 1000.0   # default form is milli-BPM; True gives a plain int
    _text(0, name)
    _text(1, pat)
    _text(2, "%.1f BPM" % bpm)


def _push_transport(force=False):
    global _last_transport
    bits = 0
    if transport.isPlaying():
        bits |= 1
    if transport.isRecording():
        bits |= 2
    if ui.isLoopRecEnabled():
        bits |= 4
    if force or bits != _last_transport:
        _last_transport = bits
        _send(SUB_TRANSPORT, [bits])


def _push_all():
    global _last_rows, _last_colors, _last_transport
    _last_rows = ["", "", ""]
    _last_colors = None
    _last_transport = None
    _send(SUB_CONFIG, [BASE_NOTE, MIDI_CHANNEL - 1])
    # The layout is the board's: it answers every hello with its `mode`, and
    # _set_mode repaints for it. Until then paint for the mode last known.
    _push_colors(force=True)
    _push_text()
    _push_transport(force=True)


# --- control mode ------------------------------------------------------------

def _set_mode(mode):
    global _mode
    mode = MODE_CONTROL if mode == MODE_CONTROL else MODE_PADS
    if mode == _mode:
        return
    _mode = mode
    _push_colors(force=True)
    _push_text()


def _snap_tempo(raw):
    """Whole BPM, pulled onto multiples of TEMPO_SNAP_STEP when close, clamped to FL's range."""
    raw = max(TEMPO_MIN, min(TEMPO_MAX, raw))
    step = round(raw / TEMPO_SNAP_STEP) * TEMPO_SNAP_STEP
    if abs(raw - step) <= TEMPO_SNAP_PULL:
        return float(step)
    return float(round(raw))


def _set_tempo(bpm):
    flags = midi.REC_Control | midi.REC_UpdateControl
    general.processRECEvent(midi.REC_Tempo, int(round(bpm * 1000)), flags)


def _control_tilt(pad, value):
    """The board leaned while `pad` is held: on the metronome pad that is the tempo."""
    global _metro_hold
    if pad != CTRL_METRONOME or _metro_hold is None:
        return
    offset = value - TILT_CENTRE
    if abs(offset) > TILT_DEAD_ZONE:
        _metro_hold["moved"] = True
    if not _metro_hold["moved"]:
        return
    bpm = _snap_tempo(_metro_hold["tempo0"] + offset / 63.0 * TEMPO_TILT_SPAN)
    if abs(bpm - mixer.getCurrentTempo() / 1000.0) >= 0.5:
        _set_tempo(bpm)
        _push_text()


def _control_release(pad):
    """The metronome pad toggles on release, and only when the hold was not a tempo gesture."""
    global _metro_hold
    if pad != CTRL_METRONOME or _metro_hold is None:
        return
    moved = _metro_hold["moved"]
    _metro_hold = None
    if not moved:
        transport.globalTransport(FPT_METRONOME, 1)
        _push_colors()


def _control_press(pad):
    """One control-layout pad went down. Buttons are momentary; the metronome pad acts on release."""
    global _metro_hold
    if pad == CTRL_METRONOME:
        _metro_hold = {"tempo0": mixer.getCurrentTempo() / 1000.0, "moved": False}
        return
    if pad == CTRL_PLAY:
        transport.start()
    elif pad == CTRL_STOP:
        transport.stop()
    elif pad == CTRL_RECORD:
        transport.record()
    elif pad == CTRL_LOOP_MODE:
        transport.setLoopMode()
    elif pad == CTRL_PATTERN_PREV:
        patterns.jumpToPattern(max(1, patterns.patternNumber() - 1))
    elif pad == CTRL_PATTERN_NEXT:
        patterns.jumpToPattern(min(patterns.patternCount(), patterns.patternNumber() + 1))
    elif pad == CTRL_UNDO:
        general.undo()
    elif CTRL_MUTE_FIRST <= pad < CTRL_MUTE_FIRST + CTRL_MUTE_COUNT:
        chan = pad - CTRL_MUTE_FIRST
        if chan < channels.channelCount():
            channels.muteChannel(chan)
    _push_colors()
    _push_transport()


# --- FL callbacks ------------------------------------------------------------

def OnInit():
    global _connected
    _connected = False
    _send(SUB_HELLO, [DAW_ID_FL_STUDIO, PROTO_VER])
    print("CrossPad: hello sent, waiting for the DAW Control app")


def OnDeInit():
    _send(SUB_BYE)


def OnSysEx(event):
    """Device hello (sync everything), its `mode` (repaint), a control-mode `pad` (act)."""
    global _connected
    data = event.sysex
    if len(data) < 5 or data[1] != MANUFACTURER or data[2] != CMD_DAW:
        return
    sub = data[3]
    if sub == SUB_MODE:
        _set_mode(data[4])
        event.handled = True
    elif sub == SUB_PAD:
        if _connected and len(data) >= 6:
            if data[5] == 1:
                _control_press(data[4])
            else:
                _control_release(data[4])
        event.handled = True
    elif sub == SUB_TILT:
        if _connected and len(data) >= 6:
            _control_tilt(data[4], data[5])
        event.handled = True
    elif sub == SUB_HELLO and len(data) >= 8:
        pads = data[5]
        _connected = pads >= PAD_COUNT
        if _connected:
            print("CrossPad: DAW Control app connected (proto %d, %d pads)" % (data[4], pads))
            # Answer every device hello: the app may have started, or the board
            # rebooted, long after OnInit's hello. The device replies with its
            # own hello only the first time, so this cannot ping-pong.
            _send(SUB_HELLO, [DAW_ID_FL_STUDIO, PROTO_VER])
            _push_all()
        event.handled = True


def OnRefresh(flags):
    if not _connected:
        return
    _push_colors()
    _push_text()
    _push_transport()


def OnIdle():
    # Tempo and pattern edits do not always raise a refresh flag; the row
    # cache makes this cheap when nothing changed. In control mode the LEDs
    # follow the mixer meters and mutes the same way, through the colour cache.
    global _last_ping, _metro_flash_until
    if _connected:
        now = time.monotonic()
        _push_text()
        if _mode == MODE_CONTROL:
            if _metro_flash_until and now >= _metro_flash_until:
                _metro_flash_until = 0.0
                _push_colors(force=True)
            elif not _metro_flash_until:
                _push_colors()
        if now - _last_ping >= PING_PERIOD_S:
            _last_ping = now
            _send(SUB_PING)


def OnUpdateBeatIndicator(value):
    if not _connected:
        return
    _send(SUB_BEAT, [value])
    if _mode != MODE_CONTROL or _last_colors is None:
        return
    # The metronome pad blinks with the beat: bar and beat in the screen's
    # dot colours for BEAT_FLASH_S, then OnIdle repaints. Colour passes pause
    # meanwhile so the meters do not overwrite the flash.
    global _metro_flash_until
    if value:
        flash = _rgb7(BEAT_BAR_COLOR if value == 1 else BEAT_BEAT_COLOR)
        _last_colors[CTRL_METRONOME] = flash
        _send(SUB_LED, [CTRL_METRONOME] + list(flash))
        _metro_flash_until = time.monotonic() + BEAT_FLASH_S


def OnMidiMsg(event):
    """Notes pass through to FL (the selected channel / FPC). The encoder drives the channel rack."""
    global _last_encoder
    status = event.status & 0xF0
    if status != midi.MIDI_CONTROLCHANGE:
        return
    if event.data1 == CC_ENCODER:
        if _last_encoder is not None:
            delta = (event.data2 - _last_encoder + ENCODER_DETENTS // 2) % ENCODER_DETENTS - ENCODER_DETENTS // 2
            if delta:
                count = channels.channelCount()
                if count > 0:
                    channels.selectOneChannel((channels.selectedChannel() + delta) % count)
        _last_encoder = event.data2
        event.handled = True
    elif event.data1 == CC_ENCODER_BUTTON:
        if event.data2 > 0:
            transport.start()
        event.handled = True


# --- optional bench bridge ---------------------------------------------------
#
# A `flbridge.py` beside this file (the fl-studio-mcp FL Bridge) rides on the
# same controller entry: FL allows one script per port, and under Wine one open
# MIDI input, so the bridge cannot be its own controller. It sees only what
# this script left unhandled — its SysEx is another manufacturer's. Without
# the file nothing changes.

try:
    import flbridge as _bridge
    print("CrossPad: FL Bridge module loaded")
except Exception as _e:   # noqa: BLE001 — FL's script host dies on a raised import error
    _bridge = None
    print("CrossPad: FL Bridge not loaded: " + repr(_e))

if _bridge is not None:
    _crosspad_OnInit = OnInit
    _crosspad_OnDeInit = OnDeInit
    _crosspad_OnIdle = OnIdle
    _crosspad_OnSysEx = OnSysEx
    _crosspad_OnMidiMsg = OnMidiMsg

    def OnInit():
        _crosspad_OnInit()
        try:
            _bridge.OnInit()
        except Exception as e:
            print("bridge OnInit failed: %r" % (e,))

    def OnDeInit():
        try:
            _bridge.OnDeInit()
        except Exception:
            pass
        _crosspad_OnDeInit()

    def OnIdle():
        _crosspad_OnIdle()
        _bridge.OnIdle()

    def OnSysEx(event):
        _crosspad_OnSysEx(event)
        if not event.handled:
            _bridge.OnSysEx(event)

    def OnMidiMsg(event):
        _crosspad_OnMidiMsg(event)
        if not event.handled:
            _bridge.OnMidiMsg(event)
