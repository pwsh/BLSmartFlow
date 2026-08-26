# MQTT fixtures

Captured 2026-08-25 from a Bambu Lab X1C (printer firmware reporting `ver: "20000"`) over local MQTT
while printing, then sanitised (job/network identifiers removed). Use them to test the parser.

* `x1c_push_status.json` — one full `print.push_status` report (the X1 series sends the full ~16 KB object
  every ~1.1 s). Notable: **no `chamber_temper` key**; the chamber temperature is only available as
  `print.device.ctc.info.temp` (also mirrored at `print.info.temp`). Bed and nozzle temperatures are present
  both as the classic floats (`bed_temper`, `nozzle_temper`, …) and packed into 32-bit words under
  `print.device.bed.info.temp` / `print.device.extruder.info[0].temp` (low 16 bits = current °C, high 16 bits = target).
  Fan speeds are strings on a 0–15 scale (`big_fan1_speed: "6"` = aux fan 40 %).
* `x1c_gcode_line.json` — a `print.command == "gcode_line"` acknowledgement; these arrive several times a
  minute and must be ignored by the parser. Bambu Studio's own commands are acknowledged here too, which is
  why an ack only counts as ours when its `sequence_id` is one the firmware published (ids from 5000 up).
* `x1c_gcode_line_rejected.json` — the same acknowledgement from a printer with **Developer Mode off**:
  `result: "failed"`, `reason: "mqtt message verify failed"`, `err_code: 84033543`. Reports keep flowing; only
  write commands are signature-checked, so this is the single piece of evidence that an `M106` was refused.
* `x1c_ams_trays.json` — the `ams` block and `vt_tray` from the same X1C while printing from AMS slot 0
  (`ams.tray_now = "0"`): ABS `GFB00`, PLA `GFA00`, PLA-AERO `GFA11`, PLA `GFA05`, external spool ASA `GFB01`.
  `tray_now` encoding: `ams_index*4 + slot`; `254` = external spool (`vt_tray`); `255` = none.
