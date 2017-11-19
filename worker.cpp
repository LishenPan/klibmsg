#include "ctx.h"
#include "utils.h"

namespace klibmsg {

void NetWorkerFd::Init(int src, NetFsm* owner) {
  src_ = src;
  owner_ = owner;
}

void NetWorkerFd::Terminate() {}

void NetWorker::AddFd(int s, NetWorkerFd* fd) { poller_.Add(s, &fd->handle()); }

void NetWorker::RmFd(NetWorkerFd* fd) { poller_.Rm(&fd->handle()); }

void NetWorker::SetIn(NetWorkerFd* fd) { poller_.SetIn(&fd->handle()); }

void NetWorker::ResetIn(NetWorkerFd* fd) { poller_.ResetIn(&fd->handle()); }

void NetWorker::SetOut(NetWorkerFd* fd) { poller_.SetOut(&fd->handle()); }

void NetWorker::ResetOut(NetWorkerFd* fd) { poller_.ResetOut(&fd->handle()); }

void NetWorkerTask::Init(int src, NetFsm* owner) {
  src_ = src;
  owner_ = owner;
  item_.Init();
}

void NetWorkerTask::Terminate() { item_.Terminate(); }

int NetWorker::Init() {
  int rc = efd_.Init();
  if (rc < 0) { return rc; }

  tasks_.Init();
  stop_.Init();
  poller_.Init();
  poller_.Add(efd_.efd(), &efd_handle_);
  poller_.SetIn(&efd_handle_);
  routine_thread_ = std::thread([this]() { this->NetWorkerRoutine(); });

  return 0;
}

void NetWorker::Terminate() {
  //  Ask worker thread to terminate.
  std::unique_lock<std::mutex> lock(sync_);
  tasks_.Push(&stop_);
  efd_.Signal();
  lock.unlock();

  //  Wait till worker thread terminates. task_stop must be called before
  routine_thread_.join();

  // clean up
  poller_.Terminate();
  efd_.Terminate();
  stop_.Terminate();
  tasks_.Terminate();
}

void NetWorker::Execute(NetWorkerTask* task) {
  std::unique_lock<std::mutex> lock(sync_);
  tasks_.Push(&task->item());
  efd_.Signal();
}

void NetWorker::Cancel(NetWorkerTask* task) {
  std::unique_lock<std::mutex> lock(sync_);
  tasks_.Remove(&task->item());
}

void NetWorker::NetWorkerRoutine() {
  while (1) {
    int rc = poller_.Wait(-1);

    // process poller events
    while (1) {
      int pevent = 0;
      NetPollerHandle* phandle = nullptr;
      rc = poller_.Event(&pevent, &phandle);
      if (unlikely(rc == -EAGAIN)) break;

      // process worker tasks
      if (phandle == &efd_handle_) {
        CHECK_EQ(pevent, kNetPollerIn);

        std::unique_lock<std::mutex> lock(sync_);
        efd_.Unsignal();
        NetQueue tasks;
        memcpy(&tasks, &tasks_, sizeof(tasks_));
        tasks_.Init();
        lock.unlock();

        while (1) {
          NetQueueItem* item = tasks.Pop();
          if (unlikely(!item)) break;
          if (unlikely(item == &stop_)) {
            while (tasks.Pop() != nullptr) { continue; }
            tasks.Terminate();
            return;
          }

          // user defined task
          NetWorkerTask* task = container_of(item, NetWorkerTask, item_);
          CHECK_NE(task->owner(), (void*)0);
          CHECK_NE(task->owner()->ctx(), (void*)0);
          task->owner()->ctx()->ExecuteCriticalSection([task]() {
            task->owner()->Feed(task->src(), kNetWorkerTaskExecute, task);
          });
        }
        tasks.Terminate();
        continue;
      }

      // It is a true IO event, handle it
      NetWorkerFd* fd = container_of(phandle, NetWorkerFd, handle_);
      CHECK_NE(fd, (void*)0);
      CHECK_NE(fd->owner(), (void*)0);
      CHECK_NE(fd->owner()->ctx(), (void*)0);
      fd->owner()->ctx()->ExecuteCriticalSection(
          [fd, pevent]() { fd->owner()->Feed(fd->src(), pevent, fd); });
    }
  }
}

}  // namespace klibmsg
