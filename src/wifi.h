#pragma once

// Connects to WIFI_SSID from secrets.h and keeps reconnecting. Returns
// immediately; use wifi_is_connected() to check.
void wifi_init(void);
bool wifi_is_connected(void);
