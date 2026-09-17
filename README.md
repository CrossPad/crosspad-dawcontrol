# DAW Control

CrossPad as a DAW controller. The pads play the DAW's drum instrument, the DAW
paints the pads (instrument colours, hit feedback, beat), the LCD shows what
the DAW says, and the board's own USB audio is the audio interface.

The device side is generic: it plays notes and shows what it is told over one
SysEx protocol. Each DAW gets a small host script that speaks it. First host:
FL Studio (also under Wine). Ableton Live follows on the same protocol.

## What is in here

| Path | Purpose |
|------|---------|
| `src/DawControlApp.cpp` | Screen: host name, three text rows, transport line, beat dot; wires the host SysEx callback |
| `src/DawControlPadLogic.cpp` | Pads → notes (+ aftertouch); LEDs = host colours, press flashes them |
| `src/DawProtocol.cpp` | The wire protocol: parses host frames, answers `hello` |
| `host/fl_studio/device_CrossPad.py` | FL Studio MIDI Controller Script |
| `crosspad-app.json` | Registry metadata |

## Wire protocol

`F0 7D 1E <sub> … F7`, every byte 7-bit. Colours are `r g b` each 0..127
(the device doubles them). Text is ASCII 0x20..0x7E, 24 chars per row.
Catalog of record: crosspad-hil `crosspad_hil/knowledge/sysex.yaml`, `daw`.

| sub | direction | payload |
|---|---|---|
| `01 hello` | host → device | `daw_id proto_ver` (1 FL Studio, 2 Ableton, 0 other) |
| `01 hello` | device → host | `proto_ver pads rows caps` — reply, and unsolicited on every app show |
| `02 led` | host → device | `pad r g b` |
| `03 led_all` | host → device | 16 × `r g b` |
| `04 text` | host → device | `row` + chars (empty clears the row) |
| `05 beat` | host → device | `0 off / 1 bar / 2 beat` |
| `06 transport` | host → device | bit0 playing, bit1 recording, bit2 loop |
| `07 mode` | host → device | `0 pads / 1 steps / 2 clips` (shown only) |
| `07 mode` | device → host | `0 pads / 3 control` — the board's layout, after every host hello and on every double tap of the case |
| `08 config` | host → device | `base_note channel(0..15)` — default 36 / channel 10 |
| `09 pad` | device → host | `pad state(1 press / 0 release)` — control mode only; no notes or aftertouch go out there |
| `7F bye` | host → device | host script unloading |

Device → host beyond `hello`, `mode` and `pad` is plain MIDI: NoteOn/NoteOff
`base_note + pad` with the pad velocity, polyphonic aftertouch from pressure.

**Two layouts, one double tap.** Knock the case twice (the IMU's double tap,
`MotionGesture::DoubleTap` on the event bus) and the pads stop being a keyboard:
in *control* mode a press goes out as `09 pad` and the host decides what it
does; another double tap is the keyboard again. The board always starts as a
keyboard and tells the host its layout with `07 mode`, also after every host
hello, so a reconnect paints the right one. Bench: `IMU_GESTURE DOUBLE_TAP`
over CDC, `DAW_STATE` shows `mode=3`. The navigation
encoder is emitted by the firmware itself on channel 1: CC 1 = detent index
0..15 (wraps), CC 64 = switch. Host scripts turn the CC 1 wrap into a delta.

## FL Studio

1. Copy `host/fl_studio/` as `CrossPad/` into
   `<Documents>/Image-Line/FL Studio/Settings/Hardware/`.
   Under Wine that is `<prefix>/drive_c/users/<you>/Documents/Image-Line/FL
   Studio/Settings/Hardware/CrossPad/` (Bottles keeps the prefix under
   `~/.var/app/com.usebottles.bottles/data/bottles/bottles/<bottle>/`). FL
   discovers new script folders only at startup.
2. FL → Options → MIDI Settings: pick the CrossPad port as input **and**
   output, controller type **CrossPad**, enable, and give both the same
   port number — the script's `device.isAssigned()` is false otherwise.
   Send master sync is not needed.
   **Under Wine, pick "CrossPad Hub" instead of the board** (both
   directions): `crosspad-hil midi hub install` keeps that port pair alive
   from login and bridges whichever CrossPad is plugged in, so FL can start
   first, the board can reboot or switch its USB profile, and the link comes
   back on its own. `crosspad-hil audio nodes install --take-defaults` does
   the same for audio: "CrossPad Microphone" / "CrossPad Output" appear with
   the card and FL's streams follow them.
3. The app's bottom bar picks what "[Default input]" carries — **Mics**,
   **Line in**, **Spk bus**, **HP bus** (the same presets as Settings → USB)
   — and **USB audio** flips the board into the MIDI+UAC2 profile (persisted;
   CDC is gone in that profile, MIDI and SysEx stay).

Wine traps: only one MIDI input can be open at a time, and a board reboot
(OTA flash) or a script reload leaves that input allocated but unlinked from
the port; relink it from Linux instead of restarting FL:
`aconnect <crosspad>:0 <wine>:1; aconnect <wine>:0 <crosspad>:0` (numbers
from `aconnect -l`), then restart DAW Control so it hellos again. Wine also routes
incoming MIDI to the device by its ALSA source address, so a host tool cannot
inject MIDI into FL from its own client; a bench that needs that (the
fl-studio-mcp bridge) turns on `SYSEX_ECHO 1` over CDC and the board bounces
foreign-manufacturer SysEx back to the host.
3. On the board open **DAW Control**. The screen switches from "no host" to
   "FL Studio | pads" as soon as the script answers.

The host row: the MIDI hub (`crosspad-hil midi hub`, the same service that
gives FL its fixed port) reports this computer's CPU, RAM and clock to the
board every 2 s as companion notify frames (`0x1F 04`, `HOST_INFO` /
`FW_LATEST`, see platform-idf `docs/subsystems/ble-midi.md`). It runs from
login, so this works with FL closed too. While the reports arrive the status
bar shows the USB glyph in place of the charging bolt, and DAW Control's host
row shows the numbers. Put `{"version": "x.y.z"}` in
`~/.config/crosspad/latest.json` to announce newer firmware: the glyph turns
orange, DAW Control and Settings → Info say "vX at crosspad.app/device", and
the board remembers it across resets.

What the script does: pads reach the selected channel (put FPC there and the
pads wear FPC's pad colours; any other instrument paints them the channel
colour). Rows: channel name, pattern name, tempo. The beat dot follows FL's
beat indicator. Encoder scrolls the channel rack selection, its switch is
play/pause. Hits are lit on the board by the pad itself; FL does not need to
echo them.

Control layout (double tap the case), row 0 at the top, LEDs lit by state:

| row | pads |
|---|---|
| 0 | **Play/Pause** (green) · **Stop** (red) · **Record** (red) · **Loop mode** pattern/song (yellow when song) |
| 1 | **Pattern −** · **Pattern +** (blue) · **Metronome** (white when on) · **Undo** (purple) |
| 2–3 | **Mute** channel rack channels 1–8: the channel's colour, as bright as its mixer track's fader (0 dB = full), a glimmer when muted |

### USB audio under Wine

The board enumerates a UAC2 card (`Crosspad Audio`) in the MIDI+audio USB
profile. On Linux it is an ALSA/PipeWire device; FL under Wine reaches it
through `wineasio` (ASIO → JACK/PipeWire) — select the CrossPad card as the
PipeWire default input while recording, or map it in the JACK graph.
Choose what the board sends with `crosspad-hil audio route MICS|LINE_IN|…`
(the presets name real signal paths, see `docs/subsystems/audio-routing.md`).
The profile switch drops CDC, so switch after any `crosspad-hil` work is done:
`crosspad-hil usb-mode audio`.

## Kit sync (host side)

A folder of WAVs becomes a kit on the card with
`crosspad-hil kit push <dir> [--name NAME] [--load]` — files sorted by name
map to pads 0..15, converted to 16-bit 44.1 kHz, `kit.json` written last,
board rebooted for the kit scan, optional `KIT_LOAD`. That path needs CDC, so
it runs from the host shell, not from inside the DAW.

## Test

`crosspad-hil run daw_control`: starts the app over CDC, exchanges `hello`
over the ESP MIDI port, paints the pads with `led_all` and reads them back
with `LED_STATE`, presses a pad and expects its NoteOn on the host MIDI input.

## Install into a project

```bash
idf.py app-install --app dawcontrol --origin <this repo URL>
idf.py fullclean && idf.py build
```

Requires crosspad-core ≥ 1.13.0 (`CROSSPAD_CMD_DAW`, `IMidiOutput::sendSysEx`,
`PlatformServices::stm32MessageHandler`, the host fields of `CrosspadStatus`).
