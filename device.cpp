#include "device.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <string>

namespace klibmsg {

namespace {

// socktes use device handles all events in their own ctx
static void NetDeviceFsmHandler(NetFsm* self, int src, int type, void* srcptr) {
  NetDevice* device = NetDevice::Singleton();
  NetSocket* sock = nullptr;
  NetMsg* msg = nullptr;

  switch (src) {
    case kNetDeviceSrcWorkerTaskExecute:
      // TODO: Add tasks if needed
      LOG(INFO) << "No worker task in device currently, must be errror\n"
                << std::endl;
      UNEXPECTED_RUN();
      return;

    case kNetDeviceSrcSocketEvent:
      switch (type) {
        case kNetSockEventTypeConnected:
          sock = static_cast<NetSocket*>(srcptr);
          LOG(INFO) << "A socket is connected\n" << sock << std::endl;

          // sock will check its' msg_queue, if empty, it will close poller
          sock->worker()->Execute(&sock->task_send());
          return;
        case kNetSockEventTypeSent: LOG(INFO) << "a msg is sent\n"; return;

        case kNetSockEventTypeReceived:
          LOG(INFO) << "A msg is received\n";
          msg = (NetMsg*)(srcptr);
          device->ParseNetMsg(*msg);
          NetMsg::FreeNetMsg(msg);
          return;

        case kNetSockEventTypeError:
          LOG(INFO) << "Event type error is received" << sock << std::endl;
          UNEXPECTED_RUN();
          return;

        case kNetSockEventTypeAcceptError:
          LOG(INFO) << "Accept error is received" << sock << std::endl;
          UNEXPECTED_RUN();
          return;

        case kNetSockEventTypeAccepted:
          sock = static_cast<NetSocket*>(srcptr);
          LOG(INFO) << "A socket is accepted " << sock << std::endl;
          sock->Activate();
          sock->worker()->Execute(&sock->task_recv());
          klibmsg::NetDevice::Singleton()->PairListenSocket();
          return;

        case kNetSockEventTypeShutdown:
          sock = static_cast<NetSocket*>(srcptr);
          LOG(INFO) << "A socket is shutdown " << sock << std::endl;
          delete sock;
          return;

        default:
          // all other events are ignored.
          LOG(INFO) << "Catched event that currently device \
            is not going to handle\n";
      }
  }
}

static void NetDeviceFsmShutdownHandler(NetFsm* self, int src, int type,
                                        void* srcptr) {}

bool IsIpEqual(const struct sockaddr_in& ip1, const struct sockaddr_in& ip2) {
  return (ip1.sin_family == ip2.sin_family && ip1.sin_port == ip2.sin_port
          && ip1.sin_addr.s_addr == ip2.sin_addr.s_addr);
}

}  // namespace

void NetDevice::Init(std::function<void*(int64_t)> f1,
                     std::function<int64_t(int64_t)> f2,
                     std::function<struct sockaddr_in(int64_t)> f3) {
  // Init global singletons such as pool
  NetPool::Singleton()->Init();
  // TODO init ip addresses and ports
  Token2Dptr = f1;
  Token2DptrSize = f2;
  Token2Ip = f3;

  device_handle_ = NetDeviceFsmHandler;
  device_shutdown_handle_ = NetDeviceFsmShutdownHandler;

  // fsm in net device has no owner && ctx
  fsm_.Init(-1, device_handle_, device_shutdown_handle_, nullptr, nullptr);

  ip_addr_ = Token2Ip(kLocalIpToken);
  listen_sock_.Init(kNetDeviceSrcSocketEvent, &fsm_, ip_addr_);
  listen_sock_.Start(AF_INET, SOCK_STREAM, 0);
  listen_sock_.Bind();
  listen_sock_.Listen(kListenBacklog);
  PairListenSocket();
}

int NetDevice::SendData(const void* src_token, const void* dst_token) {
  NetMsg msg;
  msg.msg_type = 1;
  msg.src.token = (int64_t)src_token;
  msg.dst.token = (int64_t)dst_token;

  void* src_dptr = Token2Dptr(msg.src.token);
  int64_t src_sz = Token2DptrSize(msg.src.token);

  if (unlikely(src_dptr == nullptr)) {
    LOG(INFO) << "src_dptr is nullptr of token:" << msg.src.token << std::endl;
    return -1;
  }
  if (unlikely(src_sz == 0)) {
    LOG(INFO) << "src memory size is zero" << msg.src.token << std::endl;
    return -1;
  }
  msg.src.address = src_dptr;
  msg.extra.data_len = src_sz;

  SendMsg(&msg);

  return 0;
}

int NetDevice::ReadData(const void* src_token, const void* dst_token,
                        std::function<void()> callback) {
  NetMsg msg;
  msg.src.token = (int64_t)(dst_token);
  msg.dst.token = (int64_t)(src_token);
  msg.msg_type = 0;
  msg.extra.cmd = kNetMsgCmdRead;
  token2callback_.emplace(msg.dst.token, callback);

  return SendMsg(&msg);
}

void NetDevice::PollCompletionQueue() {
  std::function<void()> func;
  while (true) {
    if (cq_.Receive(&func) == 0) {
      func();
    } else {
      break;
    }
  }
}

void NetDevice::CloseCompletionQueue() {
  cq_.CloseSendEnd();
  cq_.CloseReceiveEnd();
}

int NetDevice::SendMsg(NetMsg* msg) {
  LOG(INFO) << "SendMsg is called, starting check sockets\n";

  // check if the dst machine is connected;
  NetSocket* out_sock = Token2Socket(msg->dst.token);

  // check if any connected socket has the same ip address with this dst token
  if (out_sock == nullptr) {
    const struct sockaddr_in dst_addr = Token2Ip(msg->dst.token);
    for (const auto iter : token2sock_) {
      NetSocket* sock = iter.second;
      if (IsIpEqual(sock->ip(), dst_addr)) {
        token2sock_.emplace(msg->dst.token, sock);
        out_sock = sock;
        break;
      }
    }
  }

  // no previous connection to the dst machine, create new conection.
  if (out_sock == nullptr) {
    LOG(INFO) << "out_sock for dst token:" << msg->dst.token
              << " is nullptr, constructing new socket";
    out_sock = new NetSocket;
    sock_sets_.insert(out_sock);
    out_sock->Init(kNetDeviceSrcSocketEvent, &fsm_, Token2Ip(msg->dst.token));
    token2sock_.emplace(msg->dst.token, out_sock);
    CHECK(out_sock->IsIdle());
  }

  if (out_sock->IsIdle()) {
    out_sock->Start(AF_INET, SOCK_STREAM, 0);
    out_sock->Connect();
  }

  out_sock->AsyncSendMsg(*msg);

  return 0;
}

// process received msg
int NetDevice::ParseNetMsg(const NetMsg& msg) {
  LOG(INFO) << "Starting parsing msg\n";
  if (msg.msg_type == 0) {
    // cmd msg --- currently only read is supported
    if (msg.extra.cmd == kNetMsgCmdRead) {
      NetMsg out_msg;
      out_msg.msg_type = 1;
      out_msg.src.address = Token2Dptr(msg.src.token);
      out_msg.dst.token = msg.dst.token;
      out_msg.extra.data_len = Token2DptrSize(msg.src.token);
      CHECK_NE(out_msg.extra.data_len, -1);

      SendMsg(&out_msg);
    } else {
      UNEXPECTED_RUN();
    }
  } else if (msg.msg_type == 1) {
    // data msg, call the callback
    auto it = token2callback_.find(msg.dst.token);
    if (it != token2callback_.end()) {
      cq_.Send(it->second);
      token2callback_.erase(it);
    }
  } else {
    UNEXPECTED_RUN();
  }
  return 0;
}

void NetDevice::PairListenSocket() {
  NetSocket* sock = new NetSocket;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  sock->Init(kNetDeviceSrcSocketEvent, &fsm_, addr);
  sock_sets_.insert(sock);

  // sock is initialized but not started yet, waiting new connection assigned
  // by listen_sock_
  sock->Accept(&listen_sock_);
}

NetSocket* NetDevice::Token2Socket(const int64_t token) {
  auto it = token2sock_.find(token);
  if (it == token2sock_.end()) { return nullptr; }
  return it->second;
}

NetDevice::~NetDevice() {
  for (NetSocket* sock : sock_sets_) { sock->AsyncStop(); }
}

}  // namespace klibmsg
