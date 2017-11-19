#include "fsm.h"
#include "ctx.h"
#include "utils.h"

#include <stddef.h>

namespace klibmsg {

void NetFsmEvent::Init() {
  fsm_ = nullptr;
  src_ = -1;
  srcptr_ = nullptr;
  type_ = -1;
  item_.Init();
}

int NetFsmEvent::Active() { return item_.IsInQueue(); }
void NetFsmEvent::Terminate() {}

void NetFsmEvent::Process() {
  int src = src_;
  int type = type_;
  void* srcptr = srcptr_;
  src_ = -1;
  type_ = -1;
  srcptr_ = nullptr;

  CHECK_NE((int64_t)fsm_->owner(), 0);
  fsm_->owner()->Feed(src, type, srcptr);
}

void NetFsm::Feed(int src, int type, void* srcptr) {
  if (likely(state() != kNetFsmStateStopping)) {
    Handle(src, type, srcptr);
  } else {
    ShutdownHandle(src, type, srcptr);
  }
}

void NetFsm::Init(const NetFsmHandleFunc fn, const NetFsmHandleFunc shutdown_fn,
                  int src, void* srcptr, NetFsm* owner) {
  handle_ = fn;
  shutdown_handle_ = shutdown_fn;
  state_ = kNetFsmStateIdle;
  src_ = src;
  srcptr_ = srcptr;
  owner_ = owner;
  ctx_ = owner->ctx();
  stopped_.Init();
}

void NetFsm::Init(int src, const NetFsmHandleFunc fn,
                  const NetFsmHandleFunc shutdown_fn, NetFsm* owner,
                  NetCtx* ctx) {
  handle_ = fn;
  shutdown_handle_ = shutdown_fn;
  state_ = kNetFsmStateIdle;
  src_ = src;
  srcptr_ = nullptr;
  owner_ = owner;
  ctx_ = ctx;
  stopped_.Init();
}

void NetFsm::Handle(int src, int type, void* srcptr) {
  handle_(static_cast<NetFsm*>(this), src, type, srcptr);
}

void NetFsm::ShutdownHandle(int src, int type, void* srcptr) {
  shutdown_handle_(static_cast<NetFsm*>(this), src, type, srcptr);
}

void NetFsm::Terminate() {
  CHECK(IsIdle());
  stopped_.Terminate();
}

void NetFsm::Start() {
  CHECK(IsIdle());
  Handle(kNetFsmAction, kNetFsmStart, nullptr);
  state_ = kNetFsmStateActive;
}

int NetFsm::IsIdle() {
  return (state_ == kNetFsmStateIdle && !stopped_.Active()) ? 1 : 0;
}

void NetFsm::Stop() {
  //  If stopping of the state machine was already requested, do nothing.
  if (state() != kNetFsmStateActive) return;
  state_ = kNetFsmStateStopping;
  ShutdownHandle(kNetFsmAction, kNetFsmStop, nullptr);
}

void NetFsm::Stopped(int type) {
  CHECK_EQ(state_, kNetFsmStateStopping);
  Raise(&stopped_, type);
  state_ = kNetFsmStateIdle;
}

void NetFsm::StoppedNoEvent() {
  CHECK_EQ(state_, kNetFsmStateStopping);
  state() = kNetFsmStateIdle;
}

NetWorker* NetFsm::ChooseWorker() { return ctx_->ChooseWorker(); }

void NetFsm::Action(int type) {
  CHECK_GT(type, 0);
  Feed(kNetFsmAction, type, nullptr);
}

void NetFsm::Raise(NetFsmEvent* event, int type, void* srcptr) {
  LOG(INFO) << "Raising event...\n";
  event->fsm() = static_cast<NetFsm*>(this);
  event->src() = src_;
  if (srcptr == nullptr) {
    event->srcptr() = srcptr_;
  } else {
    event->srcptr() = srcptr;
  }
  event->type() = type;
  ctx_->Raise(event);
}

void NetFsm::RaiseTo(NetFsm* dst_fsm, NetFsmEvent* event, int src, int type,
                     void* srcptr) {
  event->fsm() = dst_fsm;
  event->src() = src;
  event->type() = type;
  event->srcptr() = srcptr;
  ctx_->RaiseTo(event);
}

}  // namespace klibmsg
