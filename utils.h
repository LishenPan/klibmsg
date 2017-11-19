#ifndef UTILS_H_
#define UTILS_H_

#include <errno.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cstdio>
#include "worker.h"

namespace klibmsg {

#define UNUSED

//namespace {

const int kNetFsmAction = -2;
const int kNetFsmStart = -2;
const int kNetFsmStop = -3;

const int kNetFsmStateIdle = 1;
const int kNetFsmStateActive = 2;
const int kNetFsmStateStopping = 3;

const int kNetSockStateIdle = 1;
const int kNetSockStateStarting = 2;
const int kNetSockStateBeingAccepted = 3;
const int kNetSockStateAccepted = 4;
const int kNetSockStateConnecting = 5;
const int kNetSockStateActive = 6;
const int kNetSockStateRemovingFd = 7;
const int kNetSockStateDone = 8;
const int kNetSockStateListening = 9;
const int kNetSockStateAccepting = 10;
const int kNetSockStateCancelling = 11;
const int kNetSockStateStopping = 12;
const int kNetSockStateStoppingAccept = 13;
const int kNetSockStateAcceptingError = 14;

const int kNetSockActionAccept = 1;
const int kNetSockActionBeingAccepted = 2;
const int kNetSockActionCancel = 3;
const int kNetSockActionListen = 4;
const int kNetSockActionConnect = 5;
const int kNetSockActionActivate = 6;
const int kNetSockActionDone = 7;
const int kNetSockActionError = 8;
const int kNetSockActionStarted = 9;

// sock action sources
const int kNetSockSrcFd = 1;
const int kNetSockSrcTaskConnecting = 2;
const int kNetSockSrcTaskConnected = 3;
const int kNetSockSrcTaskAccept = 4;
const int kNetSockSrcTaskSend = 5;
const int kNetSockSrcTaskRecv = 6;
const int kNetSockSrcTaskStop = 7;

// event types raised by NetSocket
const int kNetSockEventTypeConnected = 1;
const int kNetSockEventTypeAccepted = 2;
const int kNetSockEventTypeSent = 3;
const int kNetSockEventTypeReceived = 4;
const int kNetSockEventTypeError = 5;
const int kNetSockEventTypeAcceptError = 6;
const int kNetSockEventTypeStopped = 7;
const int kNetSockEventTypeShutdown = 8;

const int64_t kNetSocketBufferSize = 128;

// poller
const int kNetPollerIn = 1;
const int kNetPollerOut = 2;
const int kNetPollerErr = 3;

const int kNetPollerHaveAsyncAdd = 1;
const int kNetPollerMaxEvents = 32;

// workerfd
const int kNetWorkerFdIn = kNetPollerIn;
const int kNetWorkerFdOut = kNetPollerOut;
const int kNetWorkerFdErr = kNetPollerErr;

const int kNetWorkerTaskExecute = 1;

// net device
const int kNetDeviceSrcWorkerTaskExecute = 1;
const int kNetDeviceSrcSocketEvent = 2;
const int kPortNum = 8341;
const int kLocalIpToken = -1;
const int kListenBacklog = 100;
const int kNetMsgCmdRead = 1;

// pool
const int kNetWorkerNum = 4;

template<class T, class M>
T* container_of_impl(M* ptr, const M T::*member) {
  return (T*)((char*)ptr - ((size_t) & (reinterpret_cast<T*>(0)->*member)));
}

void kLibMsgFsmError(const char* err_str, int state, int src, int type) {
  printf("%s\n", err_str);
  printf("src: %d, state: %d, type: %d\n", state, src, type);

  // let it down
  // LOG(ERROR) << std::string(err_str) << std::endl;
}

//}  // namespace

#define likely(x) __builtin_expect((x), 1)

#define unlikely(x) __builtin_expect((x), 0)
#define container_of(ptr, type, member) container_of_impl(ptr, &type::member)
#define NetFsmBadAction(state, src, type) \
  kLibMsgFsmError("Unexpected action", state, src, type)

#define NetFsmBadState(state, src, type) \
  kLibMsgFsmError("Unexpected state", state, src, type)

#define NetFsmBadSource(state, src, type) \
  kLibMsgFsmError("Unexpected source", state, src, type)

#define SINGLETON(ClassName)               \
  static ClassName* Singleton() {          \
    static ClassName* ptr = new ClassName; \
    return ptr;                            \
  }

#define TODO() LOG(FATAL) << "TODO";

#define DISALLOW_COPY(ClassName)        \
  ClassName(const ClassName&) = delete; \
  ClassName& operator=(const ClassName&) = delete;

#define DISALLOW_MOVE(ClassName)   \
  ClassName(ClassName&&) = delete; \
  ClassName& operator=(ClassName&&) = delete;

#define DISALLOW_COPY_AND_MOVE(ClassName) \
  DISALLOW_COPY(ClassName)                \
  DISALLOW_MOVE(ClassName)

#define UNEXPECTED_RUN() LOG(FATAL) << "Unexpected Run";

void NetCloseFd(int fd);

class NetEventFd final {
 public:
  NetEventFd() = default;
  ~NetEventFd() = default;

  int Init();
  void Terminate();
  void Stop();
  void Signal();
  void Unsignal();
  inline int& efd() { return efd_; }

 private:
  int efd_;
};

// worker thread pool

class NetPool final {
 public:
  DISALLOW_COPY_AND_MOVE(NetPool);
  SINGLETON(NetPool);

  ~NetPool();

  int Init();
  void Terminate();
  NetWorker* ChooseWorker();

 private:
  NetPool() = default;
  NetWorker worker_[kNetWorkerNum];
  mutable std::mutex mutex_;
  int cur_worker_idx_;
};

#define NETQUEUE_NOT_INQUEUE ((struct NetQueueItem*)-1)

class NetQueueItem final {
 public:
  void Init();
  void Terminate();
  bool IsInQueue();
  inline NetQueueItem*& next() { return next_; }

 private:
  NetQueueItem* next_;
};

class NetQueue final {
 public:
  NetQueue() = default;
  ~NetQueue() = default;
  void Init();
  void Terminate();
  int Empty();
  void Push(struct NetQueueItem* item);
  void Remove(struct NetQueueItem* item);
  NetQueueItem* Pop();

 private:
  NetQueueItem* head_;
  NetQueueItem* tail_;
};

class NetMsg final {
 public:
  NetMsg();
  ~NetMsg() = default;
  static NetMsg* NewNetMsg() { return new NetMsg; }
  static void FreeNetMsg(NetMsg* msg) { delete msg; }

  // msg type: 0 -- control msg; 1 -- data msg;
  int32_t msg_type : 2;

  // reserved
  int32_t : 6;

  union {
    int64_t token;
    void* address;
  } src;

  union {
    int64_t token;
    void* address;
  } dst;

  union {
    int64_t data_len;
    int64_t cmd;
  } extra;
};

class NetMsgWrapper final {
 public:
  NetMsg msg;
  NetQueueItem item;
  void Init();

  static NetMsgWrapper* New(const NetMsg& msg) {
    NetMsgWrapper* wrapper = new NetMsgWrapper;
    wrapper->msg = msg;
    return wrapper;
  }

  static void Free(NetMsgWrapper* wrapper) { delete wrapper; }
};

}  // namespace klibmsg

#endif
