#ifndef SOCKET_H_
#define SOCKET_H_

#include <netinet/in.h>
#include <sys/socket.h>
#include <mutex>
#include "ctx.h"
#include "fsm.h"
#include "msg.h"
#include "utils.h"
#include "worker.h"

namespace klibmsg {

class NetSocket final {
 public:
  DISALLOW_COPY_AND_MOVE(NetSocket);
  NetSocket() = default;
  ~NetSocket() = default;

  // interfaces for user thread
  void Init(int src, NetFsm* owner, const struct sockaddr_in& addr);
  int Start(int domain, int type, int protocol);
  int Bind();
  int Listen(int backlog);
  void Connect();
  void Accept(NetSocket* listener);
  int SetSockOpt(int level, int optname, const void* optval, size_t optlen);
  void AsyncSendMsg(const NetMsg& msg);
  void AsyncStop();
  void Terminate();

  // public functions called by internal && external handlers
  void InitFromFd(int s);
  int IsIdle();
  void Stop();
  void Activate();
  void Recv();
  int RecvRaw();
  int SendRaw();
  void StartFd(int fd);
  int GetErr();
  int InternalTasks(int src, int type);
  void ExtractNetMsg(NetMsg* msg);
  bool DequeueNetMsgToBuf();

  // getters && setters
  inline int& s() { return s_; }
  inline NetFsm& fsm() { return fsm_; }
  inline int& state() { return state_; }
  inline NetWorker*& worker() { return worker_; }
  inline int& errnum() { return errnum_; }
  inline NetSocket*& asock() { return asock_; }
  inline NetWorkerFd& wfd() { return wfd_; }
  inline NetWorkerTask& task_connecting() { return task_connecting_; }
  inline NetWorkerTask& task_connected() { return task_connected_; }
  inline NetWorkerTask& task_accept() { return task_accept_; }
  inline NetWorkerTask& task_send() { return task_send_; }
  inline NetWorkerTask& task_recv() { return task_recv_; }
  inline NetWorkerTask& task_stop() { return task_stop_; }
  inline NetFsmEvent& event_established() { return event_established_; }
  inline NetFsmEvent& event_sent() { return event_sent_; }
  inline NetFsmEvent& event_received() { return event_received_; }
  inline NetFsmEvent& event_error() { return event_error_; }
  inline std::mutex& msq_sync() { return msq_sync_; }
  inline NetQueue& msg_buffer_queue() { return msg_buffer_queue_; }
  inline bool& is_out_buf_busy() { return is_out_buf_busy_; }
  inline NetCtx& ctx() { return ctx_; }
  inline const struct sockaddr_in& ip() { return ip_addr_; }

  // set as public to use container_of
  NetFsm fsm_;

 private:
  // buffers
  char in_buf[kNetSocketBufferSize];
  char out_buf[kNetSocketBufferSize];
  int in_offset;
  int out_offset;
  bool is_out_buf_busy_ = false;
  struct sockaddr_in ip_addr_;

  int state_;
  NetWorker* worker_;

  int s_;
  NetWorkerFd wfd_;
  NetCtx ctx_;

  //  Asynchronous tasks for the worker.
  NetWorkerTask task_connecting_;
  NetWorkerTask task_connected_;
  NetWorkerTask task_accept_;
  NetWorkerTask task_send_;
  NetWorkerTask task_recv_;
  NetWorkerTask task_stop_;

  //  Events raised by the NetSocket.
  NetFsmEvent event_established_;
  NetFsmEvent event_sent_;
  NetFsmEvent event_received_;
  NetFsmEvent event_error_;

  //  In accepting state points to the socket being accepted.
  //  In being_accepted state points to the listener socket.
  NetSocket* asock_;

  //  Errno remembered in net error state
  int errnum_;

  // msg buffer for stored outbound msgs
  std::mutex msq_sync_;
  NetQueue msg_buffer_queue_;
};

}  // namespace klibmsg

#endif
