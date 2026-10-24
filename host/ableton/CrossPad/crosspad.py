"""
Ableton Live control surface for CrossPad running the DAW Control app.

The device side is generic: it plays notes and shows what it is told. This
script is the Live half of it, the counterpart of the FL Studio and REAPER
scripts beside it — the selected track's colour (a Drum Rack's pad colours
when the track holds one) and name, the highlighted clip or the selected
scene, tempo, transport and the beat go to the pads and LCD rows. Frames
follow the `daw` section of crosspad-hil `knowledge/sysex.yaml`; keep the
constants below in step with it.

Keyboard layout: the pads play the selected track. Live sends a controller's
notes only to armed tracks, so the script arms the selected one implicitly —
the way Live's own pad controllers do, without touching its arm button — and
lets go of it when the selection moves or the script stops. The notes are
never seen here: Live routes them to the track (the input's "Track" switch).

Control layout (double tap on the case): play/stop, stop, record, loop,
previous/next track, metronome (tempo from where the finger sits on it),
undo, and mutes for tracks 1-8 whose LEDs glow as loud as the track plays
right now.

This uses the plain control-surface interface Live calls on every script
(no _Framework components): Live calls update_display() every 100 ms, which
is the clock for everything here except the beat, which follows Live's song
time listener.
"""

import time

import Live

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
SUB_PAN = 0x0B             # device -> host: where on the pad the press sits, 64 = middle
SUB_BYE = 0x7F

PING_PERIOD_S = 1.0        # the board forgets a host silent for 5 s
PAN_CENTRE = 64

MODE_PADS = 0
MODE_CONTROL = 3

DAW_ID_ABLETON = 2
PROTO_VER = 1
PAD_COUNT = 16
TEXT_COLS = 24

# What the pads play: a Drum Rack's first sixteen pads, C1 upwards, on channel 10.
BASE_NOTE = 36
MIDI_CHANNEL = 10          # 1-based

# The firmware emits the navigation encoder as CC 1 (absolute detent 0..15,
# wrapping) and its switch as CC 64 on channel 1, independent of the app.
# Both are forwarded to this script, so they never reach the instrument.
CC_ENCODER = 1
CC_ENCODER_BUTTON = 64
CC_CHANNEL = 0             # 0-based: channel 1
ENCODER_DETENTS = 16

DRUM_RACK_PADS = 128       # RackDevice.drum_pads: one per note, index = note

# --- control layout: pad index = row * 4 + column, row 0 at the top ----------

CTRL_PLAY, CTRL_STOP, CTRL_RECORD, CTRL_LOOP = 0, 1, 2, 3
CTRL_TRACK_PREV, CTRL_TRACK_NEXT, CTRL_METRONOME, CTRL_UNDO = 4, 5, 6, 7
CTRL_MUTE_FIRST = 8        # pads 8..15 mute tracks 1..8
CTRL_MUTE_COUNT = 8

# Control-mode LED colours as 0xRRGGBB: (off/idle, on/active).
CTRL_COLORS = {
    CTRL_PLAY: (0x104020, 0x30FF60),
    CTRL_STOP: (0x401818, 0xFF4040),
    CTRL_RECORD: (0x401010, 0xFF2020),
    CTRL_LOOP: (0x403010, 0xFFC020),
    CTRL_TRACK_PREV: (0x2060FF, 0x2060FF),
    CTRL_TRACK_NEXT: (0x2060FF, 0x2060FF),
    CTRL_METRONOME: (0x303030, 0xE0E0E0),
    CTRL_UNDO: (0x8040FF, 0x8040FF),
}
PEAK_STEPS = 8             # brightness is quantised so the LEDs update on level changes, not every tick
BEAT_BAR_COLOR = 0xFFB347  # metronome pad on the bar, the screen's beat dot colours
BEAT_BEAT_COLOR = 0xE0E0FF
BEAT_FLASH_S = 0.12
MUTED_BRIGHTNESS = 0.06    # a muted track or drum pad keeps its hue, barely lit
MIN_LIT_BRIGHTNESS = 0.12  # a silent, unmuted track is still visibly there
LED_SINGLE_MAX = 4         # up to this many changed pads go out as `led`, more as one `led_all`

# Tempo from the metronome pad: same feel as the FL script — see its comments.
TEMPO_RATE_MAX = 10.0
TEMPO_SNAP_STEP = 5
PAN_FULL = 45
TEMPO_ACCEL_AFTER_S = 0.8
TEMPO_ACCEL_RAMP_S = 1.5
TEMPO_ACCEL_MAX = 4.0
TEMPO_ACCEL_LEAN = 0.9
TEMPO_MIN, TEMPO_MAX = 20.0, 999.0   # Live's own tempo range
PAN_DEAD_ZONE = 20
PAN_SETTLE_S = 0.1


# --- small helpers -----------------------------------------------------------

def _clamp(v, lo, hi):
    return max(lo, min(hi, v))


def _rgb7(color):
    """Live colour int (0xRRGGBB) -> 3 x 7-bit r g b."""
    c = color & 0xFFFFFF
    return ((c >> 17) & 0x7F, (c >> 9) & 0x7F, (c >> 1) & 0x7F)


def _scale(color, brightness):
    r, g, b = _rgb7(color)
    k = _clamp(brightness, 0.0, 1.0)
    return (int(r * k), int(g * k), int(b * k))


def _drum_rack(track):
    for device in track.devices:
        if device.can_have_drum_pads:
            return device
    return None


def _drum_pad(rack, note):
    pads = rack.drum_pads
    if len(pads) == DRUM_RACK_PADS and pads[note].note == note:
        return pads[note]
    for pad in pads:
        if pad.note == note:
            return pad
    return None


def _peak(track):
    if not track.has_audio_output:
        return 0.0
    return _clamp(track.output_meter_level, 0.0, 1.0)


class CrossPad(object):

    def __init__(self, c_instance):
        self._c = c_instance
        self._song = c_instance.song()
        self._connected = False
        self._mode = MODE_PADS
        self._last_rows = ["", "", ""]
        self._last_colors = None
        self._last_transport = None
        self._last_encoder = None
        self._last_ping = 0.0
        self._last_beat = None
        self._metro_flash_until = 0.0
        self._metro_hold = None
        self._armed = None          # the track this script armed implicitly
        # Live builds the script before its ports are up: the first hello goes
        # out from the first update_display(). The board hellos every 2 s
        # while no host answers anyway, so a lost one only costs that long.
        self._hello_due = True
        self._song.add_current_song_time_listener(self._on_song_time)

    # --- the interface Live calls ---------------------------------------------

    def disconnect(self):
        if self._song.current_song_time_has_listener(self._on_song_time):
            self._song.remove_current_song_time_listener(self._on_song_time)
        self._release_armed()
        self._send(SUB_BYE)

    def connect_script_instances(self, instances):
        pass

    def application(self):
        return Live.Application.get_application()

    def song(self):
        return self._song

    def suggest_input_port(self):
        return "Crosspad"

    def suggest_output_port(self):
        return "Crosspad"

    def suggest_map_mode(self, cc_no, channel):
        return Live.MidiMap.MapMode.absolute

    def suggest_needs_takeover(self, cc_no, channel):
        return False

    def can_lock_to_devices(self):
        return False

    def lock_to_device(self, device):
        pass

    def unlock_from_device(self, device):
        pass

    def toggle_lock(self):
        pass

    def restore_bank(self, bank):
        pass

    def port_settings_changed(self):
        self._hello_due = True

    def request_rebuild_midi_map(self):
        self._c.request_rebuild_midi_map()

    def refresh_state(self):
        self._hello_due = True

    def build_midi_map(self, midi_map_handle):
        handle = self._c.handle()
        Live.MidiMap.forward_midi_cc(handle, midi_map_handle, CC_CHANNEL, CC_ENCODER)
        Live.MidiMap.forward_midi_cc(handle, midi_map_handle, CC_CHANNEL, CC_ENCODER_BUTTON)

    def receive_midi(self, midi_bytes):
        if midi_bytes and midi_bytes[0] == 0xF0:
            self._on_sysex(midi_bytes)
        else:
            self._on_short(midi_bytes)

    def update_display(self):
        if self._hello_due:
            self._say_hello()
        if not self._connected:
            return
        now = time.monotonic()
        self._follow_selection()
        self._run_tempo(now)
        self._push_text()
        self._push_transport()
        if self._mode == MODE_CONTROL:
            if self._metro_flash_until and now >= self._metro_flash_until:
                self._metro_flash_until = 0.0
                self._push_colors(force=True)
            elif not self._metro_flash_until:
                self._push_colors()
        else:
            self._push_colors()
        if now - self._last_ping >= PING_PERIOD_S:
            self._last_ping = now
            self._send(SUB_PING)

    # --- frame helpers ---------------------------------------------------------

    def _send(self, sub, payload=()):
        frame = (0xF0, MANUFACTURER, CMD_DAW, sub) + tuple(b & 0x7F for b in payload) + (0xF7,)
        self._c.send_midi(frame)

    def _text(self, row, text):
        s = "".join(ch if 0x20 <= ord(ch) <= 0x7E else " " for ch in str(text))[:TEXT_COLS]
        if self._last_rows[row] == s:
            return
        self._last_rows[row] = s
        self._send(SUB_TEXT, [row] + [ord(ch) for ch in s])

    def _say_hello(self):
        self._hello_due = False
        self._connected = False
        self._send(SUB_HELLO, [DAW_ID_ABLETON, PROTO_VER])

    # --- what the DAW shows ----------------------------------------------------

    def _selected_track(self):
        return self._song.view.selected_track

    def _pad_colors(self):
        """16 colours: a Drum Rack's pad colours when the selected track holds one, else the track colour."""
        track = self._selected_track()
        if track is None:
            return [(0, 0, 0)] * PAD_COUNT
        rack = _drum_rack(track)
        if rack is None:
            return [_rgb7(track.color)] * PAD_COUNT
        colors = []
        for pad_idx in range(PAD_COUNT):
            pad = _drum_pad(rack, BASE_NOTE + pad_idx)
            if pad is None or not pad.chains:
                colors.append((0, 0, 0))   # an empty pad plays nothing: it stays dark
                continue
            color = getattr(pad.chains[0], "color", track.color)
            colors.append(_scale(color, MUTED_BRIGHTNESS) if pad.mute else _rgb7(color))
        return colors

    def _mute_pad_color(self, index):
        tracks = self._song.tracks
        if index >= len(tracks):
            return (0, 0, 0)
        track = tracks[index]
        if track.mute:
            return _scale(track.color, MUTED_BRIGHTNESS)
        level = round(_peak(track) * PEAK_STEPS) / PEAK_STEPS
        return _scale(track.color, max(MIN_LIT_BRIGHTNESS, level))

    def _control_colors(self):
        """16 colours for the control layout: state-lit buttons, then the eight mutes."""
        song = self._song
        active = {
            CTRL_PLAY: song.is_playing,
            CTRL_STOP: not song.is_playing,
            CTRL_RECORD: song.record_mode,
            CTRL_LOOP: song.loop,
            CTRL_TRACK_PREV: True,
            CTRL_TRACK_NEXT: True,
            CTRL_METRONOME: song.metronome,
            CTRL_UNDO: True,
        }
        colors = [_rgb7(CTRL_COLORS[pad][1 if active[pad] else 0]) for pad in range(CTRL_MUTE_FIRST)]
        for i in range(CTRL_MUTE_COUNT):
            colors.append(self._mute_pad_color(i))
        return colors

    def _push_colors(self, force=False):
        colors = self._control_colors() if self._mode == MODE_CONTROL else self._pad_colors()
        if not force and colors == self._last_colors:
            return
        if not force and self._last_colors is not None:
            changed = [pad for pad in range(PAD_COUNT) if colors[pad] != self._last_colors[pad]]
            if len(changed) <= LED_SINGLE_MAX:
                # Meters move one or two pads at a time; a full repaint per
                # tick would be 48 bytes of SysEx for every flicker.
                for pad in changed:
                    r, g, b = colors[pad]
                    self._send(SUB_LED, [pad, r, g, b])
                self._last_colors = colors
                return
        self._last_colors = colors
        payload = []
        for r, g, b in colors:
            payload += [r, g, b]
        self._send(SUB_LED_ALL, payload)

    def _place_name(self):
        """The highlighted clip on the selected track, else the selected scene."""
        view = self._song.view
        slot = view.highlighted_clip_slot
        if slot is not None and slot.has_clip and slot.clip.name:
            return slot.clip.name
        scene = view.selected_scene
        if scene.name:
            return scene.name
        return "Scene %d" % (list(self._song.scenes).index(scene) + 1)

    def _push_text(self):
        track = self._selected_track()
        self._text(0, track.name if track is not None else "-")
        self._text(1, self._place_name())
        self._text(2, "%.1f BPM" % self._song.tempo)

    def _push_transport(self, force=False):
        song = self._song
        bits = 0
        if song.is_playing:
            bits |= 1
        if song.record_mode or song.session_record:
            bits |= 2
        if song.loop:
            bits |= 4
        if force or bits != self._last_transport:
            self._last_transport = bits
            self._send(SUB_TRANSPORT, [bits])

    def _push_all(self):
        self._last_rows = ["", "", ""]
        self._last_colors = None
        self._last_transport = None
        self._send(SUB_CONFIG, [BASE_NOTE, MIDI_CHANNEL - 1])
        # The layout is the board's: it answers every hello with its `mode`,
        # and _set_mode repaints for it. Until then paint for the mode last known.
        self._push_colors(force=True)
        self._push_text()
        self._push_transport(force=True)

    # --- the pads play the selected track ----------------------------------------

    def _release_armed(self):
        track, self._armed = self._armed, None
        if track is None:
            return
        try:
            track.implicit_arm = False
        except RuntimeError:   # the track was deleted meanwhile: nothing left to give back
            pass

    def _follow_selection(self):
        track = self._selected_track()
        if track is not None and track == self._armed:
            return
        self._release_armed()
        if track is None or not track.can_be_armed or not track.has_midi_input:
            return
        track.implicit_arm = True
        self._armed = track

    def _select_track_by(self, delta):
        tracks = list(self._song.visible_tracks)
        if not tracks:
            return
        current = self._selected_track()
        idx = tracks.index(current) if current in tracks else -1
        self._song.view.selected_track = tracks[(idx + delta) % len(tracks)]

    # --- control mode ------------------------------------------------------------

    def _set_mode(self, mode):
        mode = MODE_CONTROL if mode == MODE_CONTROL else MODE_PADS
        if mode == self._mode:
            return
        self._mode = mode
        self._push_colors(force=True)
        self._push_text()

    def _set_tempo(self, bpm):
        self._song.tempo = _clamp(float(bpm), TEMPO_MIN, TEMPO_MAX)

    def _control_pan(self, pad, value):
        """The press on `pad` sits at `value` (1 left .. 127 right): on the metronome pad that sets the tempo's rate."""
        h = self._metro_hold
        if pad != CTRL_METRONOME or h is None:
            return
        if time.monotonic() - h["t0"] < PAN_SETTLE_S:
            return
        offset = value - PAN_CENTRE
        if abs(offset) <= PAN_DEAD_ZONE:
            h["rate"] = 0.0
            h["lean"] = 0.0
            return
        h["moved"] = True
        lean = min(1.0, (abs(offset) - PAN_DEAD_ZONE) / float(PAN_FULL - PAN_DEAD_ZONE))
        if h["lean"] < TEMPO_ACCEL_LEAN <= lean:
            h["last"] = time.monotonic()   # the edge time starts now, not at the press
        h["lean"] = lean
        h["rate"] = (1.0 if offset > 0 else -1.0) * lean * TEMPO_RATE_MAX

    def _run_tempo(self, now):
        """update_display: move the tempo at the held rate, faster the longer the edge is held."""
        h = self._metro_hold
        if h is None:
            return
        dt = now - h["last"]
        h["last"] = now
        if h["lean"] >= TEMPO_ACCEL_LEAN:
            h["edge_s"] += dt
            if h["edge_s"] > TEMPO_ACCEL_AFTER_S:
                ramp = min(1.0, (h["edge_s"] - TEMPO_ACCEL_AFTER_S) / TEMPO_ACCEL_RAMP_S)
                h["accel"] = 1.0 + ramp * (TEMPO_ACCEL_MAX - 1.0)
        else:
            h["edge_s"] = 0.0
            h["accel"] = 1.0
        if h["rate"] == 0.0:
            return
        before = h["tempo"]
        after = _clamp(before + h["rate"] * h["accel"] * dt, TEMPO_MIN, TEMPO_MAX)
        h["tempo"] = after
        if round(after) != round(before):
            self._set_tempo(round(after))
            self._push_text()

    def _control_release(self, pad):
        """The metronome pad toggles on release, and only when the press stayed in the middle."""
        h = self._metro_hold
        if pad != CTRL_METRONOME or h is None:
            return
        self._metro_hold = None
        if h["moved"]:
            # A long run at the edge was aiming for a round number; a short,
            # careful one for exactly where it stopped.
            if h["edge_s"] > TEMPO_ACCEL_AFTER_S:
                landed = round(h["tempo"] / TEMPO_SNAP_STEP) * TEMPO_SNAP_STEP
            else:
                landed = round(h["tempo"])
            self._set_tempo(landed)
            self._push_text()
        else:
            self._song.metronome = not self._song.metronome
            self._push_colors()

    def _control_press(self, pad):
        """One control-layout pad went down. Buttons are momentary; the metronome pad acts on release."""
        song = self._song
        if pad == CTRL_METRONOME:
            now = time.monotonic()
            self._metro_hold = {"tempo": song.tempo, "rate": 0.0, "lean": 0.0, "moved": False,
                                "t0": now, "last": now, "edge_s": 0.0, "accel": 1.0}
            return
        if pad == CTRL_PLAY:
            self._toggle_play()
        elif pad == CTRL_STOP:
            song.stop_playing()
        elif pad == CTRL_RECORD:
            song.record_mode = not song.record_mode
        elif pad == CTRL_LOOP:
            song.loop = not song.loop
        elif pad == CTRL_TRACK_PREV:
            self._select_track_by(-1)
        elif pad == CTRL_TRACK_NEXT:
            self._select_track_by(1)
        elif pad == CTRL_UNDO:
            if song.can_undo:
                song.undo()
        elif CTRL_MUTE_FIRST <= pad < CTRL_MUTE_FIRST + CTRL_MUTE_COUNT:
            tracks = song.tracks
            index = pad - CTRL_MUTE_FIRST
            if index < len(tracks):
                tracks[index].mute = not tracks[index].mute
        self._push_colors()
        self._push_transport()

    def _toggle_play(self):
        # Live has no pause: stop keeps the position and continue plays on from it.
        if self._song.is_playing:
            self._song.stop_playing()
        else:
            self._song.continue_playing()

    # --- the beat ------------------------------------------------------------------

    def _on_song_time(self):
        if not self._connected:
            return
        if not self._song.is_playing:
            self._last_beat = None
            return
        t = self._song.get_current_beats_song_time()
        key = (t.bars, t.beats)
        if key == self._last_beat:
            return
        self._last_beat = key
        value = 1 if t.beats == 1 else 2
        self._send(SUB_BEAT, [value])
        if self._mode != MODE_CONTROL or self._last_colors is None:
            return
        # The metronome pad blinks with the beat in the screen's dot colours
        # for BEAT_FLASH_S; colour passes pause meanwhile so the meters do not
        # overwrite the flash.
        flash = _rgb7(BEAT_BAR_COLOR if value == 1 else BEAT_BEAT_COLOR)
        self._last_colors[CTRL_METRONOME] = flash
        self._send(SUB_LED, [CTRL_METRONOME] + list(flash))
        self._metro_flash_until = time.monotonic() + BEAT_FLASH_S

    # --- what the board says -------------------------------------------------------

    def _on_sysex(self, data):
        """Device hello (sync everything), its `mode` (repaint), a control-mode `pad` or `pan` (act)."""
        if len(data) < 5 or data[1] != MANUFACTURER or data[2] != CMD_DAW:
            return
        sub = data[3]
        if sub == SUB_MODE:
            self._set_mode(data[4])
        elif sub == SUB_PAD and len(data) >= 7:
            if self._connected:
                if data[5] == 1:
                    self._control_press(data[4])
                else:
                    self._control_release(data[4])
        elif sub == SUB_PAN and len(data) >= 7:
            if self._connected:
                self._control_pan(data[4], data[5])
        elif sub == SUB_HELLO and len(data) >= 9:
            # Device hello: proto pads rows caps.
            pads = data[5]
            self._connected = pads >= PAD_COUNT
            if self._connected:
                self._c.log_message("CrossPad: DAW Control app connected (proto %d, %d pads)"
                                    % (data[4], pads))
                # Answer every device hello: the app may have started, or the
                # board rebooted, long after ours. The device replies with its
                # own hello only the first time, so this cannot ping-pong.
                self._send(SUB_HELLO, [DAW_ID_ABLETON, PROTO_VER])
                self._push_all()
            else:
                self._release_armed()

    def _on_short(self, data):
        """The encoder: CC 1 detents select tracks, CC 64 toggles play."""
        if len(data) < 3 or data[0] & 0xF0 != 0xB0:
            return
        if data[1] == CC_ENCODER:
            if self._last_encoder is not None:
                half = ENCODER_DETENTS // 2
                delta = (data[2] - self._last_encoder + half) % ENCODER_DETENTS - half
                if delta:
                    self._select_track_by(delta)
            self._last_encoder = data[2]
        elif data[1] == CC_ENCODER_BUTTON and data[2] > 0:
            self._toggle_play()
