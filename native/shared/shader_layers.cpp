// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "shader_layers.hpp"
#include "host_api.hpp"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#elif defined(PROSPERO)
#include <GL/glcorearb.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr std::size_t kMaxLayers = 8, kMaxUniforms = 16, kMaxSource = 64 * 1024;

struct Uniform {
  std::string name;
  float value[4] = {};
  int size = 0;
  GLint location = -1;
};

struct Layer {
  int id = 0;
  std::string source, error;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  float x = 0, y = 0, width = 0, height = 0, opacity = 1, scale = 1;
  std::vector<Uniform> uniforms;
  GLuint program = 0, texture = 0, fbo = 0;
  int fbo_width = 0, fbo_height = 0;
  GLint time = -1, resolution = -1, rect = -1, layer_opacity = -1;
  bool tried = false; // compiled once, successfully or not
};

std::vector<Layer> layers;
int next_id = 1;
GLuint vao = 0, composite = 0;
GLint composite_opacity = -1;

// Covers the viewport with one oversized triangle; uv is 0..1 from the top-left corner.
const char* vertex_source = R"(#version 410 core
out vec2 uv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
  uv = vec2(p.x, 1.0 - p.y);
}
)";
// The layer's straight-alpha output, premultiplied and faded for the premultiplied over blend.
const char* composite_source = R"(#version 410 core
in vec2 uv;
uniform sampler2D layer;
uniform float opacity;
out vec4 color;
void main() {
  vec4 c = texture(layer, vec2(uv.x, 1.0 - uv.y));
  float a = clamp(c.a, 0.0, 1.0) * opacity;
  color = vec4(c.rgb * a, a);
}
)";

GLuint compile(GLenum type, const char* source, std::string& error) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok) return shader;
  char log[2048] = {};
  glGetShaderInfoLog(shader, sizeof log, nullptr, log);
  error = log[0] ? log : "compilation failed";
  glDeleteShader(shader);
  return 0;
}

GLuint link(const char* fragment_source, std::string& error) {
  GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source, error);
  GLuint fragment = vertex ? compile(GL_FRAGMENT_SHADER, fragment_source, error) : 0;
  GLuint program = 0;
  if (vertex && fragment) {
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
      char log[2048] = {};
      glGetProgramInfoLog(program, sizeof log, nullptr, log);
      error = log[0] ? log : "link failed";
      glDeleteProgram(program);
      program = 0;
    }
  }
  if (vertex) glDeleteShader(vertex);
  if (fragment) glDeleteShader(fragment);
  return program;
}

void compile_layer(Layer& layer) {
  layer.tried = true;
  // A source without a #version line gets 410 core, the version both hosts run.
  const std::string source = layer.source.find("#version") == std::string::npos
    ? "#version 410 core\n" + layer.source : layer.source;
  layer.program = link(source.c_str(), layer.error);
  if (!layer.program) {
    std::fprintf(stderr, "[PS5-REACT] shader %d: %s\n", layer.id, layer.error.c_str());
    return;
  }
  layer.time = glGetUniformLocation(layer.program, "uTime");
  layer.resolution = glGetUniformLocation(layer.program, "uResolution");
  layer.rect = glGetUniformLocation(layer.program, "uRect");
  layer.layer_opacity = glGetUniformLocation(layer.program, "uOpacity");
  for (Uniform& u : layer.uniforms) u.location = glGetUniformLocation(layer.program, u.name.c_str());
}

void free_layer_gl(Layer& layer) {
  if (layer.program) glDeleteProgram(layer.program);
  if (layer.texture) glDeleteTextures(1, &layer.texture);
  if (layer.fbo) glDeleteFramebuffers(1, &layer.fbo);
  layer.program = layer.texture = layer.fbo = 0;
  layer.fbo_width = layer.fbo_height = 0;
}

// Layers that ended in JS but still own GL objects; deleted on the next render, where GL is current.
std::vector<Layer> retired;

bool sized(const Layer& layer) { return layer.width >= 1 && layer.height >= 1 && layer.opacity > 0; }

bool ensure_target(Layer& layer, int w, int h) {
  if (layer.fbo && layer.fbo_width == w && layer.fbo_height == h) return true;
  if (!layer.texture) glGenTextures(1, &layer.texture);
  if (!layer.fbo) glGenFramebuffers(1, &layer.fbo);
  glBindTexture(GL_TEXTURE_2D, layer.texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glBindFramebuffer(GL_FRAMEBUFFER, layer.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, layer.texture, 0);
  const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  layer.fbo_width = w; layer.fbo_height = h;
  return complete;
}

Layer* find(JSContext* ctx, JSValueConst value, const char* call) {
  int32_t id = 0;
  if (JS_ToInt32(ctx, &id, value)) return nullptr;
  for (Layer& layer : layers) if (layer.id == id) return &layer;
  JS_ThrowRangeError(ctx, "shader.%s %d: no such shader layer", call, id);
  return nullptr;
}

// Reads an optional finite number property; false only on an exception or a non-finite value.
bool number(JSContext* ctx, JSValueConst object, const char* name, float& out, int id) {
  JSValue value = JS_GetPropertyStr(ctx, object, name);
  if (JS_IsUndefined(value)) return true;
  double d = 0;
  const bool ok = !JS_ToFloat64(ctx, &d, value);
  JS_FreeValue(ctx, value);
  if (!ok) return false;
  if (!std::isfinite(d)) {
    JS_ThrowRangeError(ctx, "shader.update %d: %s must be a finite number", id, name);
    return false;
  }
  out = static_cast<float>(d);
  return true;
}

bool set_uniforms(JSContext* ctx, Layer& layer, JSValueConst object) {
  JSPropertyEnum* names = nullptr;
  uint32_t count = 0;
  if (JS_GetOwnPropertyNames(ctx, &names, &count, object, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) return false;
  bool ok = true;
  for (uint32_t i = 0; i < count && ok; ++i) {
    const char* name = JS_AtomToCString(ctx, names[i].atom);
    JSValue value = JS_GetProperty(ctx, object, names[i].atom);
    float v[4] = {};
    int size = 0;
    if (JS_IsNumber(value)) {
      double d = 0;
      JS_ToFloat64(ctx, &d, value);
      v[0] = static_cast<float>(d);
      size = 1;
    } else if (JS_IsArray(value)) {
      JSValue length = JS_GetPropertyStr(ctx, value, "length");
      int32_t n = 0;
      JS_ToInt32(ctx, &n, length);
      JS_FreeValue(ctx, length);
      size = n;
      for (int k = 0; k < std::min(n, 4); ++k) {
        JSValue item = JS_GetPropertyUint32(ctx, value, k);
        double d = NAN;
        if (JS_IsNumber(item)) JS_ToFloat64(ctx, &d, item);
        JS_FreeValue(ctx, item);
        if (!std::isfinite(d)) size = 0;
        v[k] = static_cast<float>(d);
      }
    }
    JS_FreeValue(ctx, value);
    if (!name) { ok = false; break; }
    if (size < 1 || size > 4) {
      JS_ThrowTypeError(ctx, "shader.update %d: uniform %s must be a number or 2 to 4 finite numbers", layer.id, name);
      ok = false;
    } else {
      auto it = std::find_if(layer.uniforms.begin(), layer.uniforms.end(), [&](const Uniform& u) { return u.name == name; });
      if (it == layer.uniforms.end()) {
        if (layer.uniforms.size() >= kMaxUniforms) {
          JS_ThrowRangeError(ctx, "shader.update %d: more than %zu uniforms", layer.id, kMaxUniforms);
          ok = false;
        } else {
          Uniform& u = layer.uniforms.emplace_back();
          u.name = name;
          u.location = layer.program ? glGetUniformLocation(layer.program, name) : -1;
          it = layer.uniforms.end() - 1;
        }
      }
      if (ok) {
        std::memcpy(it->value, v, sizeof v);
        it->size = size;
      }
    }
    JS_FreeCString(ctx, name);
  }
  for (uint32_t i = 0; i < count; ++i) JS_FreeAtom(ctx, names[i].atom);
  js_free(ctx, names);
  return ok;
}

JSValue create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  host::CallScope scope("shader.create");
  if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "shader.create: expected GLSL source text");
  if (layers.size() >= kMaxLayers) return JS_ThrowRangeError(ctx, "shader.create: more than %zu shader layers", kMaxLayers);
  std::size_t length = 0;
  const char* source = JS_ToCStringLen(ctx, &length, argv[0]);
  if (!source) return JS_EXCEPTION;
  if (length > kMaxSource) {
    JS_FreeCString(ctx, source);
    return JS_ThrowRangeError(ctx, "shader.create: source is %zu bytes, the limit is %zu", length, kMaxSource);
  }
  Layer& layer = layers.emplace_back();
  layer.id = next_id++;
  layer.source.assign(source, length);
  JS_FreeCString(ctx, source);
  return JS_NewInt32(ctx, layer.id);
}

JSValue update(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  host::CallScope scope("shader.update");
  if (argc < 2 || !JS_IsObject(argv[1])) return JS_ThrowTypeError(ctx, "shader.update: expected an ID and an options object");
  Layer* layer = find(ctx, argv[0], "update");
  if (!layer) return JS_EXCEPTION;
  Layer next = *layer; // all or nothing: a bad option leaves the layer as it was
  if (!number(ctx, argv[1], "x", next.x, layer->id) || !number(ctx, argv[1], "y", next.y, layer->id) ||
      !number(ctx, argv[1], "width", next.width, layer->id) || !number(ctx, argv[1], "height", next.height, layer->id) ||
      !number(ctx, argv[1], "opacity", next.opacity, layer->id) ||
      !number(ctx, argv[1], "resolutionScale", next.scale, layer->id)) return JS_EXCEPTION;
  if (!(next.scale >= 0.05f && next.scale <= 1)) return JS_ThrowRangeError(ctx, "shader.update %d: resolutionScale must be 0.05 to 1", layer->id);
  JSValue uniforms = JS_GetPropertyStr(ctx, argv[1], "uniforms");
  const bool ok = JS_IsUndefined(uniforms) || !JS_IsObject(uniforms) ? JS_IsUndefined(uniforms) || JS_IsNull(uniforms)
    : set_uniforms(ctx, next, uniforms);
  JS_FreeValue(ctx, uniforms);
  if (!ok) {
    if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "shader.update %d: uniforms must be an object", layer->id);
    return JS_EXCEPTION;
  }
  layer->x = next.x; layer->y = next.y; layer->width = std::max(0.0f, next.width); layer->height = std::max(0.0f, next.height);
  layer->opacity = std::clamp(next.opacity, 0.0f, 1.0f);
  layer->scale = next.scale;
  layer->uniforms = std::move(next.uniforms);
  return JS_UNDEFINED;
}

JSValue destroy(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  host::CallScope scope("shader.destroy");
  if (argc < 1) return JS_ThrowTypeError(ctx, "shader.destroy: expected an ID");
  Layer* layer = find(ctx, argv[0], "destroy");
  if (!layer) return JS_EXCEPTION;
  retired.push_back(std::move(*layer));
  layers.erase(layers.begin() + (layer - layers.data()));
  return JS_UNDEFINED;
}

JSValue error(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "shader.error: expected an ID");
  Layer* layer = find(ctx, argv[0], "error");
  if (!layer) return JS_EXCEPTION;
  return layer->error.empty() ? JS_NULL : JS_NewStringLen(ctx, layer->error.data(), layer->error.size());
}
} // namespace

JSValue shader_layers::api(JSContext* ctx) {
  JSValue out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, out, "create", JS_NewCFunction(ctx, create, "create", 1));
  JS_SetPropertyStr(ctx, out, "update", JS_NewCFunction(ctx, update, "update", 2));
  JS_SetPropertyStr(ctx, out, "destroy", JS_NewCFunction(ctx, destroy, "destroy", 1));
  JS_SetPropertyStr(ctx, out, "error", JS_NewCFunction(ctx, error, "error", 1));
  return out;
}

bool shader_layers::active() {
  if (!retired.empty()) return true;
  for (const Layer& layer : layers) if (sized(layer) && (layer.program || !layer.tried)) return true;
  return false;
}

void shader_layers::render(const Mapping& map) {
  for (Layer& layer : retired) free_layer_gl(layer);
  retired.clear();
  if (!vao) glGenVertexArrays(1, &vao);
  if (!composite) {
    std::string log;
    composite = link(composite_source, log);
    if (!composite) {
      std::fprintf(stderr, "[PS5-REACT] shader composite: %s\n", log.c_str());
      return;
    }
    composite_opacity = glGetUniformLocation(composite, "opacity");
  }
  glBindVertexArray(vao);
  glActiveTexture(GL_TEXTURE0);
  for (Layer& layer : layers) {
    if (!sized(layer)) continue;
    if (!layer.tried) compile_layer(layer);
    if (!layer.program) continue;
    const int w = std::max(1, static_cast<int>(std::lround(layer.width * layer.scale)));
    const int h = std::max(1, static_cast<int>(std::lround(layer.height * layer.scale)));
    if (!ensure_target(layer, w, h)) continue;
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, layer.fbo);
    glViewport(0, 0, w, h);
    glUseProgram(layer.program);
    const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - layer.start).count();
    if (layer.time >= 0) glUniform1f(layer.time, seconds);
    if (layer.resolution >= 0) glUniform2f(layer.resolution, static_cast<float>(w), static_cast<float>(h));
    if (layer.rect >= 0) glUniform4f(layer.rect, layer.x, layer.y, layer.width, layer.height);
    if (layer.layer_opacity >= 0) glUniform1f(layer.layer_opacity, layer.opacity);
    for (const Uniform& u : layer.uniforms) {
      if (u.location < 0) continue;
      if (u.size == 1) glUniform1f(u.location, u.value[0]);
      else if (u.size == 2) glUniform2fv(u.location, 1, u.value);
      else if (u.size == 3) glUniform3fv(u.location, 1, u.value);
      else glUniform4fv(u.location, 1, u.value);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    // Composite at the layer's rect on the surface; GL's y runs up.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const float left = map.offset_x + layer.x * map.scale, top = map.offset_y + layer.y * map.scale;
    const int x0 = static_cast<int>(std::lround(left)), x1 = static_cast<int>(std::lround(left + layer.width * map.scale));
    const int y0 = static_cast<int>(std::lround(top)), y1 = static_cast<int>(std::lround(top + layer.height * map.scale));
    glViewport(x0, map.surface_height - y1, x1 - x0, y1 - y0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(composite);
    glBindTexture(GL_TEXTURE_2D, layer.texture);
    glUniform1f(composite_opacity, layer.opacity);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void shader_layers::release_gl() {
  for (Layer& layer : layers) {
    free_layer_gl(layer);
    layer.tried = false;
  }
  for (Layer& layer : retired) free_layer_gl(layer);
  retired.clear();
  if (composite) glDeleteProgram(composite);
  if (vao) glDeleteVertexArrays(1, &vao);
  composite = vao = 0;
}
