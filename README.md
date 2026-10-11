# OpenAssistant

A fully local, Alexa-style voice assistant for **Home Assistant**, running on the
**Waveshare ESP32-C6-Touch-AMOLED-2.16**: a 480×480 round-cornered AMOLED with
two microphones, a speaker and a touch screen.

Say *"Alexa, Licht an"* and the lights in the room the panel is in turn on. No
cloud: the wake word, speech recognition and intent handling all run on your
own Home Assistant server. The board streams the microphone, shows what's
happening, and plays a short chime (if you want it to).

## Features

- **Wake word, local:** "Alexa" detected by [openWakeWord] on Home Assistant.
  Works in one breath (*"Alexa Licht an"*) or with a pause (*"Alexa" … "Licht an"*).
- **Interrupt any time:** saying the wake word again starts a new command, even
  while the panel is still listening or showing an answer.
- **Room aware:** the panel registers itself in Home Assistant as a device. Put
  it in an area, and "Licht an" switches *that* room's lights, no room name needed.
- **Speech to text, local:** [Whisper] (German by default; any language Whisper and
  Assist support works).
- **Minimal UI:** one bubble. Grey = waiting, blue pulsing = listening, purple =
  thinking, red = muted. The answer appears as text underneath.
- **Tap the screen** to mute (or to cancel a command in progress).
- **Buttons:** BOOT = push-to-talk without the wake word, KEY = sounds on/off
  (with an on-screen pop-up; remembered across restarts).
- **Home Assistant entities** via MQTT: a status sensor (idle / listening /
  thinking / muted / offline) and a mute switch, for automations and dashboards.

## How it works

```
 ESP32-C6 panel                                   Home Assistant
┌──────────────────────┐   WebSocket API      ┌────────────────────────────┐
│ mic ─► 16 kHz PCM ───┼─────────────────────►│ Assist pipeline            │
│                      │                      │  openWakeWord  ("alexa")   │
│ end-of-speech        │◄── wake word event ──┤  Whisper       (speech→text)│
│ detection            │◄── transcript ───────┤  Assist        (intents)   │
│ UI / chime           │◄── answer ───────────┤                            │
│                      │   MQTT discovery     │                            │
│ "Voice Panel" device ├─────────────────────►│ device in an area          │
└──────────────────────┘                      └────────────────────────────┘
```

The board has no PSRAM and a single 160 MHz core, so it doesn't run any speech
models itself. Instead:

1. It streams the microphone to an Assist pipeline that waits for the wake word.
   One such "standby" run is always open, even during a conversation, so a new
   "Alexa" can interrupt.
2. Home Assistant reports the wake word a few hundred milliseconds late, so the
   pipeline keeps 1 s of audio from before the detection. That way a command
   said in one breath isn't cut off.
3. The board decides when you've stopped talking (Home Assistant's own detector
   would end the recording in the pause after "Alexa").
4. Whisper's transcript starts with "Alexa"; the board strips it and sends the
   command text to Assist with its device ID, which is what makes "Licht an"
   resolve to the panel's area.

## Hardware

| | |
|---|---|
| Board | [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://www.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-2.16) |
| SoC | ESP32-C6, 160 MHz RISC-V, 512 KB SRAM, no PSRAM, 16 MB flash |
| Display | 2.16" AMOLED 480×480, QSPI (SH8601-compatible driver) |
| Touch | CST9217 |
| Audio | ES8311 (speaker), ES7210 (2 microphones) |
| Power | AXP2101 PMU, optional LiPo |

Pins are in [`src/board.h`](src/board.h). Note that Waveshare's own ESP-IDF
examples swap `LCD_CS` and `TP_INT`; the values here match their XiaoZhi
firmware, which runs on this board.

## Home Assistant setup

Tested with Home Assistant OS 2026.9.

1. **Add-ons** (Settings → Add-ons → Add-on store):
   - **openWakeWord**: the built-in `alexa` model is trained on English
     pronunciation. A low `threshold` (0.25 here; default 0.5) makes it accept
     other pronunciations and natural variation, at the cost of the occasional
     false trigger (which just shows "Nicht verstanden").
   - **Whisper**: `language: de` (or yours), `model: small-int8`, and
     `initial_prompt: "Alexa."` so the wake word is transcribed as "Alexa" and
     can be stripped. Don't put example *commands* in the prompt: Whisper
     reproduces them from noise.
   - **Mosquitto broker**, if you don't run one yet.

   Accept the Wyoming integrations HA discovers for openWakeWord and Whisper.
2. **Assist pipeline** (Settings → Voice assistants → Add): conversation agent
   *Home Assistant*, speech-to-text *faster-whisper*, wake word
   *openwakeword / alexa*, no text-to-speech. Note its ID (in the URL when editing).
3. **Long-lived access token:** your profile → Security → Long-lived access tokens.
4. **MQTT login:** any user the broker accepts. With the Mosquitto add-on, a
   Home Assistant user works (Settings → People → Users → Add, *local only*,
   not an administrator).
5. **Expose** the entities you want to control to Assist, and give them
   speakable names or aliases. Speech recognition can only match names you
   can actually say.
6. After the first boot, the device **Voice Panel** appears under the MQTT
   integration. Assign it to an area; the lights in that area are what
   "Licht an" / "Licht aus" switch.

> **Speech-to-Phrase** would be the faster, command-optimised alternative to
> Whisper, but its decoder crashed on every request in a Proxmox VM here (with
> CPU types `x86-64-v2-AES` and `x86-64-v3`). Whisper works and answers in about
> a second on the same VM.

## Building and flashing

Requires [PlatformIO](https://platformio.org/) (CLI or VS Code). The platform,
ESP-IDF 5.5 and all components are downloaded on the first build.

```sh
cp src/secrets.h.example src/secrets.h   # then fill in Wi-Fi, HA and MQTT
pio run -t upload                        # build and flash over USB
pio device monitor                       # serial log
```

The board uses its native USB port (USB-Serial/JTAG); no driver or boot-button
dance is needed.

**On Windows**, set `PYTHONIOENCODING=utf-8` before `pio run` if the upload
stops right after `esptool` starts: esptool's progress bar can crash
PlatformIO's console output when it's redirected.

## Usage

| Action | Result |
|---|---|
| "Alexa, Licht an" (one breath or with a pause) | Turns on the lights in the panel's area |
| "Alexa" again while it's busy | Abandons the current command, listens anew |
| Tap the screen while idle | Mute (red) / unmute |
| Tap while listening or thinking | Cancel |
| BOOT button | Push-to-talk, no wake word |
| KEY button | Sounds on/off |
| `switch.voice_panel_stumm` in HA | Mute from Home Assistant |
| `sensor.voice_panel_status` in HA | Current state, for automations |

Everything Assist understands works: lights, switches, covers, climate, scenes,
"wie spät ist es", and so on. Timers don't, because those need a device type
Home Assistant only supports via its own satellite integrations.

## Tuning

| What | Where |
|---|---|
| Wake word sensitivity | openWakeWord add-on option `threshold` |
| Microphone gain (36 dB) | `MIC_GAIN_DB` in [`src/audio.cpp`](src/audio.cpp) |
| End of command: silence, timeouts | `END_SILENCE_MS`, `NO_SPEECH_MS`, … in [`src/main.cpp`](src/main.cpp) |
| Pre-wake-word audio buffer | `WAKE_BUFFER_S` in [`src/ha.cpp`](src/ha.cpp) |
| Wake word text that gets stripped | `is_wake_fragment()` in [`src/ha.cpp`](src/ha.cpp) |
| Answer display time, colours, brightness | [`src/main.cpp`](src/main.cpp), [`src/ui.cpp`](src/ui.cpp) |

To use a different wake word, pick it in the pipeline and adapt
`is_wake_fragment()` so it's removed from the transcript.

## Project layout

```
src/
  main.cpp       state machine, mic streaming, end-of-speech detection, buttons
  ha.cpp         Home Assistant WebSocket client: pipeline runs, audio, intents
  mqtt.cpp       MQTT discovery: the "Voice Panel" device and its entities
  ui.cpp         LVGL UI: bubble, status text, pop-up
  display.cpp    QSPI AMOLED + touch + LVGL port
  audio.cpp      ES8311 / ES7210 over I2S, chimes
  pmu.cpp        AXP2101 power rails (also the display reset)
  wifi.cpp       Wi-Fi station
  board.h        pin map
  font_*.c       generated LVGL fonts (Montserrat + Font Awesome icons)
  secrets.h.example
```

## Memory notes

With no PSRAM, everything shares ~512 KB of SRAM (well over 100 KB stays free
at runtime). Two things that are harmless elsewhere crash LVGL here: box shadows
and whole-object opacity both allocate large temporary buffers on every frame,
and LVGL doesn't handle a failed allocation. The UI avoids both.

## Credits

- Pin assignments and panel init from Waveshare's
  [ESP32-C6-Touch-AMOLED-2.16](https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.16)
  examples (XiaoZhi firmware) and the C6 port of
  [Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter).
- Built with [ESP-IDF](https://github.com/espressif/esp-idf),
  [LVGL](https://lvgl.io), [esp_lvgl_port](https://github.com/espressif/esp-bsp),
  [esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev)
  and [esp_websocket_client](https://github.com/espressif/esp-protocols).
- Fonts generated with [lv_font_conv](https://github.com/lvgl/lv_font_conv) from
  [Montserrat](https://github.com/JulietaUla/Montserrat) (SIL Open Font License 1.1)
  and [Font Awesome Free](https://fontawesome.com) icons (CC BY 4.0, font under
  SIL OFL 1.1).
- "Alexa" is a trademark of Amazon. This project is not affiliated with Amazon;
  it only uses the openWakeWord model of that name.

[openWakeWord]: https://github.com/dscripka/openWakeWord
[Whisper]: https://github.com/home-assistant/addons/tree/master/whisper
