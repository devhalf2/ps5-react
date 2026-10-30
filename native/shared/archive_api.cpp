// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"
#include "archives.hpp"
#include "archive_preflight.hpp"
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>

namespace {
bool path(JSContext* ctx, JSValueConst value, std::string& out) {
  if (!JS_IsString(value)) return false;
  std::size_t length = 0; const char* input = JS_ToCStringLen(ctx,&length,value);
  if (!input) return false;
  char resolved[PATH_MAX];
  const bool ok = length && length < PATH_MAX && !std::memchr(input,0,length) && host::resolve_path(input,resolved,sizeof resolved);
  if (ok) out = resolved;
  JS_FreeCString(ctx,input); return ok;
}
JSValue extract(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 3 || !JS_IsArray(argv[0])) return JS_ThrowTypeError(ctx,"archives.extract: expected sources, destination, maxBytes");
  archives::Request request;
  if (!path(ctx,argv[1],request.destination)) return JS_ThrowTypeError(ctx,"archives.extract: destination is not allowed");
  // Package resources are always read-only, even when console access is enabled.
  const char* destination = JS_ToCString(ctx,argv[1]);
  const bool readonly = destination && (!std::strcmp(destination,"/app0") || !std::strncmp(destination,"/app0/",6));
  if (destination) JS_FreeCString(ctx,destination);
  if (readonly) return JS_ThrowTypeError(ctx,"archives.extract: /app0 is read-only");
  double maximum = 0;
  if (JS_ToFloat64(ctx,&maximum,argv[2]) || !std::isfinite(maximum) || maximum < 1 || maximum > 1099511627776.0 || std::floor(maximum) != maximum)
    return JS_ThrowTypeError(ctx,"archives.extract: maxBytes must be an integer between 1 and 1 TiB");
  request.max_bytes = static_cast<std::uint64_t>(maximum);
  if (argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) {
    if (!JS_IsString(argv[3])) return JS_ThrowTypeError(ctx,"archives.extract: password must be a string");
    std::size_t size = 0;
    const char* password = JS_ToCStringLen(ctx,&size,argv[3]);
    if (!password) return JS_EXCEPTION;
    const bool valid = size <= 1024 && !std::memchr(password,0,size);
    if (valid) request.password.assign(password,size);
    JS_FreeCString(ctx,password);
    if (!valid) return JS_ThrowTypeError(ctx,"archives.extract: password must have at most 1024 UTF-8 bytes and no NUL");
  }
  const JSValue length_value = JS_GetPropertyStr(ctx,argv[0],"length"); std::uint32_t length = 0;
  const int result = JS_ToUint32(ctx,&length,length_value); JS_FreeValue(ctx,length_value);
  if (result || !length || length > 1024) return JS_ThrowTypeError(ctx,"archives.extract: expected 1–1024 ordered volumes");
  for (std::uint32_t i = 0; i < length; ++i) {
    const JSValue value = JS_GetPropertyUint32(ctx,argv[0],i); std::string file;
    const bool ok = path(ctx,value,file); JS_FreeValue(ctx,value);
    if (!ok) return JS_ThrowTypeError(ctx,"archives.extract: source path is not allowed");
    request.sources.push_back(std::move(file));
  }
  if (argc > 4) request.stream = JS_ToBool(ctx,argv[4]) == 1;
  // A downloading 7z's URL and request headers ("Name: value"), for the engine to fetch its end ahead.
  if (argc > 5 && JS_IsString(argv[5])) {
    const char* url = JS_ToCString(ctx,argv[5]);
    if (!url) return JS_EXCEPTION;
    request.tail_url = url;
    JS_FreeCString(ctx,url);
    if (request.tail_url.size() > 8192 || (request.tail_url.rfind("https://",0) && request.tail_url.rfind("http://",0)))
      return JS_ThrowTypeError(ctx,"archives.extract: tailUrl must be an http(s) URL");
  }
  if (argc > 6 && JS_IsArray(argv[6])) {
    const JSValue count_value = JS_GetPropertyStr(ctx,argv[6],"length"); std::uint32_t count = 0;
    const int counted = JS_ToUint32(ctx,&count,count_value); JS_FreeValue(ctx,count_value);
    if (counted || count > 32) return JS_ThrowTypeError(ctx,"archives.extract: at most 32 tail headers");
    for (std::uint32_t i = 0; i < count; ++i) {
      const JSValue value = JS_GetPropertyUint32(ctx,argv[6],i);
      const char* text = JS_IsString(value) ? JS_ToCString(ctx,value) : nullptr;
      JS_FreeValue(ctx,value);
      std::string header = text ? text : "";
      if (text) JS_FreeCString(ctx,text);
      std::string name = header.substr(0, header.find(':'));
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      // The engine sets the range itself.
      if (header.empty() || header.size() > 2048 || header.find(':') == std::string::npos || header.find_first_of("\r\n") != std::string::npos ||
          name == "range" || name == "if-range" || name == "host" || name == "content-length" || name == "connection")
        return JS_ThrowTypeError(ctx,"archives.extract: invalid tail header");
      request.tail_headers.push_back(std::move(header));
    }
  }
  std::string error; const auto id = archives::enqueue(std::move(request),error);
  if (!id) return JS_ThrowPlainError(ctx,"archives.extract: %s",error.c_str());
  return JS_NewUint32(ctx,id);
}
// The headers are read on a worker: a download writing to the same drive can hold one read for seconds.
// The render thread waits a few frames at most and otherwise reports the read as pending.
constexpr std::chrono::milliseconds inspect_wait{25};
JSValue inspect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1 || !JS_IsArray(argv[0])) return JS_ThrowTypeError(ctx,"archives.inspect: expected sources");
  const JSValue length_value = JS_GetPropertyStr(ctx,argv[0],"length"); std::uint32_t length = 0;
  const int result = JS_ToUint32(ctx,&length,length_value); JS_FreeValue(ctx,length_value);
  if (result || !length || length > 1024) return JS_ThrowTypeError(ctx,"archives.inspect: expected 1–1024 ordered volumes");
  std::vector<std::string> sources;
  for (std::uint32_t i = 0; i < length; ++i) {
    const JSValue value = JS_GetPropertyUint32(ctx,argv[0],i); std::string file;
    const bool ok = path(ctx,value,file); JS_FreeValue(ctx,value);
    if (!ok) return JS_ThrowTypeError(ctx,"archives.inspect: source path is not allowed");
    sources.push_back(std::move(file));
  }
  const host::CallScope scope("archives.inspect");
  const auto [inspection, pending] = archives::inspect_soon(sources, inspect_wait);
  JSValue out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx,out,"kind",JS_NewString(ctx,inspection.kind.c_str()));
  JS_SetPropertyStr(ctx,out,"refusal",JS_NewString(ctx,inspection.refusal.c_str()));
  if (pending) JS_SetPropertyStr(ctx,out,"pending",JS_TRUE);
  return out;
}
JSValue cancel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::uint32_t id = 0;
  if (!argc || JS_ToUint32(ctx,&id,argv[0])) return JS_ThrowTypeError(ctx,"archives.cancel: expected task id");
  archives::cancel(id); return JS_UNDEFINED;
}
JSValue poll(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  JSValue out = JS_NewArray(ctx); std::uint32_t index = 0;
  for (const auto& snapshot : archives::poll()) {
    JSValue item = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,item,"id",JS_NewUint32(ctx,snapshot.id));
    JS_SetPropertyStr(ctx,item,"state",JS_NewString(ctx,snapshot.state.c_str()));
    JS_SetPropertyStr(ctx,item,"error",JS_NewString(ctx,snapshot.error.c_str()));
    JS_SetPropertyStr(ctx,item,"phase",JS_NewString(ctx,snapshot.phase.c_str()));
    JS_SetPropertyStr(ctx,item,"synced",JS_NewFloat64(ctx,snapshot.synced));
    JS_SetPropertyStr(ctx,item,"syncTotal",JS_NewFloat64(ctx,snapshot.sync_total));
    JS_SetPropertyStr(ctx,item,"written",JS_NewFloat64(ctx,snapshot.written));
    JS_SetPropertyStr(ctx,item,"total",snapshot.total ? JS_NewFloat64(ctx,snapshot.total) : JS_NULL);
    JS_SetPropertyStr(ctx,item,"entries",JS_NewFloat64(ctx,snapshot.entries));
    JSValue artifacts = JS_NewArray(ctx); std::uint32_t at = 0;
    for (const auto& path : snapshot.artifacts) JS_SetPropertyUint32(ctx,artifacts,at++,JS_NewString(ctx,path.c_str()));
    JS_SetPropertyStr(ctx,item,"artifacts",artifacts);
    JS_SetPropertyUint32(ctx,out,index++,item);
  }
  return out;
}
}
JSValue ps5_react_archive_api(JSContext* ctx) {
  JSValue api = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx,api,"extract",JS_NewCFunction(ctx,extract,"extract",5));
  JS_SetPropertyStr(ctx,api,"inspect",JS_NewCFunction(ctx,inspect,"inspect",1));
  JS_SetPropertyStr(ctx,api,"cancel",JS_NewCFunction(ctx,cancel,"cancel",1));
  JS_SetPropertyStr(ctx,api,"poll",JS_NewCFunction(ctx,poll,"poll",0));
  return api;
}
