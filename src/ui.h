#pragma once

#include <lvgl.h>

enum class UiState {
    Idle,       // waiting for the wake word
    Listening,  // wake word heard, recording the command
    Thinking,   // command sent, waiting for Home Assistant
    Muted,      // mic off
    Error,      // text set separately via ui_set_text
    Offline,    // no connection to Home Assistant yet
};

typedef void (*ui_tap_cb_t)(void);

// All ui_* functions take the LVGL lock themselves; call them from any task.
void ui_init(lv_display_t* disp, ui_tap_cb_t on_tap);
void ui_set_state(UiState state);
void ui_set_text(const char* text);

// Brief pop-up at the top: "Töne an" / "Töne aus".
void ui_show_sounds_toast(bool on);
