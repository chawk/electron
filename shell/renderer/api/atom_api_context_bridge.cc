// Copyright (c) 2019 Slack Technologies, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/renderer/api/atom_api_context_bridge.h"

#include <string>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "shell/common/api/remote/object_life_monitor.h"
#include "shell/common/native_mate_converters/callback_converter_deprecated.h"
#include "shell/common/native_mate_converters/once_callback.h"
#include "shell/common/native_mate_converters/value_converter.h"
#include "shell/common/promise_util.h"
#include "shell/renderer/atom_render_frame_observer.h"
#include "third_party/blink/public/web/web_local_frame.h"

namespace electron {

namespace api {

namespace {

content::RenderFrame* GetRenderFrame(v8::Local<v8::Value> value) {
  v8::Local<v8::Context> context =
      v8::Local<v8::Object>::Cast(value)->CreationContext();
  if (context.IsEmpty())
    return nullptr;
  blink::WebLocalFrame* frame = blink::WebLocalFrame::FrameForContext(context);
  if (!frame)
    return nullptr;
  return content::RenderFrame::FromWebFrame(frame);
}

std::map<content::RenderFrame*, RenderFramePersistenceStore*> store_map_;

RenderFramePersistenceStore* GetOrCreateStore(
    content::RenderFrame* render_frame) {
  auto it = store_map_.find(render_frame);
  if (it == store_map_.end()) {
    auto* store = new RenderFramePersistenceStore(render_frame);
    ;
    store_map_[render_frame] = store;
    return store;
  }
  return it->second;
}

// Sourced from "extensions/renderer/v8_schema_registry.cc"
// Recursively freezes every v8 object on |object|.
void DeepFreeze(const v8::Local<v8::Object>& object,
                const v8::Local<v8::Context>& context) {
  v8::Local<v8::Array> property_names =
      object->GetOwnPropertyNames(context).ToLocalChecked();
  for (uint32_t i = 0; i < property_names->Length(); ++i) {
    v8::Local<v8::Value> child =
        object->Get(context, property_names->Get(context, i).ToLocalChecked())
            .ToLocalChecked();
    if (child->IsObject())
      DeepFreeze(v8::Local<v8::Object>::Cast(child), context);
  }
  object->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen);
}

class FunctionLifeMonitor final : public ObjectLifeMonitor {
 public:
  static void BindTo(v8::Isolate* isolate,
                     v8::Local<v8::Object> target,
                     RenderFramePersistenceStore* store,
                     size_t func_id) {
    new FunctionLifeMonitor(isolate, target, store, func_id);
  }

 protected:
  FunctionLifeMonitor(v8::Isolate* isolate,
                      v8::Local<v8::Object> target,
                      RenderFramePersistenceStore* store,
                      size_t func_id)
      : ObjectLifeMonitor(isolate, target), store_(store), func_id_(func_id) {}
  ~FunctionLifeMonitor() override = default;

  void RunDestructor() override { store_->functions().erase(func_id_); }

 private:
  RenderFramePersistenceStore* store_;
  size_t func_id_;
};

}  // namespace

RenderFramePersistenceStore::RenderFramePersistenceStore(
    content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {}

RenderFramePersistenceStore::~RenderFramePersistenceStore() = default;

void RenderFramePersistenceStore::OnDestruct() {
  delete this;
}

v8::Local<v8::Value> PassValueToOtherContext(
    v8::Local<v8::Context> source,
    v8::Local<v8::Context> destination,
    v8::Local<v8::Value> value,
    RenderFramePersistenceStore* store) {
  // Proxy functions and monitor the lifetime in the new context to release
  // the global handle at the right time.
  if (value->IsFunction()) {
    auto func = v8::Local<v8::Function>::Cast(value);
    v8::Global<v8::Function> global_func(source->GetIsolate(), func);
    v8::Global<v8::Context> global_source(source->GetIsolate(), source);

    size_t func_id = store->take_id();
    store->functions()[func_id] =
        std::make_tuple(std::move(global_func), std::move(global_source));
    {
      v8::Context::Scope destination_scope(destination);
      v8::Local<v8::Value> proxy_func = mate::ConvertToV8(
          destination->GetIsolate(),
          base::BindRepeating(&ProxyFunctionWrapper, store, func_id));
      FunctionLifeMonitor::BindTo(destination->GetIsolate(),
                                  v8::Local<v8::Object>::Cast(proxy_func),
                                  store, func_id);
      return proxy_func;
    }
  }

  // Proxy promises as they have a safe and guarunteed memory lifecycle (unlike
  // functions)
  if (value->IsPromise()) {
    v8::Context::Scope destination_scope(destination);

    auto v8_promise = v8::Local<v8::Promise>::Cast(value);
    auto* promise =
        new util::Promise<v8::Local<v8::Value>>(destination->GetIsolate());
    v8::Local<v8::Promise> handle = promise->GetHandle();

    auto then_cb = base::BindOnce(
        [](util::Promise<v8::Local<v8::Value>>* promise, v8::Isolate* isolate,
           v8::Global<v8::Context> source, v8::Global<v8::Context> destination,
           RenderFramePersistenceStore* store, v8::Local<v8::Value> result) {
          promise->Resolve(PassValueToOtherContext(
              source.Get(isolate), destination.Get(isolate), result, store));
          delete promise;
        },
        promise, destination->GetIsolate(),
        v8::Global<v8::Context>(source->GetIsolate(), source),
        v8::Global<v8::Context>(destination->GetIsolate(), destination), store);
    auto catch_cb = base::BindOnce(
        [](util::Promise<v8::Local<v8::Value>>* promise, v8::Isolate* isolate,
           v8::Global<v8::Context> source, v8::Global<v8::Context> destination,
           RenderFramePersistenceStore* store, v8::Local<v8::Value> result) {
          promise->Reject(PassValueToOtherContext(
              source.Get(isolate), destination.Get(isolate), result, store));
          delete promise;
        },
        promise, destination->GetIsolate(),
        v8::Global<v8::Context>(source->GetIsolate(), source),
        v8::Global<v8::Context>(destination->GetIsolate(), destination), store);

    ignore_result(v8_promise->Then(
        source,
        v8::Local<v8::Function>::Cast(
            mate::ConvertToV8(destination->GetIsolate(), then_cb)),
        v8::Local<v8::Function>::Cast(
            mate::ConvertToV8(destination->GetIsolate(), catch_cb))));

    return handle;
  }

  // Errors aren't serializable currently, we need to pull the message out and
  // re-construct in the destination context
  if (value->IsNativeError()) {
    v8::Context::Scope destination_context_scope(destination);
    return v8::Exception::Error(
        v8::Exception::CreateMessage(destination->GetIsolate(), value)->Get());
  }

  // Manually go through the array and pass each value individually into a new
  // array so that deep functions get proxied or arrays of promises.
  if (value->IsArray()) {
    v8::Context::Scope destination_context_scope(destination);
    v8::Local<v8::Array> arr = v8::Local<v8::Array>::Cast(value);
    size_t length = arr->Length();
    v8::Local<v8::Array> cloned_arr =
        v8::Array::New(destination->GetIsolate(), length);
    for (size_t i = 0; i < length; i++) {
      ignore_result(cloned_arr->Set(
          destination, static_cast<int>(i),
          PassValueToOtherContext(source, destination,
                                  arr->Get(source, i).ToLocalChecked(),
                                  store)));
    }
    return cloned_arr;
  }

  // Proxy all objects
  if (value->IsObject() && !value->IsNullOrUndefined()) {
    return CreateProxyForAPI(
               mate::Dictionary(source->GetIsolate(),
                                v8::Local<v8::Object>::Cast(value)),
               source, destination, store)
        .GetHandle();
  }

  // Serializable objects
  // TODO(MarshallOfSound): Use the V8 serializer so we can remove the special
  // null / undefiend handling
  if (value->IsNull()) {
    v8::Context::Scope destination_context_scope(destination);
    return v8::Null(destination->GetIsolate());
  }

  if (value->IsUndefined()) {
    v8::Context::Scope destination_context_scope(destination);
    return v8::Undefined(destination->GetIsolate());
  }

  base::Value ret;
  {
    v8::Context::Scope source_context_scope(source);
    // TODO(MarshallOfSound): What do we do if serialization fails? Throw an
    // error here?
    if (!mate::ConvertFromV8(source->GetIsolate(), value, &ret))
      return v8::Null(destination->GetIsolate());
  }

  v8::Context::Scope destination_context_scope(destination);
  return mate::ConvertToV8(destination->GetIsolate(), ret);
}

v8::Local<v8::Value> ProxyFunctionWrapper(RenderFramePersistenceStore* store,
                                          size_t func_id,
                                          mate::Arguments* args) {
  // Context the proxy function was called from
  v8::Local<v8::Context> calling_context = args->isolate()->GetCurrentContext();
  // Context the function was created in
  v8::Local<v8::Context> func_owning_context =
      std::get<1>(store->functions()[func_id]).Get(args->isolate());

  v8::Context::Scope func_owning_context_scope(func_owning_context);
  v8::Local<v8::Function> func =
      (std::get<0>(store->functions()[func_id])).Get(args->isolate());

  std::vector<v8::Local<v8::Value>> original_args;
  std::vector<v8::Local<v8::Value>> proxied_args;
  args->GetRemaining(&original_args);
  for (auto value : original_args) {
    proxied_args.push_back(PassValueToOtherContext(
        calling_context, func_owning_context, value, store));
  }

  v8::MaybeLocal<v8::Value> maybe_return_value;
  bool did_error = false;
  std::string error_message;
  {
    v8::TryCatch try_catch(args->isolate());
    maybe_return_value = func->Call(func_owning_context, func,
                                    proxied_args.size(), proxied_args.data());
    if (try_catch.HasCaught()) {
      did_error = true;
      auto message = try_catch.Message();

      if (message.IsEmpty() ||
          !mate::ConvertFromV8(args->isolate(), message->Get(),
                               &error_message)) {
        error_message =
            "An unknown exception occurred in the isolated context, an error "
            "occurred but a valid exception was not thrown.";
      }
    }
  }

  if (did_error) {
    v8::Context::Scope calling_context_scope(calling_context);
    args->ThrowError(error_message);
    return v8::Local<v8::Object>();
  }

  if (maybe_return_value.IsEmpty())
    return v8::Undefined(args->isolate());

  auto return_value = maybe_return_value.ToLocalChecked();

  return PassValueToOtherContext(func_owning_context, calling_context,
                                 return_value, store);
}

mate::Dictionary CreateProxyForAPI(mate::Dictionary api,
                                   v8::Local<v8::Context> source_context,
                                   v8::Local<v8::Context> target_context,
                                   RenderFramePersistenceStore* store) {
  mate::Dictionary proxy =
      mate::Dictionary::CreateEmpty(target_context->GetIsolate());
  auto maybe_keys =
      api.GetHandle()->GetOwnPropertyNames(api.isolate()->GetCurrentContext());
  if (maybe_keys.IsEmpty())
    return proxy;
  auto keys = maybe_keys.ToLocalChecked();

  v8::Context::Scope target_context_scope(target_context);
  uint32_t length = keys->Length();
  std::string key_str;
  int key_int;
  for (uint32_t i = 0; i < length; i++) {
    v8::Local<v8::Value> key = keys->Get(target_context, i).ToLocalChecked();
    // Try get the key as a string
    if (!mate::ConvertFromV8(api.isolate(), key, &key_str)) {
      // Try get the key as an int
      if (!mate::ConvertFromV8(api.isolate(), key, &key_int))
        continue;
      // Convert the int to a string as they are interoperable as object keys
      key_str = base::NumberToString(key_int);
    }
    v8::Local<v8::Value> value;
    if (!api.Get(key_str, &value))
      continue;

    if (value->IsFunction()) {
      auto func = v8::Local<v8::Function>::Cast(value);
      v8::Global<v8::Function> global_func(api.isolate(), func);

      size_t func_id = store->take_id();
      store->functions()[func_id] =
          std::make_tuple(std::move(global_func),
                          v8::Global<v8::Context>(source_context->GetIsolate(),
                                                  source_context));
      proxy.SetMethod(
          key_str, base::BindRepeating(&ProxyFunctionWrapper, store, func_id));
    } else if (value->IsObject() && !value->IsNullOrUndefined() &&
               !value->IsArray() && !value->IsPromise()) {
      mate::Dictionary sub_api(api.isolate(),
                               v8::Local<v8::Object>::Cast(value));
      proxy.Set(key_str, CreateProxyForAPI(sub_api, source_context,
                                           target_context, store));
    } else {
      proxy.Set(key_str, PassValueToOtherContext(source_context, target_context,
                                                 value, store));
    }
  }

  return proxy;
}

#ifdef DCHECK_IS_ON
mate::Dictionary DebugGC(mate::Dictionary empty) {
  auto* render_frame = GetRenderFrame(empty.GetHandle());
  RenderFramePersistenceStore* store = GetOrCreateStore(render_frame);
  mate::Dictionary ret = mate::Dictionary::CreateEmpty(empty.isolate());
  ret.Set("functionCount", store->functions().size());
  return ret;
}
#endif

void ExposeAPIInMainWorld(const std::string& key,
                          mate::Dictionary api,
                          mate::Arguments* args) {
  auto* render_frame = GetRenderFrame(api.GetHandle());
  RenderFramePersistenceStore* store = GetOrCreateStore(render_frame);
  auto* frame = render_frame->GetWebFrame();
  DCHECK(frame);
  v8::Local<v8::Context> main_context = frame->MainWorldScriptContext();
  mate::Dictionary global(main_context->GetIsolate(), main_context->Global());

  if (global.Has(key)) {
    args->ThrowError(
        "Cannot bind an API on top of an existing property on the window "
        "object");
    return;
  }

  v8::Local<v8::Context> isolated_context =
      frame->WorldScriptContext(api.isolate(), World::ISOLATED_WORLD);

  {
    v8::Context::Scope main_context_scope(main_context);
    mate::Dictionary proxy =
        CreateProxyForAPI(api, isolated_context, main_context, store);
    DeepFreeze(proxy.GetHandle(), main_context);
    global.SetReadOnlyNonConfigurable(key, proxy);
  }
}

}  // namespace api

}  // namespace electron

namespace {

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  using namespace electron::api;  // NOLINT(build/namespaces)

  v8::Isolate* isolate = context->GetIsolate();
  mate::Dictionary dict(isolate, exports);
  dict.SetMethod("exposeAPIInMainWorld", &ExposeAPIInMainWorld);
#ifdef DCHECK_IS_ON
  dict.SetMethod("_debugGCMaps", &DebugGC);
#endif
}

}  // namespace

NODE_LINKED_MODULE_CONTEXT_AWARE(atom_renderer_context_bridge, Initialize)