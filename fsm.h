#ifndef FSM_H_
#define FSM_H_

#include <functional>
#include "utils.h"

namespace klibmsg {

class NetFsm;
typedef std::function<void(NetFsm*, int, int, void*)> NetFsmHandleFunc;
class NetFsmEvent {
 public:
  NetFsmEvent() = default;
  ~NetFsmEvent() = default;
  inline NetFsm*& fsm() { return fsm_; }
  inline int& src() { return src_; }
  inline void*& srcptr() { return srcptr_; }
  inline int& type() { return type_; }
  inline NetQueueItem& item() { return item_; }
  void Init();
  void Terminate();
  int Active();
  void Process();

  // memory pool
  static NetFsmEvent* NewNetFsmEvent() { return new NetFsmEvent; }
  static void FreeFsmEvent(NetFsmEvent* event) { delete event; }

  // set as public to use container_of
  NetQueueItem item_;

 private:
  NetFsm* fsm_;
  int src_;
  void* srcptr_;
  int type_;
};

class NetCtx;
class NetWorker;
class NetFsm {
 public:
  DISALLOW_COPY_AND_MOVE(NetFsm);
  NetFsm() = default;
  ~NetFsm() = default;
  void Init(NetFsmHandleFunc fn, NetFsmHandleFunc shutdown_fn, int src,
            void* srcptr, NetFsm* owner);
  void Init(int src, NetFsmHandleFunc fn, NetFsmHandleFunc shutdown_fn,
            NetFsm* owner, NetCtx* ctx);
  void Handle(int src, int type, void* srcptr);
  void ShutdownHandle(int src, int type, void* srcptr);
  void Terminate();
  int IsIdle();
  void Start();
  void Stop();
  // stop the fsm with member event stopped_
  void Stopped(int type);
  void StoppedNoEvent();
  NetWorker* ChooseWorker();
  void Action(int type);

  // Raise event to ctx, will be executed by workers
  void Raise(NetFsmEvent* event, int type, void* srcptr = nullptr);
  void RaiseTo(NetFsm* dst_fsm, NetFsmEvent* event, int src, int type,
               void* srcptr);

  // execute directly in current thread, called only by internal states
  void Feed(int src, int type, void* srcptr);
  inline int& state() { return state_; }
  inline int& src() { return src_; }
  inline void*& srcptr() { return srcptr_; }
  inline NetFsm*& owner() { return owner_; }
  inline NetCtx*& ctx() { return ctx_; }

 private:
  int state_;
  int src_;
  void* srcptr_;
  NetFsm* owner_;
  NetCtx* ctx_;
  NetFsmEvent stopped_;
  NetFsmHandleFunc handle_;
  NetFsmHandleFunc shutdown_handle_;
};

}  //  namespace klibmsg
#endif
