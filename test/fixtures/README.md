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
  minute and must be ignored by the parser.
