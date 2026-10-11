#pragma once

#include <driver/i2c_master.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_SAMPLE_RATE 16000  // what Home Assistant's voice pipeline expects

void audio_init(i2c_master_bus_handle_t bus);

// Blocks until `samples` mono 16-bit samples have been read from the mic.
void audio_read(int16_t* dest, size_t samples);

enum class Chime { Wake, Done, Error };

// Queued; plays on a separate task so the mic keeps running. Does nothing
// while sounds are off.
void audio_play_chime(Chime chime);

void audio_set_sounds(bool enabled);
bool audio_sounds_enabled(void);

// True while a chime is playing or ended less than `tail_ms` ago (the mic
// hears it and its echo, so callers may want to skip that audio).
bool audio_is_playing(int tail_ms = 0);
