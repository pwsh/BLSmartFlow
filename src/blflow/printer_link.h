// printer_link.h - MQTT link to the Bambu Lab printer.
//
// The printer speaks MQTT over TLS on port 8883 with user "bblp" and the LAN
// access code as the password. A printer that is switched off, or a TLS
// handshake against an unreachable IP, can block for many seconds - so the whole
// client lives in its own FreeRTOS task and never touches the Arduino loop.
// Everything it learns is published through state.h's snapshot API.

#ifndef BLSF_PRINTER_LINK_H
#define BLSF_PRINTER_LINK_H

#include <Arduino.h>

namespace blsf {

// Creates the "printer" task. Safe to call once, from setup().
void printerLinkStart();

// Re-reads the printer section of the config and forces a reconnect on the next
// task iteration. Called after any change to printer settings - no reboot needed.
void printerLinkReconfigure();

// True when IP, access code and serial are all present.
bool printerLinkConfigured();

// True when the printer has reported within config.fan.staleSec.
bool printerLinkOnline();

}  // namespace blsf

#endif  // BLSF_PRINTER_LINK_H
