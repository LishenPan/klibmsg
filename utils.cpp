#include "utils.h"
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "eventfd.h"

namespace klibmsg {

void NetCloseFd(int fd) {
  LOG(INFO) << "NetClosefd is called, what happend?\n";
  int rc;
  if (unlikely(fd < 0)) { return; }
  rc = close(fd);
  if (likely(rc == 0)) return;
  CHECK(errno == EINTR || errno == ETIMEDOUT || errno == EWOULDBLOCK
        || errno == EINPROGRESS || errno == ECONNRESET);
}

int NetEventFd::Init() {
  efd_ = eventfd(0, EFD_CLOEXEC);
  if (unlikely(efd_ == -1 && (errno == EMFILE || errno == ENFILE))) {
    return -EMFILE;
  }
  CHECK(efd_ != -1);
  int flags = fcntl(efd_, F_GETFL, 0);
  if (unlikely(flags == -1)) { flags = 0; }
  int rc = fcntl(efd_, F_SETFL, flags | O_NONBLOCK);
  CHECK_NE(rc, -1);

  return 0;
}

void NetEventFd::Terminate() {
  int fd = efd_;
  efd_ = -1;
  NetCloseFd(fd);
}

void NetEventFd::Stop() { Signal(); }

void NetEventFd::Signal() {
  const uint64_t one = 1;
  int fd = efd_;
  if (unlikely(fd < 0)) { return; }

  ssize_t nbytes = write(fd, &one, sizeof(one));
  CHECK_EQ(nbytes, sizeof(one));
}

void NetEventFd::Unsignal() {
  int fd = efd_;
  if (unlikely(fd < 0)) { return; }

  uint64_t count = 0;
  ssize_t sz = read(fd, &count, sizeof(count));
  CHECK_GT(sz, 0);
  CHECK_EQ(sz, sizeof(count));
}

void NetQueue::Init() {
  head_ = nullptr;
  tail_ = nullptr;
}

void NetQueue::Terminate() {
  head_ = nullptr;
  tail_ = nullptr;
}

int NetQueue::Empty() { return head_ ? 0 : 1; }

void NetQueue::Push(NetQueueItem* item) {
  if (item->next() != NETQUEUE_NOT_INQUEUE) { return; }
  // CHECK_EQ(item->next(), NETQUEUE_NOT_INQUEUE);

  item->next() = nullptr;
  if (!head_) head_ = item;
  if (tail_) tail_->next() = item;
  tail_ = item;
}

void NetQueue::Remove(NetQueueItem* item) {
  if (item->next() == NETQUEUE_NOT_INQUEUE) { return; }

  struct NetQueueItem* prev = nullptr;
  for (NetQueueItem* it = head_; it != nullptr; it = it->next()) {
    if (it == item) {
      if (head_ == it) head_ = it->next();
      if (tail_ == it) tail_ = prev;
      if (prev) prev->next() = it->next();
      item->next() = NETQUEUE_NOT_INQUEUE;
      return;
    }
    prev = it;
  }
}

NetQueueItem* NetQueue::Pop() {
  if (!head_) return nullptr;
  struct NetQueueItem* result = head_;
  head_ = result->next();
  if (!head_) { tail_ = nullptr; }
  result->next() = NETQUEUE_NOT_INQUEUE;
  return result;
}

void NetQueueItem::Init() { next() = NETQUEUE_NOT_INQUEUE; }

void NetQueueItem::Terminate() { CHECK_EQ(next(), NETQUEUE_NOT_INQUEUE); }

bool NetQueueItem::IsInQueue() {
  return next() == NETQUEUE_NOT_INQUEUE ? false : true;
}

int NetPool::Init() {
  cur_worker_idx_ = 0;
  bool ok = true;
  for (int i = 0; i < kNetWorkerNum; ++i) { ok = worker_[i].Init(); }
  return ok;
}

void NetPool::Terminate() {
  for (int i = 0; i < kNetWorkerNum; ++i) { worker_[i].Terminate(); }
}

NetWorker* NetPool::ChooseWorker() {
  std::unique_lock<std::mutex> lock(mutex_);
  int t = cur_worker_idx_++;
  if (cur_worker_idx_ >= kNetWorkerNum) { cur_worker_idx_ = 0; }
  return &worker_[t];
}

void NetMsgWrapper::Init() { item.Init(); }

NetMsg::NetMsg() { memset(this, 0, sizeof(NetMsg)); }

}  // namespace klibmsg
