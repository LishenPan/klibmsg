#ifndef DEVICE_H_
#define DEVICE_H_

#include <unordered_set>
#include "ctx.h"
#include "socket.h"
#include "channel.h"
#include <unordered_map>

namespace klibmsg {

class NetDevice final {
 public:
  DISALLOW_COPY_AND_MOVE(NetDevice);
  ~NetDevice();

  SINGLETON(NetDevice);

  // Interface for user thread
  // Every token must be global unique on every machine, every caller from the
  // up side must know the tokens
  void Init(std::function<void*(int64_t)> f1,
            std::function<int64_t(int64_t)> f2,
            std::function<struct sockaddr_in(int64_t)> f3);
  int ReadData(const void* src_token, const void* dst_token,
               std::function<void()> callback);
  int SendData(const void* src_token, const void* dst_token);
  // called by another thread to execute callbacks.
  void PollCompletionQueue();
  void CloseCompletionQueue();

  // Functions for internal && external handlers
  void PairListenSocket();
  int ParseNetMsg(const NetMsg& msg);

  // Resouces for outbound sockets
  std::function<int64_t(int64_t)> Token2DptrSize;  // return -1 if fail
  std::function<void*(int64_t)> Token2Dptr;        // return nullptr if fail
  std::function<struct sockaddr_in(int64_t)> Token2Ip;  // should never fail

  NetSocket* Token2Socket(const int64_t token);
  inline NetFsm& fsm() { return fsm_; }
  inline NetSocket& listen_sock() { return listen_sock_; }

  // Public to use container_of
  NetFsm fsm_;

 private:
  NetDevice() = default;
  int SendMsg(NetMsg* msg);

  struct sockaddr_in ip_addr_;
  NetSocket listen_sock_;

  // internal useage
  std::unordered_map<int64_t, NetSocket*> token2sock_;
  std::unordered_map<int64_t, std::function<void()>> token2callback_;
  NetFsmHandleFunc device_handle_;
  NetFsmHandleFunc device_shutdown_handle_;

  // socket resources
  std::unordered_set<NetSocket*> sock_sets_;

  // completion queue
  Channel<std::function<void()>> cq_;
};

}  // namespace klibmsg

#endif
