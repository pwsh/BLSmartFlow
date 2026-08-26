// ha_mqtt.h - optional link to an external MQTT broker, with Home Assistant
// auto-discovery.
//
// Independent of the printer link: plain TCP, its own PubSubClient, driven from
// the main loop. Publishing is rate-limited to config.mqtt.publishIntervalSec
// plus an immediate publish whenever the fan output changes, so a broker outage
// or a chatty fan curve cannot flood the network.

#ifndef BLSF_HA_MQTT_H
#define BLSF_HA_MQTT_H

#include <Arduino.h>

namespace blsf {

void haMqttSetup();
void haMqttLoop();

// Applies a change to the mqtt section: disconnects, re-reads and (re)publishes
// discovery as needed.
void haMqttReconfigure();

bool haMqttConnected();

}  // namespace blsf

#endif  // BLSF_HA_MQTT_H
