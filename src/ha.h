#pragma once

#include <stddef.h>
#include <stdint.h>

// Home Assistant Assist pipeline client over the WebSocket API. The board
// streams the mic continuously; HA does wake word (openWakeWord), speech to
// text and intent handling. One pipeline run covers one conversation; a new
// run starts automatically after each one ends.

enum class HaEvent {
    Ready,         // authenticated, waiting for the wake word
    Offline,       // connection lost (it reconnects by itself)
    WakeWord,      // wake word detected (also mid-conversation): recording a new command
    Listening,     // recording started without wake word (push-to-talk)
    SpeechEnded,   // HA's VAD heard the end of the command
    Transcript,    // text = the command HA understood, wake word removed
    Response,      // text = HA's answer
    Error,         // text = message to show
};

// Called from the WebSocket task. `text` is only valid during the call.
typedef void (*ha_event_cb_t)(HaEvent event, const char* text);

void ha_init(ha_event_cb_t cb);

// Cancels the current conversation, if any; with push_to_talk, starts
// recording a new one straight away. The wake word keeps being listened for
// throughout, so saying it again during a conversation starts a new one.
void ha_restart(bool push_to_talk);

// Ends the recording of the command. Wake word runs need this: HA's own
// end-of-speech detection is off for them (see ha.cpp).
void ha_end_speech(void);

// True while HA wants audio (a run is in the wake word or speech stage).
bool ha_wants_audio(void);

// One chunk (up to 512 samples) of 16 kHz mono 16-bit PCM. Queued; never
// blocks.
void ha_send_audio(const int16_t* samples, size_t count);
