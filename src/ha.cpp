#include "ha.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "mqtt.h"
#include "secrets.h"

static const char* TAG = "ha";

#define URI_STR2(x) #x
#define URI_STR(x) URI_STR2(x)
#define HA_URI "ws://" HA_HOST ":" URI_STR(HA_PORT) "/api/websocket"

// A wake word run waits indefinitely in practice; HA's defaults (a few
// seconds for the wake word, minutes for the run) are meant for the browser.
#define WAKE_RUN_TIMEOUT_S  3600
// Wake word runs: HA detects "Alexa" a few hundred ms after it was said, so it
// forwards this much audio from before the detection to speech-to-text; that
// way "Alexa Licht an" in one breath keeps its "Licht". The transcript then
// starts with (part of) "Alexa", which strip_wake_word() removes before the
// text goes to intent handling in a separate run.
#define WAKE_BUFFER_S       "1.0"
#define PTT_RUN_TIMEOUT_S   60
#define RETRY_MS            3000
#define MAX_MESSAGE         (32 * 1024)
#define AUDIO_CHUNK_MAX     512   // samples per ha_send_audio() call
#define AUDIO_QUEUE_CHUNKS  8     // 8 x 32 ms = 256 ms of slack for slow sends

// Two pipeline runs can be open at once:
//   standby: always waiting for the wake word, even during a conversation,
//            so a new "Alexa" can interrupt the current one;
//   active:  the conversation in progress (recording / transcribing).
// When the standby hears the wake word it becomes the active run (replacing
// any old one) and a fresh standby is started. The mic audio goes to both.
struct Run {
    int id;          // subscription id; 0 = none
    int handler;     // binary handler id for its audio; -1 until run-start
    bool streaming;  // HA wants audio for it
    bool ptt;        // started without wake word (HA does end-of-speech)
};

// All protocol state lives in ha_task. The WebSocket client holds its own
// lock while calling ws_event, so ws_event only queues work: sending from a
// second task while holding another lock could deadlock against it.
struct Cmd {
    enum Kind { Message, Restart, Disconnected, EndAudio, StartStandby } kind;
    char* json;  // Message: owned by the receiver
    bool ptt;    // Restart
};

static esp_websocket_client_handle_t s_client;
static ha_event_cb_t s_cb;
static QueueHandle_t s_cmds;
static esp_timer_handle_t s_retry_timer;

// ha_task only
static bool s_authed;
static int s_next_id = 1;
static Run s_active;
static Run s_standby;
static int s_intent_id;          // run handling the cleaned-up command text
static int s_lookup_id;          // pending device id lookup (render_template)
static char s_device_id[40];     // our MQTT device in HA; gives commands an area

// Mirrors of the runs' audio handlers for the mic task; -1 = not streaming.
static volatile int s_active_handler = -1;
static volatile int s_standby_handler = -1;

// ws task only: reassembly of messages split across events
static char* s_rx;
static int s_rx_len;

static const char* json_str(cJSON* obj, const char* a, const char* b = nullptr,
                            const char* c = nullptr) {
    const char* path[] = {a, b, c};
    for (const char* key : path) {
        if (!key) break;
        obj = cJSON_GetObjectItem(obj, key);
        if (!obj) return nullptr;
    }
    return cJSON_IsString(obj) ? obj->valuestring : nullptr;
}

static void post(Cmd cmd) {
    if (xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) free(cmd.json);
}

static void send_json(const char* json) {
    esp_websocket_client_send_text(s_client, json, strlen(json), pdMS_TO_TICKS(1000));
}

static void sync_handlers(void) {
    s_active_handler = s_active.streaming ? s_active.handler : -1;
    s_standby_handler = s_standby.streaming ? s_standby.handler : -1;
}

static void unsubscribe(int id) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"id\":%d,\"type\":\"unsubscribe_events\",\"subscription\":%d}",
             s_next_id++, id);
    send_json(buf);
}

// A frame with just the handler id ends a run's audio stream. With HA's own
// speech detection off, a lost end frame leaves the run waiting forever.
static void end_audio(Run& run) {
    if (!run.streaming) return;
    run.streaming = false;
    sync_handlers();
    uint8_t end = (uint8_t)run.handler;
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (esp_websocket_client_send_bin(s_client, (const char*)&end, 1, pdMS_TO_TICKS(1000)) == 1) return;
        ESP_LOGW(TAG, "sending end of audio failed (attempt %d)", attempt);
    }
}

// HA cancels a pipeline run when its subscription is dropped.
static void cancel(Run& run) {
    if (!run.id) return;
    unsubscribe(run.id);
    run = {0, -1, false, false};
    sync_handlers();
}

static void send_run(Run& run, bool ptt) {
    run = {s_next_id++, -1, false, ptt};
    sync_handlers();
    char device[64] = "";
    if (s_device_id[0]) snprintf(device, sizeof(device), ",\"device_id\":\"%s\"", s_device_id);
    char buf[400];
    // Both kinds stop after speech-to-text; the text is cleaned up and sent to
    // intent handling separately (start_intent).
    if (ptt) {
        snprintf(buf, sizeof(buf),
                 "{\"id\":%d,\"type\":\"assist_pipeline/run\",\"pipeline\":\"%s\","
                 "\"start_stage\":\"stt\",\"end_stage\":\"stt\","
                 "\"input\":{\"sample_rate\":%d},\"timeout\":%d%s}",
                 run.id, HA_PIPELINE_ID, AUDIO_SAMPLE_RATE, PTT_RUN_TIMEOUT_S, device);
    } else {
        // timeout 0: no wake word timeout. no_vad: HA's speech detection would
        // take the buffered "Alexa" as the start of the command and end the
        // recording in the pause after it; the board decides instead and ends
        // the audio with ha_end_speech().
        snprintf(buf, sizeof(buf),
                 "{\"id\":%d,\"type\":\"assist_pipeline/run\",\"pipeline\":\"%s\","
                 "\"start_stage\":\"wake_word\",\"end_stage\":\"stt\","
                 "\"input\":{\"sample_rate\":%d,\"timeout\":0,"
                 "\"audio_seconds_to_buffer\":" WAKE_BUFFER_S ",\"no_vad\":true},"
                 "\"timeout\":%d%s}",
                 run.id, HA_PIPELINE_ID, AUDIO_SAMPLE_RATE, WAKE_RUN_TIMEOUT_S, device);
    }
    send_json(buf);
    ESP_LOGI(TAG, "run %d started (%s%s)", run.id, ptt ? "push-to-talk" : "standby",
             s_device_id[0] ? ", with device" : ", no device yet");
}

static void start_standby(void) {
    if (!s_authed) return;
    cancel(s_standby);
    send_run(s_standby, false);
}

// Asks HA for our device's id, then (re)starts the standby run. On first boot
// the MQTT device may not exist yet; it's looked up again after each
// conversation until found.
static void lookup_device_then_standby(void) {
    if (!s_authed) return;
    s_lookup_id = s_next_id++;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"type\":\"render_template\","
             "\"template\":\"{{ device_id('%s') or '' }}\"}",
             s_lookup_id, MQTT_STATUS_ENTITY);
    send_json(buf);
}

static void finish_lookup(const char* device_id) {
    unsubscribe(s_lookup_id);
    s_lookup_id = 0;
    if (device_id && strlen(device_id) < sizeof(s_device_id) && strcmp(device_id, s_device_id)) {
        strcpy(s_device_id, device_id);
        ESP_LOGI(TAG, "device id %s", s_device_id[0] ? s_device_id : "(not found)");
    }
    start_standby();
}

static const char* error_text(const char* code, const char* message) {
    if (!code) return message ? message : "Fehler";
    if (!strcmp(code, "stt-no-text-recognized")) return "Nicht verstanden";
    if (!strcmp(code, "stt-stream-failed")) return "Spracherkennung fehlgeschlagen";
    if (!strcmp(code, "wake-engine-missing") || !strcmp(code, "wake-provider-missing"))
        return "Kein Wake-Word-Dienst in HA";
    if (!strcmp(code, "stt-provider-missing")) return "Keine Spracherkennung in HA";
    if (!strcmp(code, "pipeline-not-found")) return "Pipeline nicht gefunden";
    return message ? message : code;
}

static int edit_distance(const char* a, const char* b) {
    int la = strlen(a), lb = strlen(b);
    if (la > 15 || lb > 15) return 99;
    int d[16][16];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int sub = d[i - 1][j - 1] + (a[i - 1] != b[j - 1]);
            int del = d[i - 1][j] + 1, ins = d[i][j - 1] + 1;
            d[i][j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
        }
    return d[la][lb];
}

// "alexa", a tail of it ("xa", "lexa") as cut off by the audio buffer, or a
// misspelling Whisper might produce ("Alexia", "Aleksa").
static bool is_wake_fragment(const char* w) {
    static const char* WAKE = "alexa";
    int len = strlen(w);
    if (len >= 2 && len <= 5 && !strcmp(WAKE + 5 - len, w)) return true;
    return len >= 4 && edit_distance(w, WAKE) <= 2;
}

// Returns a pointer into `text` past leading wake words ("Alexa", "Hey Alexa",
// or "Alexa, Alexa" after an interruption) and the punctuation after them.
static const char* strip_wake_word(const char* text) {
    const char* p = text;
    for (int words = 0; words < 3; words++) {
        while (*p == ' ' || *p == ',' || *p == '.' || *p == '!' || *p == '?') p++;
        char w[16];
        int n = 0;
        const char* q = p;
        while (((*q | 0x20) >= 'a' && (*q | 0x20) <= 'z') && n < 15) w[n++] = *q++ | 0x20;
        w[n] = '\0';
        if (n == 0 || (*q & 0x80) || !(is_wake_fragment(w) || (words == 0 && !strcmp(w, "hey"))))
            break;
        p = q;
    }
    while (*p == ' ' || *p == ',' || *p == '.' || *p == '!' || *p == '?' || *p == '-') p++;
    return p;
}

static void json_escape(char* out, size_t cap, const char* in) {
    size_t o = 0;
    for (; *in && o + 7 < cap; in++) {
        unsigned char c = *in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c < 0x20) o += snprintf(out + o, cap - o, "\\u%04x", c);
        else out[o++] = c;
    }
    out[o] = '\0';
}

// Sends the cleaned-up command through intent handling as the device, so
// area-aware commands ("Licht an") work.
static void start_intent(const char* text) {
    char esc[400], buf[640], device[64] = "";
    json_escape(esc, sizeof(esc), text);
    if (s_device_id[0]) snprintf(device, sizeof(device), ",\"device_id\":\"%s\"", s_device_id);
    s_intent_id = s_next_id++;
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"type\":\"assist_pipeline/run\",\"pipeline\":\"%s\","
             "\"start_stage\":\"intent\",\"end_stage\":\"intent\",\"input\":{\"text\":\"%s\"}%s}",
             s_intent_id, HA_PIPELINE_ID, esc, device);
    send_json(buf);
}

static void handle_intent_event(const char* type, cJSON* data) {
    if (!strcmp(type, "intent-end")) {
        cJSON* resp = cJSON_GetObjectItem(cJSON_GetObjectItem(data, "intent_output"), "response");
        const char* speech = json_str(resp, "speech", "plain", "speech");
        const char* rtype = json_str(resp, "response_type");
        ESP_LOGI(TAG, "response [%s]: '%s'", rtype ? rtype : "?", speech ? speech : "");
        bool failed = rtype && !strcmp(rtype, "error");
        s_cb(failed ? HaEvent::Error : HaEvent::Response, speech ? speech : "");
    } else if (!strcmp(type, "error")) {
        s_cb(HaEvent::Error, error_text(json_str(data, "code"), json_str(data, "message")));
    } else if (!strcmp(type, "run-end")) {
        s_intent_id = 0;
    }
}

static void handle_standby_event(const char* type, cJSON* data) {
    if (!strcmp(type, "run-start")) {
        cJSON* h = cJSON_GetObjectItem(cJSON_GetObjectItem(data, "runner_data"),
                                       "stt_binary_handler_id");
        if (cJSON_IsNumber(h)) {
            s_standby.handler = h->valueint;
            s_standby.streaming = true;
            sync_handlers();
        }
    } else if (!strcmp(type, "wake_word-end")) {
        const char* word = json_str(data, "wake_word_output", "wake_word_id");
        if (!word) return;
        ESP_LOGI(TAG, "wake word '%s'%s", word, s_active.id ? ", replacing the current conversation" : "");
        // This run carries on into speech-to-text as the new conversation.
        cancel(s_active);
        s_intent_id = 0;  // an answer still on its way belongs to the old one
        s_active = s_standby;
        s_standby = {0, -1, false, false};
        start_standby();
        s_cb(HaEvent::WakeWord, word);
    } else if (!strcmp(type, "error")) {
        const char* code = json_str(data, "code");
        ESP_LOGW(TAG, "standby error %s", code ? code : "?");
        // Timeouts and HA's duplicate wake-up guard are routine; anything else
        // (e.g. the wake word add-on being down) is shown.
        bool routine = code && (!strcmp(code, "timeout") || !strcmp(code, "wake-word-timeout") ||
                                !strcmp(code, "duplicate_wake_up_detected"));
        if (!routine) s_cb(HaEvent::Error, error_text(code, json_str(data, "message")));
        s_standby.streaming = false;
        sync_handlers();
    } else if (!strcmp(type, "run-end")) {
        // Ended without detecting a wake word (error/timeout): try again shortly.
        s_standby = {0, -1, false, false};
        sync_handlers();
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, 500 * 1000);
    }
}

static void handle_active_event(const char* type, cJSON* data) {
    if (!strcmp(type, "run-start")) {  // push-to-talk runs start here
        cJSON* h = cJSON_GetObjectItem(cJSON_GetObjectItem(data, "runner_data"),
                                       "stt_binary_handler_id");
        if (cJSON_IsNumber(h)) {
            s_active.handler = h->valueint;
            s_active.streaming = true;
            sync_handlers();
        }
    } else if (!strcmp(type, "stt-start")) {
        if (s_active.ptt) s_cb(HaEvent::Listening, nullptr);
    } else if (!strcmp(type, "stt-vad-end")) {
        s_cb(HaEvent::SpeechEnded, nullptr);
    } else if (!strcmp(type, "stt-end")) {
        end_audio(s_active);
        const char* text = json_str(data, "stt_output", "text");
        const char* command = strip_wake_word(text ? text : "");
        ESP_LOGI(TAG, "heard: '%s' -> command '%s'", text ? text : "", command);
        if (!command[0]) {
            s_cb(HaEvent::Error, "Nicht verstanden");
        } else {
            s_cb(HaEvent::Transcript, command);
            start_intent(command);
        }
    } else if (!strcmp(type, "error")) {
        end_audio(s_active);
        const char* code = json_str(data, "code");
        const char* message = json_str(data, "message");
        ESP_LOGW(TAG, "pipeline error %s: %s", code ? code : "?", message ? message : "");
        if (!code || strcmp(code, "timeout")) s_cb(HaEvent::Error, error_text(code, message));
    } else if (!strcmp(type, "run-end")) {
        end_audio(s_active);
        s_active = {0, -1, false, false};
        sync_handlers();
        if (!s_device_id[0]) lookup_device_then_standby();
    }
}

static void handle_message(const char* json) {
    cJSON* root = cJSON_Parse(json);
    if (!root) return;
    const char* type = json_str(root, "type");
    cJSON* idj = cJSON_GetObjectItem(root, "id");
    int id = cJSON_IsNumber(idj) ? idj->valueint : 0;

    if (!type) {
    } else if (!strcmp(type, "auth_required")) {
        char buf[300];
        snprintf(buf, sizeof(buf), "{\"type\":\"auth\",\"access_token\":\"%s\"}", HA_TOKEN);
        send_json(buf);
    } else if (!strcmp(type, "auth_ok")) {
        ESP_LOGI(TAG, "authenticated");
        s_authed = true;
        s_active = s_standby = {0, -1, false, false};
        sync_handlers();
        lookup_device_then_standby();
        s_cb(HaEvent::Ready, nullptr);
    } else if (!strcmp(type, "auth_invalid")) {
        ESP_LOGE(TAG, "token rejected");
        s_cb(HaEvent::Error, "HA-Token ung\xC3\xBCltig");
    } else if (!strcmp(type, "result")) {
        if (!cJSON_IsFalse(cJSON_GetObjectItem(root, "success")) || !id) {
        } else if (id == s_lookup_id) {
            finish_lookup(nullptr);
        } else if (id == s_standby.id || id == s_active.id) {
            const char* code = json_str(root, "error", "code");
            const char* message = json_str(root, "error", "message");
            ESP_LOGE(TAG, "run %d rejected: %s %s", id, code ? code : "", message ? message : "");
            s_cb(HaEvent::Error, error_text(code, message));
            if (id == s_standby.id) {
                s_standby = {0, -1, false, false};
                esp_timer_stop(s_retry_timer);
                esp_timer_start_once(s_retry_timer, RETRY_MS * 1000);
            } else {
                s_active = {0, -1, false, false};
            }
            sync_handlers();
        }
    } else if (!strcmp(type, "event")) {
        cJSON* event = cJSON_GetObjectItem(root, "event");
        const char* etype = json_str(event, "type");
        cJSON* data = cJSON_GetObjectItem(event, "data");
        if (!id) {
        } else if (id == s_lookup_id) {
            finish_lookup(json_str(event, "result"));
        } else if (etype && id == s_intent_id) {
            handle_intent_event(etype, data);
        } else if (etype && id == s_standby.id) {
            handle_standby_event(etype, data);
        } else if (etype && id == s_active.id) {
            handle_active_event(etype, data);
        }  // anything else: late events from cancelled runs
    }
    cJSON_Delete(root);
}

static void ha_task(void*) {
    Cmd cmd;
    for (;;) {
        if (xQueueReceive(s_cmds, &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd.kind) {
            case Cmd::Message:
                handle_message(cmd.json);
                free(cmd.json);
                break;
            case Cmd::Restart:  // cancel the conversation; maybe start push-to-talk
                if (!s_authed) break;
                cancel(s_active);
                s_intent_id = 0;
                if (cmd.ptt) send_run(s_active, true);
                if (!s_standby.id) start_standby();
                break;
            case Cmd::EndAudio:
                end_audio(s_active);
                break;
            case Cmd::StartStandby:
                if (!s_standby.id) start_standby();
                break;
            case Cmd::Disconnected:
                s_active = s_standby = {0, -1, false, false};
                sync_handlers();
                s_lookup_id = 0;
                s_intent_id = 0;
                if (s_authed) {
                    s_authed = false;
                    ESP_LOGW(TAG, "connection lost");
                    s_cb(HaEvent::Offline, nullptr);
                }
                break;
        }
    }
}

static void ws_event(void*, esp_event_base_t, int32_t event_id, void* event_data) {
    auto* d = (esp_websocket_event_data_t*)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected to %s", HA_URI);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            // Stop the mic task sending into a dead socket right away.
            s_active_handler = -1;
            s_standby_handler = -1;
            post({Cmd::Disconnected, nullptr, false});
            break;
        case WEBSOCKET_EVENT_DATA:
            if (d->op_code != 0x1 && d->op_code != 0x0) break;  // text frames only
            if (d->payload_offset == 0) {
                free(s_rx);
                s_rx = nullptr;
                s_rx_len = d->payload_len;
                if (s_rx_len > MAX_MESSAGE) {
                    ESP_LOGW(TAG, "skipping %d-byte message", s_rx_len);
                    break;
                }
                s_rx = (char*)malloc(s_rx_len + 1);
            }
            if (!s_rx || d->payload_offset + d->data_len > s_rx_len) break;
            memcpy(s_rx + d->payload_offset, d->data_ptr, d->data_len);
            if (d->payload_offset + d->data_len == s_rx_len) {
                s_rx[s_rx_len] = '\0';
                post({Cmd::Message, s_rx, false});  // ha_task frees it
                s_rx = nullptr;
            }
            break;
        default:
            break;
    }
}

// Audio goes through a queue to its own task, so a slow WebSocket send never
// stalls the mic task: if that missed its I2S deadline, the driver would drop
// samples and HA would get choppy audio.
struct AudioChunk {
    uint16_t count;
    int16_t samples[AUDIO_CHUNK_MAX];
};

static QueueHandle_t s_audio_q;
static volatile uint32_t s_audio_dropped;

// The same chunk goes to every run that wants audio: during a conversation
// that's the active run and the standby listening for a new "Alexa".
static void sender_task(void*) {
    static AudioChunk chunk;
    static uint8_t frame[1 + AUDIO_CHUNK_MAX * sizeof(int16_t)];
    uint32_t reported = 0;
    for (;;) {
        if (xQueueReceive(s_audio_q, &chunk, portMAX_DELAY) != pdTRUE) continue;
        size_t bytes = chunk.count * sizeof(int16_t);
        memcpy(frame + 1, chunk.samples, bytes);
        int handlers[2] = {s_active_handler, s_standby_handler};
        for (int h : handlers) {
            if (h < 0) continue;
            frame[0] = (uint8_t)h;
            esp_websocket_client_send_bin(s_client, (const char*)frame, 1 + bytes, pdMS_TO_TICKS(500));
        }
        if (s_audio_dropped != reported) {
            reported = s_audio_dropped;
            ESP_LOGW(TAG, "network too slow: %u audio chunks dropped so far", (unsigned)reported);
        }
    }
}

static void retry_cb(void*) {
    post({Cmd::StartStandby, nullptr, false});
}

void ha_init(ha_event_cb_t cb) {
    s_cb = cb;
    s_active = s_standby = {0, -1, false, false};
    s_cmds = xQueueCreate(16, sizeof(Cmd));
    xTaskCreate(ha_task, "ha", 6144, nullptr, 4, nullptr);
    s_audio_q = xQueueCreate(AUDIO_QUEUE_CHUNKS, sizeof(AudioChunk));
    xTaskCreate(sender_task, "ha_audio", 3072, nullptr, 5, nullptr);

    esp_timer_create_args_t targs = {};
    targs.callback = retry_cb;
    targs.name = "ha_retry";
    esp_timer_create(&targs, &s_retry_timer);

    esp_websocket_client_config_t cfg = {};
    cfg.uri = HA_URI;
    cfg.buffer_size = 2048;
    cfg.task_stack = 4096;
    cfg.task_prio = 5;
    cfg.reconnect_timeout_ms = 3000;
    cfg.network_timeout_ms = 5000;
    s_client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event, nullptr);
    esp_websocket_client_start(s_client);
}

void ha_restart(bool push_to_talk) {
    post({Cmd::Restart, nullptr, push_to_talk});
}

void ha_end_speech(void) {
    post({Cmd::EndAudio, nullptr, false});
}

bool ha_wants_audio(void) {
    return s_active_handler >= 0 || s_standby_handler >= 0;
}

void ha_send_audio(const int16_t* samples, size_t count) {
    static AudioChunk chunk;
    if (count > AUDIO_CHUNK_MAX || !ha_wants_audio()) return;
    chunk.count = count;
    memcpy(chunk.samples, samples, count * sizeof(int16_t));
    if (xQueueSend(s_audio_q, &chunk, 0) != pdTRUE) {
        AudioChunk old;
        xQueueReceive(s_audio_q, &old, 0);  // drop the oldest rather than the newest
        xQueueSend(s_audio_q, &chunk, 0);
        s_audio_dropped = s_audio_dropped + 1;
    }
}
