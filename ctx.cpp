#include "ctx.h"
#include "utils.h"

namespace klibmsg {

void NetCtx::Init(std::function<void(NetCtx*)> on_leave) {
  ctx_onleave_handle_ = on_leave;

  events_.Init();
  eventsto_.Init();
}

void NetCtx::Terminate() {
  eventsto_.Terminate();
  events_.Terminate();
}

void NetCtx::ExecuteCriticalSection(std::function<void()> func) {
  // enter ctx
  std::unique_lock<std::mutex> lock(sync_);
  func();

  // leaving ctx
  // Process any queued events before leaving the context.
  while (1) {
    NetQueueItem* item = events_.Pop();
    NetFsmEvent* event = container_of(item, NetFsmEvent, item_);
    if (!event) break;
    event->Process();
  }

  if (ctx_onleave_handle_ != nullptr) { (ctx_onleave_handle_)(this); }

  if (eventsto_.Empty()) { return; }

  //  Make a copy of the queue of the external events so that it does not
  //  get corrupted once we unlock the context.
  NetQueue eventsto = eventsto_;
  eventsto_.Init();

  lock.unlock();

  while (1) {
    NetQueueItem* item = eventsto.Pop();
    NetFsmEvent* event = container_of(item, NetFsmEvent, item_);
    if (!event) break;
    event->fsm()->ctx()->ExecuteCriticalSection(
        [event]() { event->Process(); });
  }

  eventsto.Terminate();
}

NetWorker* NetCtx::ChooseWorker() {
  return NetPool::Singleton()->ChooseWorker();
}

void NetCtx::Raise(NetFsmEvent* event) { events_.Push(&event->item()); }
void NetCtx::RaiseTo(NetFsmEvent* event) { eventsto_.Push(&event->item()); }

}  // namespace klibmsg
