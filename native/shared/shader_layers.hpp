// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

extern "C" {
#include "quickjs.h"
}

// GPU shader layers (docs/SHADERS.md). JavaScript creates a layer from GLSL fragment source and updates its
// rect and uniforms; the presenter draws every layer, then the software framebuffer over them with its alpha,
// so a View with `cutout` shows the layer through and the UI painted after it stays on top.
// Everything runs on the render thread. The JS calls never touch GL: the presenter compiles a layer the
// first time it draws it.
namespace shader_layers {
// Where framebuffer pixels land on the surface: surface = offset + framebuffer * scale, y down.
struct Mapping {
  int surface_height;
  float offset_x, offset_y, scale;
};

// ABI v10: `__ps5ReactNative.shader`.
JSValue api(JSContext* ctx);
// True while any layer would draw (non-empty rect, opacity above 0, compiled or not yet tried).
bool active();
// Draws the layers into the bound default framebuffer, in creation order. Leaves blending enabled
// for the framebuffer pass (premultiplied over), the default framebuffer bound and the VAO unbound.
void render(const Mapping& mapping);
// Deletes GL objects; call before the context goes. Layers recompile if drawn again.
void release_gl();
} // namespace shader_layers
