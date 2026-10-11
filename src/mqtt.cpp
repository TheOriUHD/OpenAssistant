#include "mqtt.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <mqtt_client.h>
#include <stdio.h>
#include <string.h>

#include "secrets.h"

static const char* TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static mqtt_mute_cb_t s_on_mute;
static bool s_connected;
static char s_id[13];                 // MAC as hex, unique per board
static char s_base[40];               // voice_panel/<id>
static char s_client_id[32];          // voice_panel_<id>
static char s_avail_topic[64];
static char s_mute_cmd_topic[64];
// Last values, re-sent after a reconnect so HA is never stale.
static char s_state[16] = "offline";
static bool s_muted;

static void publish(const char* suffix, const char* payload, bool retain) {
    if (!s_connected) return;
    char topic[80];
    snprintf(topic, sizeof(topic), "%s/%s", s_base, suffix);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, retain);
}

static void publish_discovery(void) {
    char dev[220];
    snprintf(dev, sizeof(dev),
             "\"dev\":{\"ids\":[\"voice_panel_%s\"],\"name\":\"Voice Panel\","
             "\"mf\":\"Waveshare\",\"mdl\":\"ESP32-C6-Touch-AMOLED-2.16\",\"sw\":\"0.1.0\"}",
             s_id);
    char topic[96], payload[640];

    snprintf(topic, sizeof(topic), "homeassistant/sensor/voice_panel_%s/status/config", s_id);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Status\",\"uniq_id\":\"voice_panel_%s_status\",\"ic\":\"mdi:microphone\","
             "\"stat_t\":\"%s/state\",\"avty_t\":\"%s\",%s}",
             s_id, s_base, s_avail_topic, dev);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);

    snprintf(topic, sizeof(topic), "homeassistant/switch/voice_panel_%s/mute/config", s_id);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Stumm\",\"uniq_id\":\"voice_panel_%s_mute\",\"ic\":\"mdi:microphone-off\","
             "\"stat_t\":\"%s/mute\",\"cmd_t\":\"%s\",\"avty_t\":\"%s\",%s}",
             s_id, s_base, s_mute_cmd_topic, s_avail_topic, dev);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
}

static void on_event(void*, esp_event_base_t, int32_t id, void* data) {
    auto* ev = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected, announcing device voice_panel_%s", s_id);
            s_connected = true;
            publish_discovery();
            esp_mqtt_client_publish(s_client, s_avail_topic, "online", 0, 1, true);
            esp_mqtt_client_subscribe(s_client, s_mute_cmd_topic, 1);
            publish("state", s_state, true);
            publish("mute", s_muted ? "ON" : "OFF", true);
            break;
        case MQTT_EVENT_DISCONNECTED:
            if (s_connected) ESP_LOGW(TAG, "disconnected");
            s_connected = false;
            break;
        case MQTT_EVENT_DATA:
            if (ev->topic_len == (int)strlen(s_mute_cmd_topic) &&
                !strncmp(ev->topic, s_mute_cmd_topic, ev->topic_len) && s_on_mute) {
                s_on_mute(ev->data_len == 2 && !strncmp(ev->data, "ON", 2));
            }
            break;
        case MQTT_EVENT_ERROR:
            if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
                ESP_LOGE(TAG, "broker refused login (code %d)", ev->error_handle->connect_return_code);
            break;
        default:
            break;
    }
}

void mqtt_init(mqtt_mute_cb_t on_mute) {
    s_on_mute = on_mute;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_id, sizeof(s_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_base, sizeof(s_base), "voice_panel/%s", s_id);
    snprintf(s_client_id, sizeof(s_client_id), "voice_panel_%s", s_id);
    snprintf(s_avail_topic, sizeof(s_avail_topic), "%s/availability", s_base);
    snprintf(s_mute_cmd_topic, sizeof(s_mute_cmd_topic), "%s/mute/set", s_base);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = MQTT_URI;
    cfg.credentials.username = MQTT_USER;
    cfg.credentials.authentication.password = MQTT_PASSWORD;
    cfg.credentials.client_id = s_client_id;
    cfg.session.last_will.topic = s_avail_topic;
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    cfg.task.stack_size = 4096;
    cfg.buffer.size = 1024;
    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_event, nullptr);
    esp_mqtt_client_start(s_client);
}

void mqtt_publish_state(const char* state) {
    strncpy(s_state, state, sizeof(s_state) - 1);
    publish("state", s_state, true);
}

void mqtt_publish_mute(bool muted) {
    s_muted = muted;
    publish("mute", muted ? "ON" : "OFF", true);
}
