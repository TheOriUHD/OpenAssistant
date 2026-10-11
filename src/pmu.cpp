#include "pmu.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"

static const char* TAG = "pmu";
static i2c_master_dev_handle_t s_dev;

static void write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    ESP_ERROR_CHECK(i2c_master_transmit(s_dev, buf, sizeof(buf), 100));
}

static uint8_t read_reg(uint8_t reg) {
    uint8_t val = 0;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(s_dev, &reg, 1, &val, 1, 100));
    return val;
}

static void set_aldo3(bool on) {
    uint8_t reg = read_reg(0x90);
    write_reg(0x90, on ? (reg | 0x04) : (reg & ~0x04));
}

// Register values follow Waveshare's XiaoZhi BSP for this board.
void pmu_init(i2c_master_bus_handle_t bus) {
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = BOARD_AXP2101_ADDR;
    cfg.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg, &s_dev));

    write_reg(0x22, 0b110);  // PWRON > OFFLEVEL as power-off source
    write_reg(0x27, 0x10);   // hold PWR 4 s to power off
    write_reg(0x80, 0x01);   // only DC1 on
    write_reg(0x82, (3300 - 1500) / 100);  // DC1 3.3 V
    for (uint8_t reg = 0x92; reg <= 0x95; reg++) {
        write_reg(reg, (3300 - 500) / 100);  // ALDO1..4 3.3 V
    }
    write_reg(0x90, 0x0F);   // ALDO1..4 on
    write_reg(0x64, 0x02);   // charge to 4.1 V
    write_reg(0x61, 0x02);   // precharge 50 mA
    write_reg(0x62, 0x0A);   // charge 400 mA
    write_reg(0x63, 0x01);   // termination 25 mA

    // ALDO3 doubles as the panel reset line.
    set_aldo3(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    set_aldo3(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    set_aldo3(true);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "AXP2101 rails up, panel reset pulsed");
}
