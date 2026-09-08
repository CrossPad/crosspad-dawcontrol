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

This file must stay self-contained: FL's embedded Python has no pip.
"""

import channels
import device
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
SUB_BYE = 0x7F

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

# --- state -------------------------------------------------------------------

_last_rows = ["", "", ""]
_last_colors = None
_last_transport = None
_last_encoder = None
_connected = False


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


def _push_colors(force=False):
    global _last_colors
    colors = _pad_colors()
    if not force and colors == _last_colors:
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
    _send(SUB_MODE, [0])
    _push_colors(force=True)
    _push_text()
    _push_transport(force=True)


# --- FL callbacks ------------------------------------------------------------

def OnInit():
    global _connected
    _connected = False
    _send(SUB_HELLO, [DAW_ID_FL_STUDIO, PROTO_VER])
    print("CrossPad: hello sent, waiting for the DAW Control app")


def OnDeInit():
    _send(SUB_BYE)


def OnSysEx(event):
    """Device hello: the app just started (or answered ours) — sync everything."""
    global _connected
    data = event.sysex
    if len(data) >= 8 and data[1] == MANUFACTURER and data[2] == CMD_DAW and data[3] == SUB_HELLO:
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
    # cache makes this cheap when nothing changed.
    if _connected:
        _push_text()


def OnUpdateBeatIndicator(value):
    if _connected:
        _send(SUB_BEAT, [value])


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
