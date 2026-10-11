#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>
#include <nvs.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "board.h"
#include "display.h"
#include "ha.h"
#include "mqtt.h"
#include "pmu.h"
#include "ui.h"
#include "wifi.h"

static const char* TAG = "main";

#define CHUNK_SAMPLES   512    // 32 ms of audio per WebSocket frame
#define RESULT_TEXT_MS  5000   // how long HA's answer stays on screen
#define ERROR_TEXT_MS   3000
#define CHIME_ECHO_MS   150    // silence sent after a chime so HA doesn't hear its echo

// End of command after the wake word (the board decides; see ha.cpp):
#define END_SILENCE_MS  700    // this much quiet after speech ends it
#define NO_SPEECH_MS    3000   // give up if nothing is said after "Alexa"
#define MAX_COMMAND_MS  8000
#define SPEECH_FRAMES   2      // 64 ms above the threshold counts as speech
#define BURST_GAP_MS    300    // a shorter pause doesn't split a stretch of speech
#define ONGOING_MS      150    // speech this recent when HA reports the wake word = still talking
#define WHOLE_COMMAND_MS 900   // "Alexa" alone is ~0.5 s; a stretch this long includes a command

// Everything the main loop reacts to. HA events arrive from the HA task with a
// copy of their text; the main loop frees it.
struct AppEvent {
    enum Kind { Tap, Boot, Key, Ha, MuteOn, MuteOff } kind;
    HaEvent ha;
    char* text;
};

static QueueHandle_t s_events;
static volatile bool s_muted;
static volatile bool s_detect_end;  // after a wake word: board watches for end of command
static volatile uint32_t s_command_gen;  // bumped per wake word, so a new one restarts that watch
static UiState s_state = UiState::Offline;

static void post(AppEvent ev) {
    if (xQueueSend(s_events, &ev, pdMS_TO_TICKS(50)) != pdTRUE) free(ev.text);
}

static void on_tap(void) {
    post({AppEvent::Tap, HaEvent::Ready, nullptr});
}

static void on_ha_event(HaEvent ev, const char* text) {
    post({AppEvent::Ha, ev, text ? strdup(text) : nullptr});
}

static void on_mqtt_mute(bool muted) {
    post({muted ? AppEvent::MuteOn : AppEvent::MuteOff, HaEvent::Ready, nullptr});
}

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void set_state(UiState state) {
    static const char* names[] = {"idle", "listening", "thinking", "muted", "error", "offline"};
    ESP_LOGI(TAG, "state -> %s", names[(int)state]);
    s_state = state;
    ui_set_state(state);
    mqtt_publish_state(names[(int)state]);
}

static void set_muted(bool muted) {
    s_muted = muted;
    mqtt_publish_mute(muted);
}

static float rms(const int16_t* buf, size_t n) {
    double acc = 0;
    for (size_t i = 0; i < n; i++) acc += (double)buf[i] * buf[i];
    return sqrtf((float)(acc / n));
}

static float speech_threshold(float noise) {
    float threshold = noise * 2.5f;
    if (threshold < 300) threshold = 300;
    if (threshold > 3000) threshold = 3000;
    return threshold;
}

// Streams the mic to HA whenever a pipeline run wants audio and we're not
// muted. After a wake word it also decides when the command is over.
static void mic_task(void*) {
    int16_t* buf = (int16_t*)heap_caps_malloc(CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    float noise = 200.0f;  // the room's background level, learned while nobody talks
    bool tracking = false;
    int64_t start = 0, last_speech = 0, last_loud = 0;
    int64_t burst_start = 0;  // start of the latest stretch of continuous speech
    int speech_frames = 0;
    float peak = 0;
    uint32_t gen = 0;

    for (;;) {
        audio_read(buf, CHUNK_SAMPLES);
        float level = rms(buf, CHUNK_SAMPLES);
        bool chime = audio_is_playing(CHIME_ECHO_MS);
        bool detect = s_detect_end;
        int64_t t = now_ms();
        float threshold = speech_threshold(noise);
        bool loud = !chime && level > threshold;  // our blip and its echo aren't speech
        if (loud) {
            if (t - last_loud > BURST_GAP_MS) burst_start = t;
            last_loud = t;
        }

        if (!detect) {
            tracking = false;
            // Only quiet frames teach the background level, so talking
            // (including the wake word itself) doesn't raise the threshold.
            if (!chime && !loud) noise += (level - noise) * 0.02f;
        } else {
            if (!tracking || gen != s_command_gen) {
                tracking = true;
                gen = s_command_gen;
                start = t;
                peak = 0;
                // HA reports the wake word late, by a varying few hundred ms.
                // What was said up to now tells which case this is:
                //  - still talking: "Alexa Licht an" in one breath, ongoing;
                //  - a long stretch that just ended: the whole command was
                //    said in one breath and is already in HA's audio buffer;
                //  - only a short burst ("Alexa"): wait for the command.
                // In the first two cases the usual silence ends the command.
                int64_t since = t - last_loud, burst = last_loud - burst_start;
                const char* kind;
                if (since < ONGOING_MS) kind = "still talking";
                else if (burst >= WHOLE_COMMAND_MS && since < 2000) kind = "said in one breath";
                else kind = nullptr;
                speech_frames = kind ? SPEECH_FRAMES : 0;
                last_speech = kind ? last_loud : 0;
                ESP_LOGI(TAG, "wake word: last speech %lld ms ago, %lld ms long -> %s",
                         since, burst, kind ? kind : "waiting for the command");
            }
            if (!chime && level > peak) peak = level;
            if (loud) {
                speech_frames++;
                last_speech = t;
            }
            bool spoke = speech_frames >= SPEECH_FRAMES;
            const char* why = nullptr;
            if (spoke && t - last_speech > END_SILENCE_MS) why = "silence";
            else if (!spoke && t - start > NO_SPEECH_MS) why = "nothing said";
            else if (t - start > MAX_COMMAND_MS) why = "too long";
            if (why) {
                ESP_LOGI(TAG, "command over (%s) after %lld ms: peak level %.0f, threshold %.0f, %d speech frames",
                         why, t - start, peak, threshold, speech_frames);
                s_detect_end = false;
                tracking = false;
                ha_end_speech();
                post({AppEvent::Ha, HaEvent::SpeechEnded, nullptr});
            }
        }

        if (s_muted || !ha_wants_audio()) continue;
        // During a wake word command the real audio goes out even under the
        // blip: zeroing it would cut words spoken straight after "Alexa".
        if (chime && !detect) memset(buf, 0, CHUNK_SAMPLES * sizeof(int16_t));
        ha_send_audio(buf, CHUNK_SAMPLES);
    }
}

// BOOT: push-to-talk. KEY: sounds on/off. Both active low; a press is
// reported on the falling edge.
static void button_task(void*) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << BOOT_BUTTON) | (1ULL << KEY_BUTTON);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&cfg);
    int last_boot = 1, last_key = 1;
    for (;;) {
        int boot = gpio_get_level(BOOT_BUTTON), key = gpio_get_level(KEY_BUTTON);
        if (last_boot == 1 && boot == 0) post({AppEvent::Boot, HaEvent::Ready, nullptr});
        if (last_key == 1 && key == 0) post({AppEvent::Key, HaEvent::Ready, nullptr});
        last_boot = boot;
        last_key = key;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// The sounds setting survives restarts (NVS is initialised by wifi_init).
static void load_sounds_setting(void) {
    nvs_handle_t nvs;
    uint8_t on = 1;
    if (nvs_open("panel", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "sounds", &on);
        nvs_close(nvs);
    }
    audio_set_sounds(on);
    ESP_LOGI(TAG, "sounds %s", on ? "on" : "off");
}

static void save_sounds_setting(bool on) {
    nvs_handle_t nvs;
    if (nvs_open("panel", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, "sounds", on);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void log_memory(void) {
    ESP_LOGI(TAG, "heap: %u B free, %u B min, largest block %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

extern "C" void app_main(void) {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = BOARD_I2C_SDA;
    bus_cfg.scl_io_num = BOARD_I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    i2c_master_bus_handle_t bus = nullptr;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    pmu_init(bus);
    lv_display_t* disp = display_init(bus);
    s_events = xQueueCreate(16, sizeof(AppEvent));
    ui_init(disp, on_tap);
    set_state(UiState::Offline);
    audio_init(bus);
    log_memory();

    wifi_init();
    load_sounds_setting();
    mqtt_init(on_mqtt_mute);
    ha_init(on_ha_event);
    log_memory();

    xTaskCreate(mic_task, "mic", 4096, nullptr, 5, nullptr);
    xTaskCreate(button_task, "button", 2048, nullptr, 2, nullptr);

    // Event handling is brief; it must not wait behind LVGL rendering.
    vTaskPrioritySet(nullptr, 4);

    int64_t last_mem_log = now_ms();
    int64_t text_until = 0;  // when the answer/error text gives way to the idle screen
    for (;;) {
        AppEvent ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(50)) == pdTRUE) {
            switch (ev.kind) {
                case AppEvent::Tap:
                    if (s_state == UiState::Listening || s_state == UiState::Thinking) {
                        s_detect_end = false;
                        ha_restart(false);  // cancel this conversation
                        set_state(UiState::Idle);
                    } else if (s_state == UiState::Idle || s_state == UiState::Error) {
                        text_until = 0;
                        set_muted(true);
                        set_state(UiState::Muted);
                    } else if (s_state == UiState::Muted) {
                        set_muted(false);
                        set_state(UiState::Idle);
                    }
                    break;

                case AppEvent::Key: {  // sounds on/off
                    bool on = !audio_sounds_enabled();
                    audio_set_sounds(on);
                    save_sounds_setting(on);
                    ESP_LOGI(TAG, "sounds %s", on ? "on" : "off");
                    ui_show_sounds_toast(on);
                    if (on) audio_play_chime(Chime::Done);  // so you hear they're back
                    break;
                }

                case AppEvent::MuteOn:  // mute switch flipped in HA
                    s_detect_end = false;
                    if (s_state == UiState::Listening || s_state == UiState::Thinking) ha_restart(false);
                    text_until = 0;
                    set_muted(true);
                    if (s_state != UiState::Offline) set_state(UiState::Muted);
                    break;

                case AppEvent::MuteOff:
                    set_muted(false);
                    if (s_state == UiState::Muted) set_state(UiState::Idle);
                    break;

                case AppEvent::Boot:  // push-to-talk, no wake word needed
                    if (s_state == UiState::Idle || s_state == UiState::Error) {
                        text_until = 0;
                        s_detect_end = false;  // HA's own end-of-speech detection handles these
                        ha_restart(true);
                        audio_play_chime(Chime::Wake);
                        set_state(UiState::Listening);
                    }
                    break;

                case AppEvent::Ha:
                    switch (ev.ha) {
                        case HaEvent::Ready:
                            set_state(s_muted ? UiState::Muted : UiState::Idle);
                            break;
                        case HaEvent::Offline:
                            s_detect_end = false;
                            set_state(UiState::Offline);
                            break;
                        case HaEvent::WakeWord:  // also interrupts a conversation in progress
                            if (s_state != UiState::Muted && s_state != UiState::Offline) {
                                text_until = 0;
                                s_command_gen = s_command_gen + 1;
                                s_detect_end = true;
                                audio_play_chime(Chime::Wake);
                                set_state(UiState::Listening);
                            }
                            break;
                        case HaEvent::Listening:
                            break;  // push-to-talk already switched to Listening
                        case HaEvent::SpeechEnded:
                            if (s_state == UiState::Listening) set_state(UiState::Thinking);
                            break;
                        case HaEvent::Transcript:
                            if (s_state == UiState::Listening) set_state(UiState::Thinking);
                            if (s_state == UiState::Thinking && ev.text && ev.text[0])
                                ui_set_text(ev.text);
                            break;
                        case HaEvent::Response:
                            if (s_state == UiState::Thinking || s_state == UiState::Listening) {
                                audio_play_chime(Chime::Done);
                                set_state(UiState::Idle);
                                ui_set_text(ev.text && ev.text[0] ? ev.text : "OK");
                                text_until = now_ms() + RESULT_TEXT_MS;
                            }
                            break;
                        case HaEvent::Error:
                            s_detect_end = false;
                            if (s_state != UiState::Muted && s_state != UiState::Offline) {
                                audio_play_chime(Chime::Error);
                                set_state(UiState::Error);
                                ui_set_text(ev.text ? ev.text : "Fehler");
                                text_until = now_ms() + ERROR_TEXT_MS;
                            }
                            break;
                    }
                    break;
            }
            free(ev.text);
        }

        int64_t t = now_ms();
        if (text_until && t >= text_until) {
            text_until = 0;
            if (s_state == UiState::Idle || s_state == UiState::Error) set_state(UiState::Idle);
        }
        if (t - last_mem_log > 30000) {
            log_memory();
            last_mem_log = t;
        }
    }
}
