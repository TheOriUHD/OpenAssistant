#pragma once

// Waveshare ESP32-C6-Touch-AMOLED-2.16. Pins match Waveshare's XiaoZhi board
// definition for this model and the Clawdmeter C6 port (both run on this
// hardware). Waveshare's ESP-IDF examples swap LCD_CS and TP_INT; they're wrong.

#include <driver/gpio.h>

// Shared I2C bus: AXP2101 PMU, CST9217 touch, ES8311 DAC, ES7210 ADC,
// QMI8658 IMU, PCF85063 RTC.
#define BOARD_I2C_SDA       GPIO_NUM_8
#define BOARD_I2C_SCL       GPIO_NUM_7

#define BOARD_AXP2101_ADDR  0x34

// SH8601-compatible AMOLED over QSPI. No reset GPIO: the panel's reset is
// tied to the PMU's ALDO3 rail (see pmu.cpp).
#define LCD_H_RES           480
#define LCD_V_RES           480
#define LCD_CS              GPIO_NUM_15
#define LCD_PCLK            GPIO_NUM_0
#define LCD_D0              GPIO_NUM_1
#define LCD_D1              GPIO_NUM_2
#define LCD_D2              GPIO_NUM_3
#define LCD_D3              GPIO_NUM_4

#define TP_RST              GPIO_NUM_11
#define TP_INT              GPIO_NUM_5

// I2S to ES8311 (speaker) and ES7210 (microphones). No amp-enable pin.
#define AUDIO_I2S_MCLK      GPIO_NUM_19
#define AUDIO_I2S_BCLK      GPIO_NUM_20
#define AUDIO_I2S_WS        GPIO_NUM_22
#define AUDIO_I2S_DOUT      GPIO_NUM_23
#define AUDIO_I2S_DIN       GPIO_NUM_21

#define BOOT_BUTTON         GPIO_NUM_9
// Third side button ("KEY"); not in Waveshare's docs, found by the Clawdmeter
// port scanning GPIOs. Active low like BOOT.
#define KEY_BUTTON          GPIO_NUM_10
