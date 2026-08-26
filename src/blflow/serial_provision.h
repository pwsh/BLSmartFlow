// serial_provision.h - JSON-over-USB provisioning.
//
// The hosted WebSerial setup page (docs/wifiSetup.html) writes one JSON object
// per line to the USB serial port. The 1.x handler strcpy'd straight out of the
// document, so a message missing one key wrote from a null pointer; here every
// field is optional and bounded.
//
// Accepted forms:
//   {"ssid":"..","pass":"..","printerip":"..","printercode":"..","printerserial":".."}
//   {"config":{ ...full config document... }}
//   {"cmd":"status"|"restart"|"factoryreset"}

#ifndef BLSF_SERIAL_PROVISION_H
#define BLSF_SERIAL_PROVISION_H

#include <Arduino.h>

namespace blsf {

void serialProvisionSetup();
void serialProvisionLoop();

}  // namespace blsf

#endif  // BLSF_SERIAL_PROVISION_H
