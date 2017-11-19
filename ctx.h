#ifndef CTX_H_
#define CTX_H_

#include <functional>
#include <mutex>
#include "fsm.h"
#include "worker.h"
#include "utils.h"

namespace klibmsg {

class NetCtx {
 public:
  DISALLOW_COPY_AND_MOVE(NetCtx);
  NetCtx() = default;
  ~NetCtx() = default;

  inline std::mutex& sync() { return sync_; }
  inline NetQueue& events() { return events_; }
  inline NetQueue& eventsto() { return eventsto_; }

  void Init(std::function<void(NetCtx*)> on_leave);
  void Terminate();
  void ExecuteCriticalSection(std::function<void()>);
  NetWorker* ChooseWorker();
  void Raise(NetFsmEvent* event);
  void RaiseTo(NetFsmEvent* event);

 private:
  std::mutex sync_;
  NetQueue events_;
  // external events
  NetQueue eventsto_;
  std::function<void(NetCtx*)> ctx_onleave_handle_;
};

}  // namespace klibmsg

#endif
