#include "audio.h"

#include <driver/i2s_std.h>
#include <driver/i2s_tdm.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>

#include "board.h"

static const char* TAG = "audio";

// 36 dB: at room distance speech arrived around -36 dBFS with 30 dB, which
// openWakeWord and Whisper handle worse than a louder signal.
#define MIC_GAIN_DB    36.0f
#define SPEAKER_VOLUME 70

static esp_codec_dev_handle_t s_out;
static esp_codec_dev_handle_t s_in;
static QueueHandle_t s_chime_queue;
static volatile bool s_playing;
static volatile int64_t s_play_end_us;
static volatile bool s_sounds = true;

// One I2S port, full duplex: standard stereo out to the ES8311, 4-slot TDM in
// from the ES7210. Same clocking as Waveshare's XiaoZhi codec for this board.
static void create_i2s(i2s_chan_handle_t* tx, i2s_chan_handle_t* rx) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;  // 8 x 240 frames = 120 ms of slack before mic samples are lost
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, tx, rx));

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg.sample_rate_hz = AUDIO_SAMPLE_RATE;
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.bit_shift = true;
    std_cfg.slot_cfg.left_align = true;
    std_cfg.gpio_cfg.mclk = AUDIO_I2S_MCLK;
    std_cfg.gpio_cfg.bclk = AUDIO_I2S_BCLK;
    std_cfg.gpio_cfg.ws = AUDIO_I2S_WS;
    std_cfg.gpio_cfg.dout = AUDIO_I2S_DOUT;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;

    i2s_tdm_config_t tdm_cfg = {};
    tdm_cfg.clk_cfg.sample_rate_hz = AUDIO_SAMPLE_RATE;
    tdm_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    tdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tdm_cfg.clk_cfg.bclk_div = 8;
    tdm_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    tdm_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    tdm_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    tdm_cfg.slot_cfg.slot_mask =
        (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3);
    tdm_cfg.slot_cfg.ws_width = I2S_TDM_AUTO_WS_WIDTH;
    tdm_cfg.slot_cfg.bit_shift = true;
    tdm_cfg.slot_cfg.total_slot = I2S_TDM_AUTO_SLOT_NUM;
    tdm_cfg.gpio_cfg.mclk = AUDIO_I2S_MCLK;
    tdm_cfg.gpio_cfg.bclk = AUDIO_I2S_BCLK;
    tdm_cfg.gpio_cfg.ws = AUDIO_I2S_WS;
    tdm_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    tdm_cfg.gpio_cfg.din = AUDIO_I2S_DIN;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(*tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(*rx, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(*tx));
    ESP_ERROR_CHECK(i2s_channel_enable(*rx));
}

static void create_codecs(i2c_master_bus_handle_t bus, i2s_chan_handle_t tx, i2s_chan_handle_t rx) {
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_NUM_0;
    i2s_cfg.rx_handle = rx;
    i2s_cfg.tx_handle = tx;
    const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = I2C_NUM_0;
    i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = bus;
    const audio_codec_ctrl_if_t* out_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl;
    es8311_cfg.gpio_if = gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = -1;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    const audio_codec_if_t* out_codec = es8311_codec_new(&es8311_cfg);

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = out_codec;
    dev_cfg.data_if = data_if;
    s_out = esp_codec_dev_new(&dev_cfg);

    i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t* in_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    const audio_codec_if_t* in_codec = es7210_codec_new(&es7210_cfg);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec;
    s_in = esp_codec_dev_new(&dev_cfg);

    // Output: mono 16-bit. Input: 4 TDM slots, keep only slot 0 (MIC1).
    esp_codec_dev_sample_info_t out_fs = {};
    out_fs.bits_per_sample = 16;
    out_fs.channel = 1;
    out_fs.sample_rate = AUDIO_SAMPLE_RATE;
    ESP_ERROR_CHECK(esp_codec_dev_open(s_out, &out_fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_out, SPEAKER_VOLUME));

    esp_codec_dev_sample_info_t in_fs = {};
    in_fs.bits_per_sample = 16;
    in_fs.channel = 4;
    in_fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0);
    in_fs.sample_rate = AUDIO_SAMPLE_RATE;
    ESP_ERROR_CHECK(esp_codec_dev_open(s_in, &in_fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(s_in, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), MIC_GAIN_DB));
}

// A soft bell: sine with a fast attack and exponential decay.
static void play_tone(float freq, int ms) {
    static int16_t buf[256];
    const size_t total = AUDIO_SAMPLE_RATE * ms / 1000;
    const float attack = AUDIO_SAMPLE_RATE * 0.005f;
    for (size_t off = 0; off < total; off += 256) {
        size_t n = total - off < 256 ? total - off : 256;
        for (size_t i = 0; i < n; i++) {
            size_t k = off + i;
            float t = (float)k / AUDIO_SAMPLE_RATE;
            float env = expf(-t * 9.0f);
            if (k < attack) env *= k / attack;
            buf[i] = (int16_t)(9000.0f * env * sinf(2.0f * (float)M_PI * freq * t));
        }
        esp_codec_dev_write(s_out, buf, n * sizeof(int16_t));
    }
}

static void chime_task(void*) {
    Chime chime;
    for (;;) {
        if (xQueueReceive(s_chime_queue, &chime, portMAX_DELAY) != pdTRUE) continue;
        s_playing = true;
        switch (chime) {
            case Chime::Wake:   // one short blip: the command follows right after it
                play_tone(1318.5f, 120);
                break;
            case Chime::Done:   // falling: "got it"
                play_tone(1318.5f, 110);
                play_tone(880.0f, 260);
                break;
            case Chime::Error:  // low double
                play_tone(330.0f, 160);
                play_tone(330.0f, 260);
                break;
        }
        s_play_end_us = esp_timer_get_time();
        s_playing = false;
    }
}

void audio_init(i2c_master_bus_handle_t bus) {
    i2s_chan_handle_t tx = nullptr, rx = nullptr;
    create_i2s(&tx, &rx);
    create_codecs(bus, tx, rx);
    s_chime_queue = xQueueCreate(4, sizeof(Chime));
    xTaskCreate(chime_task, "chime", 3072, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "audio up (%d Hz, mic gain %.0f dB, volume %d)",
             AUDIO_SAMPLE_RATE, MIC_GAIN_DB, SPEAKER_VOLUME);
}

void audio_read(int16_t* dest, size_t samples) {
    esp_codec_dev_read(s_in, dest, samples * sizeof(int16_t));
}

void audio_play_chime(Chime chime) {
    if (s_sounds) xQueueSend(s_chime_queue, &chime, 0);
}

void audio_set_sounds(bool enabled) {
    s_sounds = enabled;
}

bool audio_sounds_enabled(void) {
    return s_sounds;
}

bool audio_is_playing(int tail_ms) {
    if (s_playing) return true;
    // The speaker keeps ringing briefly and the room echoes; the mic hears it.
    return esp_timer_get_time() - s_play_end_us < (int64_t)tail_ms * 1000;
}
