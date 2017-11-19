#ifndef POLLER_H_
#define POLLER_H_

#include <stdint.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include "utils.h"

namespace klibmsg {

struct NetPollerHandle {
  int fd;
  uint32_t events;
};

class NetPoller {
 public:
  int Init();
  void Terminate();
  void Add(int, NetPollerHandle*);
  void Rm(NetPollerHandle*);
  void SetIn(NetPollerHandle*);
  void ResetIn(NetPollerHandle*);
  void SetOut(NetPollerHandle*);
  void ResetOut(NetPollerHandle*);
  int Wait(int timeout);
  int Event(int* event, NetPollerHandle** handle);
  inline int& ep() { return ep_; }
  inline int& nevents() { return nevents_; }
  inline int& index() { return index_; }

 private:
  int ep_;
  int nevents_;
  int index_;
  struct epoll_event events_[kNetPollerMaxEvents];
};

}  // namespace klibmsg

#endif
