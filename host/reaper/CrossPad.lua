--[[
REAPER control script for CrossPad running the DAW Control app.

Install: `crosspad-hil reaper install` (with REAPER closed) copies this file to
<REAPER resource path>/Scripts/CrossPad/, starts it from __startup.lua and
enables the CrossPad MIDI device for input and output. By hand: put it there,
Actions -> Load ReaScript, run it; Preferences -> MIDI Devices -> enable the
device for input and output.

Which device: REAPER on Linux opens raw MIDI only (ALSA raw or JACK, never the
sequencer), and the board's own raw port belongs to the crosspad-hil MIDI hub.
The hub bridges a virtual raw card (snd-virmidi, "VirMIDI") for it; this script
talks to that one, or to a port named CrossPad where REAPER can open the board
directly (Windows, macOS).

The device side is generic: it plays notes and shows what it is told. This
script is the REAPER half of it, the counterpart of the FL Studio script
beside it — the selected track's colour and name, the region or marker under
the play cursor, tempo, transport and the beat go to the pads and LCD rows.
Frames follow the `daw` section of crosspad-hil `knowledge/sysex.yaml`; keep
the constants below in step with it.

Keyboard layout: the pads play the selected track. REAPER sends MIDI only to
armed, monitored tracks, so the script arms the selected track with the
CrossPad input on the pads' channel (the encoder's CCs on channel 1 stay out
of the instrument) and gives the previous one back exactly as it found it.

Control layout (double tap on the case): transport, repeat, previous/next
track, metronome (tempo from where the finger sits on it), undo, and mutes for
tracks 1-8 whose LEDs glow as loud as the track plays right now.

REAPER's input history (MIDI_GetRecentInputEvent) is polled from a deferred
loop: this file has no callbacks to hang on.
]]

-- --- protocol constants (mirror of sysex.yaml `daw`) ------------------------

local MANUFACTURER = 0x7D
local CMD_DAW = 0x1E
local SUB_HELLO = 0x01
local SUB_LED = 0x02
local SUB_LED_ALL = 0x03
local SUB_TEXT = 0x04
local SUB_BEAT = 0x05
local SUB_TRANSPORT = 0x06
local SUB_MODE = 0x07
local SUB_CONFIG = 0x08
local SUB_PAD = 0x09          -- device -> host: pad state, control mode only
local SUB_PING = 0x0A         -- host -> device: still here, once a second
local SUB_PAN = 0x0B          -- device -> host: where on the pad the press sits, 64 = middle
local SUB_BYE = 0x7F

local PING_PERIOD_S = 1.0     -- the board forgets a host silent for 5 s
local PAN_CENTRE = 64

local MODE_PADS = 0
local MODE_CONTROL = 3

local DAW_ID_REAPER = 3
local PROTO_VER = 1
local PAD_COUNT = 16
local TEXT_COLS = 24

-- What the pads play: C1 upwards on channel 10, as the FL script.
local BASE_NOTE = 36
local MIDI_CHANNEL = 10       -- 1-based

-- The firmware emits the navigation encoder as CC 1 (absolute detent 0..15,
-- wrapping) and its switch as CC 64 on channel 1, independent of the app.
local CC_ENCODER = 1
local CC_ENCODER_BUTTON = 64
local ENCODER_DETENTS = 16

-- Which REAPER MIDI device is the board, in order of preference.
local DEVICE_MATCHES = { "virmidi", "crosspad" }
local NO_DEVICE_RETRY_S = 2.0
local DEVIDX_MASK = 0xFFFF    -- MIDI_GetRecentInputEvent's devIdx carries flags above the index

-- --- control layout: pad index = row * 4 + column, row 0 at the top ---------

local CTRL_PLAY, CTRL_STOP, CTRL_RECORD, CTRL_REPEAT = 0, 1, 2, 3
local CTRL_TRACK_PREV, CTRL_TRACK_NEXT, CTRL_METRONOME, CTRL_UNDO = 4, 5, 6, 7
local CTRL_MUTE_FIRST = 8     -- pads 8..15 mute tracks 1..8
local CTRL_MUTE_COUNT = 8

local CMD_RECORD = 1013       -- Transport: Record
local CMD_METRONOME = 40364   -- Options: Toggle metronome
local PLAYSTATE_PLAYING = 1
local PLAYSTATE_RECORDING = 4

-- Control-mode LED colours as 0xRRGGBB: { off/idle, on/active }.
local CTRL_COLORS = {
  [CTRL_PLAY] = { 0x104020, 0x30FF60 },
  [CTRL_STOP] = { 0x401818, 0xFF4040 },
  [CTRL_RECORD] = { 0x401010, 0xFF2020 },
  [CTRL_REPEAT] = { 0x403010, 0xFFC020 },
  [CTRL_TRACK_PREV] = { 0x2060FF, 0x2060FF },
  [CTRL_TRACK_NEXT] = { 0x2060FF, 0x2060FF },
  [CTRL_METRONOME] = { 0x303030, 0xE0E0E0 },
  [CTRL_UNDO] = { 0x8040FF, 0x8040FF },
}
local DEFAULT_TRACK_COLOR = 0x6080A0  -- a track without a colour of its own
local PEAK_STEPS = 8          -- brightness is quantised so the LEDs update on level changes, not every tick
local BEAT_BAR_COLOR = 0xFFB347
local BEAT_BEAT_COLOR = 0xE0E0FF
local BEAT_FLASH_S = 0.12
local MUTED_BRIGHTNESS = 0.06
local MIN_LIT_BRIGHTNESS = 0.12
local LED_SINGLE_MAX = 4      -- up to this many changed pads go out as `led`, more as one `led_all`

-- Tempo from the metronome pad: same feel as the FL script — see its comments.
local TEMPO_RATE_MAX = 10.0
local TEMPO_SNAP_STEP = 5
local PAN_FULL = 45
local TEMPO_ACCEL_AFTER_S = 0.8
local TEMPO_ACCEL_RAMP_S = 1.5
local TEMPO_ACCEL_MAX = 4.0
local TEMPO_ACCEL_LEAN = 0.9
local TEMPO_MIN, TEMPO_MAX = 20.0, 960.0
local PAN_DEAD_ZONE = 20
local PAN_SETTLE_S = 0.1

-- I_RECINPUT for a MIDI device: 4096 | device << 5 | channel (0 = all).
local RECINPUT_MIDI = 4096
local RECINPUT_DEV_SHIFT = 5

-- --- state -----------------------------------------------------------------

local in_dev, out_dev = nil, nil
local next_device_try = 0.0
local connected = false
local mode = MODE_PADS
local last_rows = { "", "", "" }
local last_colors = nil
local last_transport = nil
local last_encoder = nil
local last_ping = 0.0
local last_event_seq = nil
local last_beat = nil
local metro_flash_until = 0.0
local metro_hold = nil
local armed = nil             -- { track, guid, arm, mon, input } as the track was before we armed it
local told_no_device = false

-- --- small helpers ---------------------------------------------------------

local function now() return reaper.time_precise() end

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

local function round(v) return math.floor(v + 0.5) end

local function rgb7(color)
  return { (color >> 17) & 0x7F, (color >> 9) & 0x7F, (color >> 1) & 0x7F }
end

local function scale(color, brightness)
  local c = rgb7(color)
  local k = clamp(brightness, 0.0, 1.0)
  return { math.floor(c[1] * k), math.floor(c[2] * k), math.floor(c[3] * k) }
end

local function same_color(a, b) return a[1] == b[1] and a[2] == b[2] and a[3] == b[3] end

local function log(msg) reaper.ShowConsoleMsg("CrossPad: " .. msg .. "\n") end

-- --- device ------------------------------------------------------------------

local function find_device(count_fn, name_fn)
  for _, match in ipairs(DEVICE_MATCHES) do
    for i = 0, count_fn() - 1 do
      local present, name = name_fn(i, "")
      if present and name:lower():find(match, 1, true) then return i end
    end
  end
  return nil
end

local function open_device()
  in_dev = find_device(reaper.GetNumMIDIInputs, reaper.GetMIDIInputName)
  out_dev = find_device(reaper.GetNumMIDIOutputs, reaper.GetMIDIOutputName)
  if in_dev and out_dev then
    told_no_device = false
    return true
  end
  if not told_no_device then
    told_no_device = true
    log("no CrossPad MIDI device enabled for input and output (Preferences -> MIDI Devices); "
        .. "on Linux that is VirMIDI, bridged by the crosspad-hil MIDI hub")
  end
  return false
end

-- --- frame helpers -----------------------------------------------------------

local function send(sub, payload)
  if not out_dev then return end
  local bytes = { 0xF0, MANUFACTURER, CMD_DAW, sub }
  for _, b in ipairs(payload or {}) do bytes[#bytes + 1] = b & 0x7F end
  bytes[#bytes + 1] = 0xF7
  reaper.SendMIDIMessageToHardware(out_dev, string.char(table.unpack(bytes)))
end

local function text(row, s)
  s = tostring(s):gsub("[^\32-\126]", " "):sub(1, TEXT_COLS)
  if last_rows[row + 1] == s then return end
  last_rows[row + 1] = s
  local payload = { row }
  for i = 1, #s do payload[#payload + 1] = s:byte(i) end
  send(SUB_TEXT, payload)
end

-- --- what the DAW shows ----------------------------------------------------

local function track_color(track)
  local native = reaper.GetTrackColor(track)
  if native == 0 then return DEFAULT_TRACK_COLOR end
  local r, g, b = reaper.ColorFromNative(native)
  return (r << 16) | (g << 8) | b
end

local function selected_track() return reaper.GetSelectedTrack(0, 0) end

local function pad_colors()
  local track = selected_track()
  local c = track and rgb7(track_color(track)) or { 0, 0, 0 }
  local colors = {}
  for i = 1, PAD_COUNT do colors[i] = c end
  return colors
end

local function mute_pad_color(index)
  local track = reaper.GetTrack(0, index)
  if not track then return { 0, 0, 0 } end
  local color = track_color(track)
  if reaper.GetMediaTrackInfo_Value(track, "B_MUTE") ~= 0 then
    return scale(color, MUTED_BRIGHTNESS)
  end
  local peak = math.max(reaper.Track_GetPeakInfo(track, 0), reaper.Track_GetPeakInfo(track, 1))
  local level = round(clamp(peak, 0.0, 1.0) * PEAK_STEPS) / PEAK_STEPS
  return scale(color, math.max(MIN_LIT_BRIGHTNESS, level))
end

local function control_colors()
  local state = reaper.GetPlayState()
  local active = {
    [CTRL_PLAY] = state & PLAYSTATE_PLAYING ~= 0,
    [CTRL_STOP] = state & PLAYSTATE_PLAYING == 0,
    [CTRL_RECORD] = state & PLAYSTATE_RECORDING ~= 0,
    [CTRL_REPEAT] = reaper.GetSetRepeat(-1) == 1,
    [CTRL_TRACK_PREV] = true,
    [CTRL_TRACK_NEXT] = true,
    [CTRL_METRONOME] = reaper.GetToggleCommandState(CMD_METRONOME) == 1,
    [CTRL_UNDO] = true,
  }
  local colors = {}
  for pad = 0, CTRL_MUTE_FIRST - 1 do
    colors[pad + 1] = rgb7(CTRL_COLORS[pad][active[pad] and 2 or 1])
  end
  for i = 0, CTRL_MUTE_COUNT - 1 do
    colors[CTRL_MUTE_FIRST + i + 1] = mute_pad_color(i)
  end
  return colors
end

local function push_colors(force)
  local colors = mode == MODE_CONTROL and control_colors() or pad_colors()
  if not force and last_colors then
    local changed = {}
    for pad = 1, PAD_COUNT do
      if not same_color(colors[pad], last_colors[pad]) then changed[#changed + 1] = pad end
    end
    if #changed == 0 then return end
    if #changed <= LED_SINGLE_MAX then
      -- Meters move one or two pads at a time; a full repaint per tick would
      -- be 48 bytes of SysEx for every flicker.
      for _, pad in ipairs(changed) do
        local c = colors[pad]
        send(SUB_LED, { pad - 1, c[1], c[2], c[3] })
      end
      last_colors = colors
      return
    end
  end
  last_colors = colors
  local payload = {}
  for pad = 1, PAD_COUNT do
    local c = colors[pad]
    payload[#payload + 1] = c[1]
    payload[#payload + 1] = c[2]
    payload[#payload + 1] = c[3]
  end
  send(SUB_LED_ALL, payload)
end

local function cursor_position()
  if reaper.GetPlayState() & PLAYSTATE_PLAYING ~= 0 then return reaper.GetPlayPosition() end
  return reaper.GetCursorPosition()
end

-- Region under the cursor, else the last marker before it, else the project.
local function place_name()
  local marker, region = reaper.GetLastMarkerAndCurRegion(0, cursor_position())
  for _, idx in ipairs({ region, marker }) do
    if idx >= 0 then
      local _, _, _, _, name = reaper.EnumProjectMarkers(idx)
      if name ~= "" then return name end
    end
  end
  local project = reaper.GetProjectName(0, "")
  return project ~= "" and project:gsub("%.[Rr][Pp][Pp]$", "") or "(unsaved)"
end

local function push_text()
  local track = selected_track()
  local name = "-"
  if track then
    local _, n = reaper.GetTrackName(track)
    name = n
  end
  text(0, name)
  text(1, place_name())
  text(2, string.format("%.1f BPM", reaper.Master_GetTempo()))
end

local function push_transport(force)
  local state = reaper.GetPlayState()
  local bits = 0
  if state & PLAYSTATE_PLAYING ~= 0 then bits = bits | 1 end
  if state & PLAYSTATE_RECORDING ~= 0 then bits = bits | 2 end
  if reaper.GetSetRepeat(-1) == 1 then bits = bits | 4 end
  if force or bits ~= last_transport then
    last_transport = bits
    send(SUB_TRANSPORT, { bits })
  end
end

local function push_all()
  last_rows = { "", "", "" }
  last_colors = nil
  last_transport = nil
  send(SUB_CONFIG, { BASE_NOTE, MIDI_CHANNEL - 1 })
  -- The layout is the board's: it answers every hello with its `mode`, and
  -- set_mode repaints for it. Until then paint for the mode last known.
  push_colors(true)
  push_text()
  push_transport(true)
end

-- --- the pads play the selected track ------------------------------------------

local function track_alive(entry)
  return entry and reaper.ValidatePtr2(0, entry.track, "MediaTrack*")
      and reaper.GetTrackGUID(entry.track) == entry.guid
end

local function release_armed()
  if track_alive(armed) then
    local t = armed.track
    reaper.SetMediaTrackInfo_Value(t, "I_RECINPUT", armed.input)
    reaper.SetMediaTrackInfo_Value(t, "I_RECMON", armed.mon)
    reaper.SetMediaTrackInfo_Value(t, "I_RECARM", armed.arm)
  end
  armed = nil
end

local function follow_selection()
  local track = selected_track()
  if not connected or not in_dev or not track then
    release_armed()
    return
  end
  if armed and armed.track == track and track_alive(armed) then return end
  release_armed()
  armed = {
    track = track,
    guid = reaper.GetTrackGUID(track),
    arm = reaper.GetMediaTrackInfo_Value(track, "I_RECARM"),
    mon = reaper.GetMediaTrackInfo_Value(track, "I_RECMON"),
    input = reaper.GetMediaTrackInfo_Value(track, "I_RECINPUT"),
  }
  reaper.SetMediaTrackInfo_Value(track, "I_RECINPUT",
    RECINPUT_MIDI | (in_dev << RECINPUT_DEV_SHIFT) | MIDI_CHANNEL)
  reaper.SetMediaTrackInfo_Value(track, "I_RECMON", 1)
  reaper.SetMediaTrackInfo_Value(track, "I_RECARM", 1)
end

local function select_track_by(delta)
  local count = reaper.CountTracks(0)
  if count == 0 then return end
  local current = selected_track()
  local idx = current and (reaper.GetMediaTrackInfo_Value(current, "IP_TRACKNUMBER") - 1) or -1
  local target = reaper.GetTrack(0, (idx + delta) % count)
  reaper.SetOnlyTrackSelected(target)
  reaper.Main_OnCommand(40913, 0)   -- Track: Vertical scroll selected tracks into view
end

-- --- control mode ------------------------------------------------------------

local function set_mode(m)
  m = m == MODE_CONTROL and MODE_CONTROL or MODE_PADS
  if m == mode then return end
  mode = m
  push_colors(true)
  push_text()
end

-- One undo point per gesture: the run moves the tempo without, the landing
-- after release records it.
local function set_tempo(bpm, undo)
  reaper.SetCurrentBPM(0, bpm, undo)
end

local function control_pan(pad, value)
  if pad ~= CTRL_METRONOME or not metro_hold then return end
  if now() - metro_hold.t0 < PAN_SETTLE_S then return end
  local offset = value - PAN_CENTRE
  if math.abs(offset) <= PAN_DEAD_ZONE then
    metro_hold.rate = 0.0
    metro_hold.lean = 0.0
    return
  end
  metro_hold.moved = true
  local lean = math.min(1.0, (math.abs(offset) - PAN_DEAD_ZONE) / (PAN_FULL - PAN_DEAD_ZONE))
  if metro_hold.lean < TEMPO_ACCEL_LEAN and lean >= TEMPO_ACCEL_LEAN then
    metro_hold.last = now()   -- the edge time starts now, not at the press
  end
  metro_hold.lean = lean
  metro_hold.rate = (offset > 0 and 1.0 or -1.0) * lean * TEMPO_RATE_MAX
end

local function run_tempo(t)
  local h = metro_hold
  if not h then return end
  local dt = t - h.last
  h.last = t
  if h.lean >= TEMPO_ACCEL_LEAN then
    h.edge_s = h.edge_s + dt
    if h.edge_s > TEMPO_ACCEL_AFTER_S then
      local ramp = math.min(1.0, (h.edge_s - TEMPO_ACCEL_AFTER_S) / TEMPO_ACCEL_RAMP_S)
      h.accel = 1.0 + ramp * (TEMPO_ACCEL_MAX - 1.0)
    end
  else
    h.edge_s = 0.0
    h.accel = 1.0
  end
  if h.rate == 0.0 then return end
  local before = h.tempo
  local after = clamp(before + h.rate * h.accel * dt, TEMPO_MIN, TEMPO_MAX)
  h.tempo = after
  if round(after) ~= round(before) then
    set_tempo(round(after), false)
    push_text()
  end
end

local function control_release(pad)
  if pad ~= CTRL_METRONOME or not metro_hold then return end
  local h = metro_hold
  metro_hold = nil
  if h.moved then
    -- A long run at the edge was aiming for a round number; a short, careful
    -- one for exactly where it stopped.
    local landed
    if h.edge_s > TEMPO_ACCEL_AFTER_S then
      landed = round(h.tempo / TEMPO_SNAP_STEP) * TEMPO_SNAP_STEP
    else
      landed = round(h.tempo)
    end
    set_tempo(clamp(landed, TEMPO_MIN, TEMPO_MAX), true)
    push_text()
  else
    reaper.Main_OnCommand(CMD_METRONOME, 0)
    push_colors()
  end
end

local function control_press(pad)
  if pad == CTRL_METRONOME then
    local t = now()
    metro_hold = { tempo = reaper.Master_GetTempo(), rate = 0.0, lean = 0.0, moved = false,
                   t0 = t, last = t, edge_s = 0.0, accel = 1.0 }
    return
  end
  if pad == CTRL_PLAY then
    reaper.OnPlayButton()
  elseif pad == CTRL_STOP then
    reaper.OnStopButton()
  elseif pad == CTRL_RECORD then
    reaper.Main_OnCommand(CMD_RECORD, 0)
  elseif pad == CTRL_REPEAT then
    reaper.GetSetRepeat(2)
  elseif pad == CTRL_TRACK_PREV then
    select_track_by(-1)
  elseif pad == CTRL_TRACK_NEXT then
    select_track_by(1)
  elseif pad == CTRL_UNDO then
    reaper.Undo_DoUndo2(0)
  elseif pad >= CTRL_MUTE_FIRST and pad < CTRL_MUTE_FIRST + CTRL_MUTE_COUNT then
    local track = reaper.GetTrack(0, pad - CTRL_MUTE_FIRST)
    if track then
      local muted = reaper.GetMediaTrackInfo_Value(track, "B_MUTE")
      reaper.SetMediaTrackInfo_Value(track, "B_MUTE", muted ~= 0 and 0 or 1)
    end
  end
  push_colors()
  push_transport()
end

-- --- the beat ------------------------------------------------------------------

local function flash_metronome(value)
  if mode ~= MODE_CONTROL or not last_colors then return end
  -- The metronome pad blinks with the beat in the screen's dot colours for
  -- BEAT_FLASH_S; colour passes pause meanwhile so the meters do not
  -- overwrite the flash.
  local flash = rgb7(value == 1 and BEAT_BAR_COLOR or BEAT_BEAT_COLOR)
  last_colors[CTRL_METRONOME + 1] = flash
  send(SUB_LED, { CTRL_METRONOME, flash[1], flash[2], flash[3] })
  metro_flash_until = now() + BEAT_FLASH_S
end

local function run_beat()
  if reaper.GetPlayState() & PLAYSTATE_PLAYING == 0 then
    last_beat = nil
    return
  end
  local beat_in_bar, measure = reaper.TimeMap2_timeToBeats(0, reaper.GetPlayPosition())
  local beat = math.floor(beat_in_bar)
  local key = measure * 1000 + beat
  if key == last_beat then return end
  last_beat = key
  local value = beat == 0 and 1 or 2
  send(SUB_BEAT, { value })
  flash_metronome(value)
end

-- --- what the board says -------------------------------------------------------

local function on_sysex(buf)
  if #buf < 5 or buf:byte(2) ~= MANUFACTURER or buf:byte(3) ~= CMD_DAW then return end
  local sub = buf:byte(4)
  if sub == SUB_MODE then
    set_mode(buf:byte(5))
  elseif sub == SUB_PAD and #buf >= 7 then
    if connected then
      if buf:byte(6) == 1 then control_press(buf:byte(5)) else control_release(buf:byte(5)) end
    end
  elseif sub == SUB_PAN and #buf >= 7 then
    if connected then control_pan(buf:byte(5), buf:byte(6)) end
  elseif sub == SUB_HELLO and #buf >= 9 then
    -- Device hello: proto pads rows caps. A host's own hello (2 args) never
    -- reaches this script — the hub does not echo it back.
    local pads = buf:byte(6)
    connected = pads >= PAD_COUNT
    if connected then
      -- Answer every device hello: the app may have started, or the board
      -- rebooted, long after ours. The device replies with its own hello
      -- only the first time, so this cannot ping-pong.
      send(SUB_HELLO, { DAW_ID_REAPER, PROTO_VER })
      push_all()
    end
  end
end

local function on_short(buf, dev)
  if (dev & DEVIDX_MASK) ~= in_dev or #buf < 3 then return end
  local status, data1, data2 = buf:byte(1), buf:byte(2), buf:byte(3)
  if status & 0xF0 ~= 0xB0 then return end
  if data1 == CC_ENCODER then
    if last_encoder then
      local half = ENCODER_DETENTS // 2
      local delta = (data2 - last_encoder + half) % ENCODER_DETENTS - half
      if delta ~= 0 then select_track_by(delta) end
    end
    last_encoder = data2
  elseif data1 == CC_ENCODER_BUTTON and data2 > 0 then
    reaper.OnPlayButton()
  end
end

-- MIDI_GetRecentInputEvent(0) is the newest event; walk back to the last one
-- seen and handle them oldest first.
local function read_input()
  local seq = reaper.MIDI_GetRecentInputEvent(0)
  if last_event_seq == nil then
    last_event_seq = seq   -- history from before we started is not ours to act on
    return
  end
  if seq == last_event_seq then return end
  local fresh = {}
  local idx = 0
  while true do
    local s, buf, _, dev = reaper.MIDI_GetRecentInputEvent(idx)
    if s == 0 or s == last_event_seq then break end
    fresh[#fresh + 1] = { buf, dev }
    idx = idx + 1
  end
  last_event_seq = seq
  for i = #fresh, 1, -1 do
    local buf, dev = fresh[i][1], fresh[i][2]
    if buf:byte(1) == 0xF0 then on_sysex(buf) else on_short(buf, dev) end
  end
end

-- --- main loop -----------------------------------------------------------------

local function say_hello()
  connected = false
  send(SUB_HELLO, { DAW_ID_REAPER, PROTO_VER })
end

local function loop()
  local t = now()
  if not (in_dev and out_dev) then
    if t >= next_device_try then
      next_device_try = t + NO_DEVICE_RETRY_S
      if open_device() then say_hello() end
    end
  else
    read_input()
    follow_selection()
    if connected then
      run_tempo(t)
      run_beat()
      push_text()
      push_transport()
      if mode == MODE_CONTROL then
        if metro_flash_until > 0 and t >= metro_flash_until then
          metro_flash_until = 0.0
          push_colors(true)
        elseif metro_flash_until == 0 then
          push_colors()
        end
      else
        push_colors()
      end
      if t - last_ping >= PING_PERIOD_S then
        last_ping = t
        send(SUB_PING)
      end
    end
  end
  reaper.defer(loop)
end

reaper.atexit(function()
  release_armed()
  send(SUB_BYE)
end)

loop()
