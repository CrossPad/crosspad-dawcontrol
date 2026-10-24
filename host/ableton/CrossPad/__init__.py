"""
Ableton Live MIDI Remote Script for CrossPad running the DAW Control app.

Install: copy this directory (as `CrossPad/`) into
  <User Library>/Remote Scripts/
(macOS ~/Music/Ableton/User Library/Remote Scripts/, Windows
<Documents>/Ableton/User Library/Remote Scripts/, under Wine the Windows path
inside the prefix), restart Live, then Settings -> Link, Tempo & MIDI ->
Control Surface "CrossPad" with the CrossPad port as input and output, and
Track + Remote on for that input. The script itself is in crosspad.py.
"""

from .crosspad import CrossPad

try:
    from _Framework.Capabilities import (CONTROLLER_ID_KEY, NOTES_CC, PORTS_KEY, REMOTE, SCRIPT,
                                         controller_id, inport, outport)
except ImportError:   # a Live without _Framework: no auto-detection, the manual setup still works
    controller_id = None

# The board's USB IDs (both profiles carry MIDI: CDC 0x3456, MIDI+UAC2 0x3457)
# and the product strings its ports are named after. The STM32 bridge's
# "CrossPad MIDI+Serial" is another device (0x0483) and is not the DAW's.
USB_VENDOR_ID = 0x303A
USB_PRODUCT_IDS = [0x3456, 0x3457]
USB_MODEL_NAMES = ["Crosspad", "Crosspad Audio"]


def get_capabilities():
    if controller_id is None:
        return {}
    return {
        CONTROLLER_ID_KEY: controller_id(vendor_id=USB_VENDOR_ID, product_ids=USB_PRODUCT_IDS,
                                         model_name=USB_MODEL_NAMES),
        # NOTES_CC on the input is Live's "Track": the pads' notes reach the
        # selected track; SCRIPT/REMOTE carry the SysEx and the encoder.
        PORTS_KEY: [inport(props=[NOTES_CC, SCRIPT, REMOTE]), outport(props=[SCRIPT, REMOTE])],
    }


def create_instance(c_instance):
    return CrossPad(c_instance)
