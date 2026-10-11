#include "display.h"

#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_sh8601.h>
#include <esp_lcd_touch_cst9217.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>

#include "board.h"

static const char* TAG = "display";

// LVGL renders in horizontal bands this tall. Two of them, in internal DMA
// RAM: 2 x 480 x 40 x 2 B = 75 KB. The board has no PSRAM, so this is the
// biggest single RAM cost in the firmware.
#define DRAW_BUF_LINES 40

static esp_lcd_panel_io_handle_t s_io;

// The page-0x20 writes set the panel driving voltage; without them this
// panel stays black (found in the Clawdmeter C6 port). The rest mirrors
// Waveshare's XiaoZhi init. Brightness stays 0 until the first frame is out.
static const sh8601_lcd_init_cmd_t k_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},  // sleep out
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},    // QSPI mode
    {0x35, (uint8_t[]){0x00}, 1, 0},    // tearing effect on
    {0x53, (uint8_t[]){0x20}, 1, 10},   // brightness control on
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},   // display on
};

// The controller only accepts windows that start on an even pixel and have
// an even size.
static void rounder_cb(lv_area_t* area) {
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

void display_set_brightness(uint8_t level) {
    if (!s_io) return;
    // QSPI command framing: opcode 0x02, then the DCS command in bits 8..15.
    int cmd = (0x02 << 24) | (0x51 << 8);
    esp_lcd_panel_io_tx_param(s_io, cmd, &level, 1);
}

static esp_lcd_panel_handle_t init_panel(void) {
    spi_bus_config_t bus = {};
    bus.sclk_io_num = LCD_PCLK;
    bus.data0_io_num = LCD_D0;
    bus.data1_io_num = LCD_D1;
    bus.data2_io_num = LCD_D2;
    bus.data3_io_num = LCD_D3;
    bus.max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(LCD_CS, nullptr, nullptr);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_io));

    sh8601_vendor_config_t vendor = {};
    vendor.init_cmds = k_init_cmds;
    vendor.init_cmds_size = sizeof(k_init_cmds) / sizeof(k_init_cmds[0]);
    vendor.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = GPIO_NUM_NC;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.vendor_config = &vendor;

    esp_lcd_panel_handle_t panel = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    return panel;
}

static esp_lcd_touch_handle_t init_touch(i2c_master_bus_handle_t i2c) {
    esp_lcd_panel_io_handle_t tp_io = nullptr;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c, &tp_io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max = LCD_H_RES - 1;
    tp_cfg.y_max = LCD_V_RES - 1;
    tp_cfg.rst_gpio_num = TP_RST;
    tp_cfg.int_gpio_num = TP_INT;

    esp_lcd_touch_handle_t tp = nullptr;
    esp_err_t err = esp_lcd_touch_new_i2c_cst9217(tp_io, &tp_cfg, &tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed: %s", esp_err_to_name(err));
        return nullptr;
    }
    return tp;
}

lv_display_t* display_init(i2c_master_bus_handle_t i2c) {
    esp_lcd_panel_handle_t panel = init_panel();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 3;
    port_cfg.task_stack = 6144;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = s_io;
    disp_cfg.panel_handle = panel;
    disp_cfg.buffer_size = LCD_H_RES * DRAW_BUF_LINES;
    disp_cfg.double_buffer = true;
    disp_cfg.hres = LCD_H_RES;
    disp_cfg.vres = LCD_V_RES;
    disp_cfg.rounder_cb = rounder_cb;
    disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.buff_dma = 1;
    disp_cfg.flags.swap_bytes = 1;
    lv_display_t* disp = lvgl_port_add_disp(&disp_cfg);

    esp_lcd_touch_handle_t tp = init_touch(i2c);
    if (tp) {
        lvgl_port_touch_cfg_t touch_cfg = {};
        touch_cfg.disp = disp;
        touch_cfg.handle = tp;
        lvgl_port_add_touch(&touch_cfg);
    }

    ESP_LOGI(TAG, "display up (%dx%d, %d-line buffers)", LCD_H_RES, LCD_V_RES, DRAW_BUF_LINES);
    return disp;
}
