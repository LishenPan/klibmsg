#ifndef WORKER_H_
#define WORKER_H_

#include <mutex>
#include <thread>
#include "poller.h"
#include "utils.h"

namespace klibmsg {

class NetFsm;
class NetWorkerFd final {
 public:
  NetWorkerFd() = default;
  ~NetWorkerFd() = default;

  void Init(int src, NetFsm* owner);
  void Terminate();
  inline NetPollerHandle& handle() { return handle_; }
  inline NetFsm* owner() { return owner_; }
  inline int& src() { return src_; }

  // public to use container_of
  NetPollerHandle handle_;

 private:
  int src_;
  NetFsm* owner_;
};

class NetWorkerTask final {
 public:
  void Init(int src, NetFsm* owner);
  void Terminate();
  inline NetQueueItem& item() { return item_; }
  inline int& src() { return src_; }
  inline NetFsm*& owner() { return owner_; }
  inline void*& srcptr() { return srcptr_; }

  // public to use container_of
  NetQueueItem item_;

 private:
  int src_;
  NetFsm* owner_;
  void* srcptr_;
  std::function<void()> callback_func_;
};

class NetWorker final {
 public:
  NetWorker() = default;
  ~NetWorker() = default;
  int Init();
  void Terminate();
  void Execute(NetWorkerTask* task);
  void AddFd(int s, NetWorkerFd* fd);
  void RmFd(NetWorkerFd* fd);
  void SetIn(NetWorkerFd* fd);
  void ResetIn(NetWorkerFd* fd);
  void SetOut(NetWorkerFd* fd);
  void ResetOut(NetWorkerFd* fd);
  void Cancel(NetWorkerTask* task);
  inline NetPoller& poller() { return poller_; }
  inline NetPollerHandle& efd_handle() { return efd_handle_; }
  inline NetEventFd& efd() { return efd_; }

 private:
  void NetWorkerRoutine();
  mutable std::mutex sync_;
  NetQueue tasks_;
  NetQueueItem stop_;
  NetEventFd efd_;
  NetPoller poller_;
  NetPollerHandle efd_handle_;
  std::thread routine_thread_;
};

}  // namespace klibmsg

#endif
