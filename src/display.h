#pragma once

#include <driver/i2c_master.h>
#include <lvgl.h>

// Starts the panel, touch and the LVGL task. Use lvgl_port_lock() around any
// LVGL call made outside LVGL callbacks.
lv_display_t* display_init(i2c_master_bus_handle_t bus);

// 0..255, sent as the panel's own brightness command (no backlight on AMOLED).
void display_set_brightness(uint8_t level);
