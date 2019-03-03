#include "env-inl.h"
#include "node.h"
#include "node_internals.h"
#include "v8.h"

#include <atomic>

namespace node {

using v8::Array;
using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::Isolate;
using v8::kPromiseHandlerAddedAfterReject;
using v8::kPromiseRejectAfterResolved;
using v8::kPromiseRejectWithNoHandler;
using v8::kPromiseResolveAfterResolved;
using v8::Local;
using v8::Number;
using v8::Object;
using v8::Promise;
using v8::PromiseRejectEvent;
using v8::PromiseRejectMessage;
using v8::Value;

namespace task_queue {

static void RunMicrotasks(const FunctionCallbackInfo<Value>& args) {
  args.GetIsolate()->RunMicrotasks();
}

static void SetTickCallback(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsFunction());
  env->set_tick_callback_function(args[0].As<Function>());
}

void PromiseRejectCallback(PromiseRejectMessage message) {
  static std::atomic<uint64_t> unhandledRejections{0};
  static std::atomic<uint64_t> rejectionsHandledAfter{0};

  Local<Promise> promise = message.GetPromise();
  Isolate* isolate = promise->GetIsolate();
  PromiseRejectEvent event = message.GetEvent();

  Environment* env = Environment::GetCurrent(isolate);

  if (env == nullptr) return;

  Local<Function> callback = env->promise_reject_callback();
  // The promise is rejected before JS land calls SetPromiseRejectCallback
  // to initializes the promise reject callback during bootstrap.
  CHECK(!callback.IsEmpty());

  Local<Value> value;
  Local<Value> type = Number::New(env->isolate(), event);

  if (event == kPromiseRejectWithNoHandler) {
    value = message.GetValue();
    unhandledRejections++;
    TRACE_COUNTER2(TRACING_CATEGORY_NODE2(promises, rejections),
                  "rejections",
                  "unhandled", unhandledRejections,
                  "handledAfter", rejectionsHandledAfter);
  } else if (event == kPromiseHandlerAddedAfterReject) {
    value = Undefined(isolate);
    rejectionsHandledAfter++;
    TRACE_COUNTER2(TRACING_CATEGORY_NODE2(promises, rejections),
                  "rejections",
                  "unhandled", unhandledRejections,
                  "handledAfter", rejectionsHandledAfter);
  } else if (event == kPromiseResolveAfterResolved) {
    value = message.GetValue();
  } else if (event == kPromiseRejectAfterResolved) {
    value = message.GetValue();
  } else {
    return;
  }

  if (value.IsEmpty()) {
    value = Undefined(isolate);
  }

  Local<Value> args[] = { type, promise, value };
  USE(callback->Call(
      env->context(), Undefined(isolate), arraysize(args), args));
}

static void SetPromiseRejectCallback(
    const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsFunction());
  env->set_promise_reject_callback(args[0].As<Function>());
}

static void Sync(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsPromise());
  v8::Local<v8::Promise> promise = args[0].As<v8::Promise>();
  if (promise->State() == v8::Promise::kFulfilled) {
    args.GetReturnValue().Set(promise->Result());
    return;
  }

  Isolate* isolate = args.GetIsolate();
  Environment* env = Environment::GetCurrent(args);

  uv_loop_t* loop = env->event_loop();
  int state = promise->State();
  while (state == v8::Promise::kPending) {
    isolate->RunMicrotasks();
    if (uv_loop_alive(loop)) {
      uv_run(loop, UV_RUN_ONCE);
    }
    state = promise->State();
  }

  args.GetReturnValue().Set(promise->Result());
}

static void ExecuteWithinNewLoop(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsFunction());

  v8::Local<v8::Function> func = args[0].As<v8::Function>();
  Isolate* isolate = args.GetIsolate();
  Environment* env = Environment::GetCurrent(args);

  v8::Local<v8::Function> function =
      env->NewFunctionTemplate(Sync, v8::Local<v8::Signature>(),
                          v8::ConstructorBehavior::kAllow,
                          v8::SideEffectType::kHasSideEffect)
          ->GetFunction(isolate->GetCurrentContext())
          .ToLocalChecked();

  v8::Local<v8::Value> argv[] = {function};

  // Make a new event loop and swap out the isolate's event loop for it
  uv_loop_t* loop = env->event_loop();
  uv_stop(loop);
  uv_loop_t newLoop;
  uv_loop_init(&newLoop);
  env->isolate_data()->set_event_loop(&newLoop);

  // Call callback with `sync` parameter for synchronizing promises
  // made within the new loop (WARNING: If `sync` callback is called
  // on a promise made before entering into the synchronization context,
  // it will likely hang, as the underlying events driving that promise
  // are paused - only new events made within the callback should be safe
  // to `sync`)
  v8::MaybeLocal<v8::Value> result = func->Call(
    env->context(),
    v8::Undefined(isolate),
    arraysize(argv),
    argv);

  // Run new loop to completion even after result from callback
  while (uv_loop_alive(&newLoop)) {
      isolate->RunMicrotasks();
      uv_run(&newLoop, UV_RUN_ONCE);
  }

  // Close new loop and set isolate handle back to old one
  uv_loop_close(&newLoop);
  env->isolate_data()->set_event_loop(loop);

  if (!result.IsEmpty()) {
    args.GetReturnValue().Set(result.ToLocalChecked());
  }
}

static void Initialize(Local<Object> target,
                       Local<Value> unused,
                       Local<Context> context,
                       void* priv) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();

  env->SetMethod(target, "setTickCallback", SetTickCallback);
  env->SetMethod(target, "runMicrotasks", RunMicrotasks);
  target->Set(env->context(),
              FIXED_ONE_BYTE_STRING(isolate, "tickInfo"),
              env->tick_info()->fields().GetJSArray()).FromJust();

  Local<Object> events = Object::New(isolate);
  NODE_DEFINE_CONSTANT(events, kPromiseRejectWithNoHandler);
  NODE_DEFINE_CONSTANT(events, kPromiseHandlerAddedAfterReject);
  NODE_DEFINE_CONSTANT(events, kPromiseResolveAfterResolved);
  NODE_DEFINE_CONSTANT(events, kPromiseRejectAfterResolved);

  target->Set(env->context(),
              FIXED_ONE_BYTE_STRING(isolate, "promiseRejectEvents"),
              events).FromJust();
  env->SetMethod(target,
                 "setPromiseRejectCallback",
                 SetPromiseRejectCallback);

  env->SetMethod(target, "executeWithinNewLoop", ExecuteWithinNewLoop);
}

}  // namespace task_queue
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(task_queue, node::task_queue::Initialize)
