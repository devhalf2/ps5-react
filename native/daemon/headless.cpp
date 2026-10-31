// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The daemon links the app's native API (host_api.cpp) and platform files without their screen, input
// and sound: these stand in for what they reference there. main.cpp removes the modules from the API.
#include "host_api.hpp"
#include "shader_layers.hpp"
#include "text_shaper.hpp"
extern "C" {
#include "er_runtime.h"
}

JSValue ps5_react_image_api(JSContext*) { return JS_UNDEFINED; }
JSValue ps5_react_sound_api(JSContext*) { return JS_UNDEFINED; }
JSValue ps5_react_browser_capture_api(JSContext*) { return JS_UNDEFINED; }
JSValue shader_layers::api(JSContext*) { return JS_UNDEFINED; }
void text_shaper::set_language(const char*) {}
// Date.now() reads the system clock in the daemon (js_runtime.cpp), not an engine clock.
extern "C" void er_runtime_set_wall_clock(int64_t) {}

#ifdef PROSPERO
// native/ps5/host_platform.cpp drives the pad and the audio output; the daemon has neither, and leaving
// libScePad and libSceAudioOut out keeps the payload from loading modules it never uses.
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/pad.hpp"
namespace hui::ps5 {
AudioOut::~AudioOut() {}
bool AudioOut::start(audio::Mixer&) { return false; }
void AudioOut::stop() {}
void Pad::set_light_bar(std::uint8_t, std::uint8_t, std::uint8_t) {}
void Pad::rumble(float, float) {}
}
#endif
