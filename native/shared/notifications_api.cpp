// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Rich toasts (ABI v11): notifications.send builds the system notification JSON that payloads send
// through sceNotificationSend (the payload SDK's samples/notify; ShadowMountPlus TOASTS.md documents
// the fields) and falls back to the plain toast of host::notify when the platform cannot send it.
#include "system_state.hpp"
#include "host_api.hpp"

#if __has_include("app_config.hpp")
#include "app_config.hpp"
#endif
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
#ifdef PS5_REACT_TITLE
constexpr const char* kBundlePrefix = "ps5react_" PS5_REACT_TITLE "_";
#else
constexpr const char* kBundlePrefix = "ps5react_";
#endif
// Characters past this many in an id do not tell toasts apart.
constexpr std::size_t kMaxIdChars = 48;

void append_string(std::string& out, const char* text) {
  out += '"';
  for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c) {
    switch (*c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (*c < 0x20) {
          char escaped[8];
          std::snprintf(escaped, sizeof escaped, "\\u%04x", *c);
          out += escaped;
        } else out += static_cast<char>(*c);
    }
  }
  out += '"';
}

// The overlay compares createdDateTime as text: canonical UTC with milliseconds.
std::string timestamp(double ms) {
  const auto seconds = static_cast<time_t>(ms / 1000);
  tm utc{};
  gmtime_r(&seconds, &utc);
  char text[32];
  std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(static_cast<long long>(ms) % 1000));
  return text;
}

// From TOASTS.md's list of predefined icons seen in working payloads.
const char* predefined_icon(const char* kind) {
  return !std::strcmp(kind, "failed") || !std::strcmp(kind, "attention") ? "error_message_caution" : "download";
}

void append_icon(std::string& out, const char* url, const char* kind) {
  if (*url) {
    out += "{\"type\":\"Url\",\"parameters\":{\"url\":";
    append_string(out, url);
  } else {
    out += "{\"type\":\"Predefined\",\"parameters\":{\"icon\":";
    append_string(out, predefined_icon(kind));
  }
  out += "}}";
}

void append_messages(std::string& out, const char* title, const char* body) {
  out += "\"message\":{\"body\":";
  append_string(out, title);
  out += '}';
  if (*body) {
    out += ",\"subMessage\":{\"body\":";
    append_string(out, body);
    out += '}';
  }
}
} // namespace

namespace toast {
bool known_kind(const char* kind) {
  for (const char* known : {"info", "done", "failed", "attention"})
    if (!std::strcmp(kind, known)) return true;
  return false;
}

std::string json(const char* id, const char* title, const char* body, const char* icon_url, const char* kind,
                 double created_ms) {
  std::string bundle = kBundlePrefix;
  for (std::size_t i = 0; id[i] && i < kMaxIdChars; ++i) {
    const char c = id[i];
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    bundle += plain ? c : '_';
  }
  // Replacement matches userId, then bundleName, and needs an overwrite type other than "No".
  std::string out = "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\",\"channelType\":\"Downloads\","
                    "\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"Always\",\"isImmediate\":true,\"priority\":100,"
                    "\"bundleName\":";
  append_string(out, bundle.c_str());
  out += ",\"viewData\":{\"icon\":";
  append_icon(out, icon_url, kind);
  out += ',';
  append_messages(out, title, body);
  out += "},\"platformViews\":{\"previewDisabled\":{\"viewData\":{\"icon\":";
  append_icon(out, "", kind);
  out += ',';
  append_messages(out, title, "");
  out += "}}}},\"createdDateTime\":\"" + timestamp(created_ms) + "\"}";
  return out;
}
} // namespace toast

namespace {
struct Strings {
  JSContext* ctx;
  const char* values[5] = {};
  ~Strings() {
    for (const char* value : values)
      if (value) JS_FreeCString(ctx, value);
  }
};

// notifications.send(id, title, body, icon, kind): every argument a string; icon '' for none.
JSValue send(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
  Strings strings{ctx};
  for (int i = 0; i < 5; ++i)
    if (!(strings.values[i] = JS_ToCString(ctx, argv[i]))) return JS_EXCEPTION;
  const auto [id, title, body, icon, kind] = strings.values;
  if (!*id) return JS_ThrowTypeError(ctx, "notifications.send: id must not be empty");
  if (!toast::known_kind(kind))
    return JS_ThrowTypeError(ctx, "notifications.send %s: unknown kind '%s'", id, kind);
  // An app path becomes the console path the system's overlay reads; URLs pass through.
  char real[PATH_MAX] = "";
  if (*icon == '/' && !host::resolve_path(icon, real, sizeof real))
    return JS_ThrowPlainError(ctx, "notifications.send %s: icon %s: Path is not allowed", id, icon);
  if (*icon && *icon != '/') std::snprintf(real, sizeof real, "%s", icon);
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  const double created = static_cast<double>(now.tv_sec) * 1000 + static_cast<double>(now.tv_nsec / 1000000);
  if (system_platform::send_notification(toast::json(id, title, body, real, kind, created).c_str())) return JS_TRUE;
  return JS_NewBool(ctx, host::notify(title, body));
}
} // namespace

JSValue ps5_react_notifications_api(JSContext* ctx) {
  JSValue api = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, api, "send", JS_NewCFunction(ctx, send, "send", 5));
  return api;
}
