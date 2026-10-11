#include "ui.h"

#include <esp_lvgl_port.h>

#include "display.h"

LV_FONT_DECLARE(font_ui_28);
LV_FONT_DECLARE(font_icons_96);

#define ICON_MIC       "\xEF\x84\xB0"  // FontAwesome U+F130
#define ICON_MIC_SLASH "\xEF\x84\xB1"  // FontAwesome U+F131
#define ICON_VOLUME    "\xEF\x80\xA8"  // FontAwesome U+F028
#define ICON_MUTE      "\xEF\x9A\xA9"  // FontAwesome U+F6A9

#define TOAST_IN_MS    220
#define TOAST_HOLD_MS  1400
#define TOAST_OUT_MS   350

#define BUBBLE_SIZE    220
#define BUBBLE_PULSE   250

// AMOLED: black pixels are off, so keep the idle screen mostly black and
// dim to limit burn-in.
#define BRIGHTNESS_IDLE   110
#define BRIGHTNESS_ACTIVE 255

static const lv_color_t COLOR_IDLE_FILL   = lv_color_hex(0x15171C);
static const lv_color_t COLOR_IDLE_RING   = lv_color_hex(0x2B3A55);
static const lv_color_t COLOR_IDLE_ICON   = lv_color_hex(0x5C6B85);
static const lv_color_t COLOR_LISTEN      = lv_color_hex(0x2F80FF);
static const lv_color_t COLOR_THINK       = lv_color_hex(0x8B5CF6);
static const lv_color_t COLOR_MUTED       = lv_color_hex(0xD32F2F);
static const lv_color_t COLOR_ERROR       = lv_color_hex(0xF57C00);
static const lv_color_t COLOR_TEXT        = lv_color_hex(0xE6E9EF);
static const lv_color_t COLOR_TEXT_DIM    = lv_color_hex(0x7A8394);

static lv_obj_t* s_bubble;
static lv_obj_t* s_icon;
static lv_obj_t* s_text;
static lv_obj_t* s_toast;
static lv_obj_t* s_toast_icon;
static lv_obj_t* s_toast_text;
static ui_tap_cb_t s_on_tap;

static void size_anim_cb(void* obj, int32_t v) {
    lv_obj_set_size((lv_obj_t*)obj, v, v);
}

static void ring_anim_cb(void* obj, int32_t v) {
    lv_obj_set_style_border_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}

static void stop_anims(void) {
    lv_anim_delete(s_bubble, size_anim_cb);
    lv_anim_delete(s_bubble, ring_anim_cb);
    lv_obj_set_size(s_bubble, BUBBLE_SIZE, BUBBLE_SIZE);
    lv_obj_set_style_border_opa(s_bubble, LV_OPA_COVER, 0);
}

static void start_pulse(int32_t period_ms) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_bubble);
    lv_anim_set_exec_cb(&a, size_anim_cb);
    lv_anim_set_values(&a, BUBBLE_SIZE, BUBBLE_PULSE);
    lv_anim_set_duration(&a, period_ms);
    lv_anim_set_playback_duration(&a, period_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void start_ring_breathe(int32_t period_ms) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_bubble);
    lv_anim_set_exec_cb(&a, ring_anim_cb);
    lv_anim_set_values(&a, LV_OPA_20, LV_OPA_COVER);
    lv_anim_set_duration(&a, period_ms);
    lv_anim_set_playback_duration(&a, period_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void style_bubble(lv_color_t fill, lv_color_t ring, lv_color_t icon_color,
                         const char* icon) {
    lv_obj_set_style_bg_color(s_bubble, fill, 0);
    lv_obj_set_style_border_color(s_bubble, ring, 0);
    lv_label_set_text(s_icon, icon);
    lv_obj_set_style_text_color(s_icon, icon_color, 0);
}

static void screen_clicked(lv_event_t*) {
    if (s_on_tap) s_on_tap();
}

// The toast fades by animating its background, border and text opacity
// separately. Setting opa on the whole object would make LVGL render it
// through a temporary layer buffer: the same large-allocation trouble that
// the bubble's shadow caused.
static void toast_fade_cb(void* obj, int32_t v) {
    lv_obj_set_style_bg_opa((lv_obj_t*)obj, (lv_opa_t)(v * 235 / 255), 0);
    lv_obj_set_style_border_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
    lv_obj_set_style_text_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}

// Same as toast_fade_cb; a separate function so the delayed fade-out isn't
// treated as a duplicate of the fade-in.
static void toast_fade_out_cb(void* obj, int32_t v) {
    toast_fade_cb(obj, v);
}

static void toast_slide_cb(void* obj, int32_t v) {
    lv_obj_set_style_translate_y((lv_obj_t*)obj, v, 0);
}

static void toast_hidden_cb(lv_anim_t* a) {
    lv_obj_add_flag((lv_obj_t*)a->var, LV_OBJ_FLAG_HIDDEN);
}

static void create_toast(void) {
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(s_toast, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, 76);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_radius(s_toast, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x1C2029), 0);
    lv_obj_set_style_border_width(s_toast, 2, 0);
    lv_obj_set_style_pad_hor(s_toast, 30, 0);
    lv_obj_set_style_pad_ver(s_toast, 0, 0);
    lv_obj_set_style_pad_column(s_toast, 16, 0);
    lv_obj_set_flex_flow(s_toast, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_toast, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_toast_icon = lv_label_create(s_toast);
    lv_obj_set_style_text_font(s_toast_icon, &font_ui_28, 0);
    s_toast_text = lv_label_create(s_toast);
    lv_obj_set_style_text_font(s_toast_text, &font_ui_28, 0);
    lv_obj_set_style_text_color(s_toast_text, COLOR_TEXT, 0);
}

void ui_show_sounds_toast(bool on) {
    const lv_color_t accent = on ? lv_color_hex(0x34C759) : lv_color_hex(0x8E95A3);
    lvgl_port_lock(0);
    lv_anim_delete(s_toast, toast_fade_cb);
    lv_anim_delete(s_toast, toast_fade_out_cb);
    lv_anim_delete(s_toast, toast_slide_cb);
    lv_label_set_text(s_toast_icon, on ? ICON_VOLUME : ICON_MUTE);
    lv_obj_set_style_text_color(s_toast_icon, accent, 0);
    lv_label_set_text(s_toast_text, on ? "T\xC3\xB6ne an" : "T\xC3\xB6ne aus");
    lv_obj_set_style_border_color(s_toast, accent, 0);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, toast_slide_cb);
    lv_anim_set_values(&a, -28, 0);
    lv_anim_set_duration(&a, TOAST_IN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_init(&a);  // fade in
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, toast_fade_cb);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, TOAST_IN_MS);
    lv_anim_start(&a);

    lv_anim_init(&a);  // hold, then fade out and hide
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, toast_fade_out_cb);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_delay(&a, TOAST_IN_MS + TOAST_HOLD_MS);
    lv_anim_set_duration(&a, TOAST_OUT_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, toast_hidden_cb);
    lv_anim_set_early_apply(&a, false);
    lv_anim_start(&a);
    lvgl_port_unlock();
}

void ui_init(lv_display_t* disp, ui_tap_cb_t on_tap) {
    s_on_tap = on_tap;
    lvgl_port_lock(0);

    lv_obj_t* scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, screen_clicked, LV_EVENT_CLICKED, nullptr);

    s_bubble = lv_obj_create(scr);
    lv_obj_remove_flag(s_bubble, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(s_bubble, BUBBLE_SIZE, BUBBLE_SIZE);
    lv_obj_align(s_bubble, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_radius(s_bubble, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_bubble, 4, 0);
    // No shadow/glow: LVGL renders it from a ~40 KB temporary buffer on every
    // frame of the pulse animation; on this PSRAM-less heap that allocation
    // eventually fails and LVGL crashes instead of skipping the shadow.
    lv_obj_set_style_pad_all(s_bubble, 0, 0);

    s_icon = lv_label_create(s_bubble);
    lv_obj_set_style_text_font(s_icon, &font_icons_96, 0);
    lv_obj_center(s_icon);

    s_text = lv_label_create(scr);
    lv_obj_set_width(s_text, 400);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_text, &font_ui_28, 0);
    lv_obj_align(s_text, LV_ALIGN_CENTER, 0, 130);

    create_toast();
    lvgl_port_unlock();
    ui_set_state(UiState::Idle);
}

void ui_set_state(UiState state) {
    lvgl_port_lock(0);
    stop_anims();
    lv_obj_set_style_text_color(s_text, COLOR_TEXT, 0);
    switch (state) {
        case UiState::Idle:
            style_bubble(COLOR_IDLE_FILL, COLOR_IDLE_RING, COLOR_IDLE_ICON, ICON_MIC);
            lv_obj_set_style_text_color(s_text, COLOR_TEXT_DIM, 0);
            lv_label_set_text(s_text, "Sag \xE2\x80\x9E" "Alexa\xE2\x80\x9C");
            display_set_brightness(BRIGHTNESS_IDLE);
            break;
        case UiState::Listening:
            style_bubble(COLOR_LISTEN, COLOR_LISTEN, lv_color_white(), ICON_MIC);
            lv_label_set_text(s_text, "Ich h\xC3\xB6re\xE2\x80\xA6");
            start_pulse(550);
            display_set_brightness(BRIGHTNESS_ACTIVE);
            break;
        case UiState::Thinking:
            style_bubble(COLOR_THINK, COLOR_THINK, lv_color_white(), ICON_MIC);
            lv_label_set_text(s_text, "\xE2\x80\xA6");
            start_ring_breathe(400);
            display_set_brightness(BRIGHTNESS_ACTIVE);
            break;
        case UiState::Muted:
            style_bubble(COLOR_MUTED, COLOR_MUTED, lv_color_white(), ICON_MIC_SLASH);
            lv_obj_set_style_text_color(s_text, COLOR_MUTED, 0);
            lv_label_set_text(s_text, "Stumm");
            display_set_brightness(BRIGHTNESS_IDLE);
            break;
        case UiState::Error:
            style_bubble(COLOR_ERROR, COLOR_ERROR, lv_color_white(), ICON_MIC);
            display_set_brightness(BRIGHTNESS_ACTIVE);
            break;
        case UiState::Offline:
            style_bubble(lv_color_black(), COLOR_IDLE_RING, COLOR_IDLE_RING, ICON_MIC);
            lv_obj_set_style_text_color(s_text, COLOR_TEXT_DIM, 0);
            lv_label_set_text(s_text, "Verbinde\xE2\x80\xA6");
            start_ring_breathe(900);
            display_set_brightness(BRIGHTNESS_IDLE);
            break;
    }
    lvgl_port_unlock();
}

void ui_set_text(const char* text) {
    lvgl_port_lock(0);
    lv_label_set_text(s_text, text);
    lvgl_port_unlock();
}
