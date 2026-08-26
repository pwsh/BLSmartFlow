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

// Longest single G-code line the queue accepts, including the NUL. Two M106
// commands and their newlines fit in well under half of it.
static const size_t PRINTER_GCODE_MAX = 96;

// Queues one G-code line (or several, separated by '\n') for the printer task
// to publish as a `gcode_line` request on device/<serial>/request.
//
// Safe to call from any task: the text is copied into a small spinlock-guarded
// ring under a critical section and the caller never touches MQTT. Returns false
// when the queue is full, the text is empty or it does not fit - the caller is
// expected to try again on its next tick rather than block (REWORK-SPEC 17.2).
//
// This is the *only* path by which the firmware ever commands the printer.
bool printerLinkSendGcode(const char* gcode);

// Number of queued lines that have not been published yet.
uint8_t printerLinkGcodePending();

}  // namespace blsf

#endif  // BLSF_PRINTER_LINK_H
