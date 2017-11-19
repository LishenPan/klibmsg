#include "poller.h"
#include "utils.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

namespace klibmsg {

int NetPoller::Init() {
  ep() = epoll_create1(EPOLL_CLOEXEC);

  int rc = fcntl(ep(), F_SETFD, FD_CLOEXEC);
  CHECK_NE(rc, -1);

  if (ep() == -1) {
    if (errno == ENFILE || errno == EMFILE) return -EMFILE;
  }
  nevents() = 0;
  index() = 0;

  return 0;
}

void NetPoller::Terminate() { NetCloseFd(ep()); }

void NetPoller::Add(int fd, NetPollerHandle *hndl) {
  hndl->fd = fd;
  hndl->events = 0;

  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = 0;
  ev.data.ptr = (void *)hndl;
  epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev);
}

void NetPoller::Rm(NetPollerHandle *hndl) {
  epoll_ctl(ep_, EPOLL_CTL_DEL, hndl->fd, nullptr);

  //  Invalidate any subsequent events on this file descriptor.
  for (int i = index_; i < nevents_; ++i) {
    if (events_[i].data.ptr == hndl) { events_[i].events = 0; }
  }
}

void NetPoller::SetIn(NetPollerHandle *hndl) {
  if (unlikely(hndl->events & EPOLLIN)) return;

  //  Start polling for IN.
  hndl->events |= EPOLLIN;
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = hndl->events;
  ev.data.ptr = (void *)hndl;
  epoll_ctl(ep_, EPOLL_CTL_MOD, hndl->fd, &ev);
}

void NetPoller::ResetIn(NetPollerHandle *hndl) {
  if (unlikely(!(hndl->events & EPOLLIN))) return;

  //  Stop polling for IN.
  hndl->events &= ~EPOLLIN;
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = hndl->events;
  ev.data.ptr = (void *)hndl;
  epoll_ctl(ep_, EPOLL_CTL_MOD, hndl->fd, &ev);

  //  Invalidate any subsequent IN events on this file descriptor.
  for (int i = index_; i != nevents_; ++i)
    if (events_[i].data.ptr == hndl) events_[i].events &= ~EPOLLIN;
}

void NetPoller::SetOut(NetPollerHandle *hndl) {
  if (unlikely(hndl->events & EPOLLOUT)) return;

  //  Start polling for OUT.
  hndl->events |= EPOLLOUT;
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = hndl->events;
  ev.data.ptr = (void *)hndl;
  int fd = hndl->fd;
  epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev);
}

void NetPoller::ResetOut(NetPollerHandle *hndl) {
  if (unlikely(!(hndl->events & EPOLLOUT))) return;

  //  Stop polling for OUT.
  hndl->events &= ~EPOLLOUT;
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = hndl->events;
  ev.data.ptr = (void *)hndl;
  epoll_ctl(ep_, EPOLL_CTL_MOD, hndl->fd, &ev);

  //  Invalidate any subsequent OUT events on this file descriptor.
  for (int i = index_; i != nevents_; ++i) {
    if (events_[i].data.ptr == hndl) { events_[i].events &= ~EPOLLOUT; }
  }
}

int NetPoller::Wait(int timeout) {
  //  Clear all existing events.
  nevents_ = 0;
  index_ = 0;

  //  Wait for new events.
  int nevents = 0;
  while (1) {
    nevents = epoll_wait(ep_, events_, kNetPollerMaxEvents, timeout);
    if (unlikely(nevents == -1 && errno == EINTR)) continue;
    break;
  }
  CHECK_NE(nevents_, -1);
  nevents_ = nevents;
  return 0;
}

int NetPoller::Event(int *event, NetPollerHandle **hndl) {
  while (index_ < nevents_) {
    if (events_[index_].events != 0) break;
    ++index_;
  }

  if (unlikely(index_ >= nevents_)) return -EAGAIN;

  *hndl = static_cast<NetPollerHandle *>(this->events_[index_].data.ptr);
  if (likely(events_[index_].events & EPOLLIN)) {
    *event = kNetPollerIn;
    events_[index_].events &= ~EPOLLIN;
    return 0;
  } else if (likely(events_[index_].events & EPOLLOUT)) {
    *event = kNetPollerOut;
    events_[index_].events &= ~EPOLLOUT;
    return 0;
  } else {
    *event = kNetPollerErr;
    ++index_;
    return 0;
  }
}

}  // namespace klibmsg
