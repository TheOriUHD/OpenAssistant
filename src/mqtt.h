#pragma once

// Registers the panel in Home Assistant via MQTT discovery as the device
// "Voice Panel" (status sensor + mute switch). The device is what gives
// commands an area: HA resolves "Licht an" against the device's area.

// Entity HA creates for the status sensor; ha.cpp looks the device up by it.
#define MQTT_STATUS_ENTITY "sensor.voice_panel_status"

typedef void (*mqtt_mute_cb_t)(bool muted);

// Called from the MQTT task when the mute switch is flipped in HA.
void mqtt_init(mqtt_mute_cb_t on_mute);

void mqtt_publish_state(const char* state);
void mqtt_publish_mute(bool muted);
