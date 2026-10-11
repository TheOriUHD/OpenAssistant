#pragma once

#include <driver/i2c_master.h>

// Brings up the AXP2101 rails that power the display, touch and audio, and
// pulses the display reset (ALDO3). Must run before display and audio init.
void pmu_init(i2c_master_bus_handle_t bus);
