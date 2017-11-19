#include "socket.h"
#include <fcntl.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include "device.h"

namespace klibmsg {

static void NetSocketHandler(NetFsm *self, int src, int type, void *srcptr);
static void NetSocketShutdownHandler(NetFsm *self, int src, int type,
                                     void *srcptr);

void NetSocket::Init(int src, NetFsm *owner, const struct sockaddr_in &addr) {
  ctx_.Init(std::function<void(NetCtx *)>());
  fsm_.Init(src, NetSocketHandler, NetSocketShutdownHandler, owner, &ctx_);

  state() = kNetSockStateIdle;

  //  Choose a worker thread to handle this socket.
  worker_ = fsm_.ChooseWorker();

  //  Actual file descriptor will be generated during 'start' step.
  s_ = -1;
  errnum_ = 0;

  wfd().Init(kNetSockSrcFd, &fsm());
  task_connecting().Init(kNetSockSrcTaskConnecting, &fsm());
  task_connected().Init(kNetSockSrcTaskConnected, &fsm());
  task_accept().Init(kNetSockSrcTaskAccept, &fsm());
  task_send().Init(kNetSockSrcTaskSend, &fsm());
  task_recv().Init(kNetSockSrcTaskRecv, &fsm());
  task_stop().Init(kNetSockSrcTaskStop, &fsm());

  event_established().Init();
  event_sent().Init();
  event_received().Init();
  event_error().Init();

  // init input && output buffers
  in_offset = 0;
  out_offset = 0;

  // init msg buffer queue
  msg_buffer_queue_.Init();

  //  accepting is not going on at the moment.
  asock_ = nullptr;
  ip_addr_ = addr;
}

void NetSocket::Terminate() {
  CHECK_EQ(state_, kNetSockStateIdle);

  event_error().Terminate();
  event_received().Terminate();
  event_sent().Terminate();
  event_established().Terminate();

  worker()->Cancel(&task_recv());

  task_stop().Terminate();
  task_recv().Terminate();
  task_send().Terminate();
  task_accept().Terminate();
  task_connected().Terminate();
  task_connecting().Terminate();

  wfd().Terminate();
  fsm().Terminate();
}

int NetSocket::IsIdle() { return fsm().IsIdle(); }

inline void NetSocket::ExtractNetMsg(NetMsg *msg) {
  memcpy(msg, &in_buf[0], sizeof(NetMsg));
}

int NetSocket::Start(int domain, int type, int protocol) {
  type |= SOCK_CLOEXEC;
  int s = socket(domain, type, protocol);
  if (unlikely(s < 0)) { return -errno; }

  InitFromFd(s);

  fsm().Start();
  return 0;
}

void NetSocket::StartFd(int fd) {
  InitFromFd(fd);
  fsm().Start();
  fsm().Action(kNetSockActionStarted);
}

void NetSocket::InitFromFd(int s) {
  CHECK(state() == kNetSockStateIdle || state() == kNetSockStateBeingAccepted);

  CHECK_EQ(s_, -1);
  s_ = s;

  int rc = fcntl(s_, F_SETFD, FD_CLOEXEC);
  CHECK_EQ(rc, 0);

  //  Switch the socket to the non-blocking mode. All underlying sockets
  //  are always used in the callbackhronous mode.
  int opt = fcntl(s_, F_GETFL, 0);
  if (opt == -1) opt = 0;
  if (!(opt & O_NONBLOCK)) { rc = fcntl(s_, F_SETFL, opt | O_NONBLOCK); }
}

void NetSocket::Stop() { fsm().Stop(); }

void NetSocket::AsyncStop() {
  worker_->Execute(&task_stop_);
  fsm_.Raise(&event_error_, kNetSockEventTypeShutdown,
             static_cast<void *>(this));
}

int NetSocket::SetSockOpt(int level, int optname, const void *optval,
                          size_t optlen) {
  //  The socket can be modified only before it's active.
  CHECK(state_ == kNetSockStateStarting || state_ == kNetSockStateAccepted);

  int rc = setsockopt(s_, level, optname, optval, (socklen_t)optlen);
  if (unlikely(rc != 0)) return -errno;

  return 0;
}

int NetSocket::Bind() {
  //  The socket can be bind only before it's connected.
  CHECK_EQ(state_, kNetSockStateStarting);

  //  Allow re-using the address.
  int opt = 1;
  int rc = setsockopt(s_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  CHECK_EQ(rc, 0);
  rc = bind(s_, (struct sockaddr *)&ip_addr_, (socklen_t)sizeof(ip_addr_));
  if (unlikely(rc != 0)) return -errno;

  return 0;
}

int NetSocket::Listen(int backlog) {
  //  You can start listening only before the socket is connected.
  CHECK_EQ(state_, kNetSockStateStarting);

  //  Start listening for incoming connections.
  int rc = listen(s_, backlog);
  if (unlikely(rc != 0)) return -errno;

  //  Notify the state machine.
  fsm().Action(kNetSockActionListen);

  return 0;
}

void NetSocket::Accept(NetSocket *listener) {
  if (fsm().IsIdle()) {
    //  socket state change to BeingAccepted, will NOT CHANGE until success.
    fsm().Start();
    fsm().Action(kNetSockActionBeingAccepted);
  }
  listener->fsm().Action(kNetSockActionAccept);

  CHECK(!asock() || asock() == listener);
  asock() = listener;
  CHECK(!listener->asock() || listener->asock() == this);
  listener->asock() = this;

  //  Ask the worker thread to wait for the new connection.
  listener->worker()->Execute(&listener->task_accept());
}

void NetSocket::Activate() { fsm().Action(kNetSockActionActivate); }

void NetSocket::Connect() {
  fsm().Action(kNetSockActionConnect);
  int rc = connect(s_, (struct sockaddr *)&ip_addr_, sizeof(ip_addr_));

  if (likely(rc == 0)) {
    fsm().Action(kNetSockActionDone);
    return;
  }

  if (unlikely(errno != EINPROGRESS)) {
    errnum() = errno;
    fsm().Action(kNetSockActionError);
    return;
  }

  //  Start asynchronous connect.
  worker()->Execute(&task_connecting());
}

int NetSocket::InternalTasks(int src, int type) {
  // internal tasks sent from user thread to worker thread
  switch (src) {
    case kNetSockSrcTaskSend:
      CHECK(type == kNetWorkerTaskExecute);
      worker()->SetOut(&wfd());
      return 1;
    case kNetSockSrcTaskRecv:
      CHECK(type == kNetWorkerTaskExecute);
      worker()->SetIn(&wfd());
      return 1;
    case kNetSockSrcTaskConnected:
      CHECK(type == kNetWorkerTaskExecute);
      worker()->AddFd(s_, &wfd());
      return 1;
    case kNetSockSrcTaskConnecting:
      CHECK(type == kNetWorkerTaskExecute);
      worker()->AddFd(s_, &wfd());
      worker()->SetOut(&wfd());
      return 1;
    case kNetSockSrcTaskAccept:
      CHECK(type == kNetWorkerTaskExecute);
      worker()->AddFd(s_, &wfd());
      worker()->SetIn(&wfd());
      return 1;
  }

  return 0;
}

static void NetSocketShutdownHandler(NetFsm *self, int src, int type,
                                     void *srcptr) {
  NetSocket *sock = container_of(self, NetSocket, fsm_);

  if (sock->InternalTasks(src, type)) { return; }

  if (unlikely(src == kNetFsmAction && type == kNetFsmStop)) {
    //  Socket in ACCEPTING or CANCELLING state cannot be closed.
    //  Stop the socket being accepted first.
    CHECK(sock->state() != kNetSockStateAccepting
          && sock->state() != kNetSockStateCancelling);

    sock->errnum() = 0;

    //  Synchronous stop.
    if (sock->state() == kNetSockStateIdle) { goto finish3; }
    if (sock->state() == kNetSockStateDone) { goto finish2; }
    if (sock->state() == kNetSockStateStarting
        || sock->state() == kNetSockStateAccepted
        || sock->state() == kNetSockStateAcceptingError
        || sock->state() == kNetSockStateListening) {
      goto finish1;
    }

    //  When socket that's being accepted is asked to stop, we have to
    //  ask the listener socket to stop accepting first.
    if (sock->state() == kNetSockStateBeingAccepted) {
      sock->fsm().Action(kNetSockActionCancel);
      sock->state() = kNetSockStateStoppingAccept;
      return;
    }

    //  Asynchronous stop.
    if (sock->state() != kNetSockStateRemovingFd) { sock->AsyncStop(); }

    sock->state() = kNetSockStateStopping;
    return;
  }
  if (unlikely(sock->state() == kNetSockStateStoppingAccept)) {
    CHECK(src == kNetFsmAction && type == kNetSockActionDone);
    goto finish2;
  }
  if (unlikely(sock->state() == kNetSockStateStopping)) {
    if (src != kNetSockSrcTaskStop) return;
    CHECK(type == kNetWorkerTaskExecute);
    sock->worker()->RmFd(&sock->wfd());
  finish1:
    NetCloseFd(sock->s());
    sock->s() = -1;
  finish2:
    sock->state() = kNetSockStateIdle;
    sock->fsm().Stopped(kNetSockEventTypeStopped);
  finish3:
    return;
  }

  NetFsmBadState(sock->state(), src, type);
}

static void NetSocketHandler(NetFsm *self, int src, int type,
                             UNUSED void *srcptr) {
  int rc = 0;
  int s = -1;
  int sockerr = 0;

  NetSocket *sock = container_of(self, NetSocket, fsm_);

  if (sock->InternalTasks(src, type)) { return; }

  switch (sock->state()) {
    // IDLE state
    case kNetSockStateIdle:
      switch (src) {
        case kNetFsmAction:
          switch (type) {
            case kNetFsmStart: sock->state() = kNetSockStateStarting; return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        default: NetFsmBadSource(sock->state(), src, type);
      }

    // Starting state:
    // OS socket is created, but it's not yet passed to workers
    // We can only set socket options, local and remote address
    case kNetSockStateStarting:
      //  Events from the owner of the sock.
      switch (src) {
        case kNetFsmAction:
          switch (type) {
            case kNetSockActionListen:
              sock->state() = kNetSockStateListening;
              return;
            case kNetSockActionConnect:
              sock->state() = kNetSockStateConnecting;
              return;
            case kNetSockActionBeingAccepted:
              sock->state() = kNetSockStateBeingAccepted;
              return;
            case kNetSockActionStarted:
              sock->worker()->AddFd(sock->s(), &sock->wfd());
              sock->state() = kNetSockStateActive;
              return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        default: NetFsmBadSource(sock->state(), src, type);
      }

    // Being_accepted state:
    // accept() was called, waiting new connection from listen_sock
    case kNetSockStateBeingAccepted:
      switch (src) {
        case kNetFsmAction:
          switch (type) {
            case kNetSockActionDone:
              sock->state() = kNetSockStateAccepted;
              sock->fsm().Raise(&sock->event_established(),
                                kNetSockEventTypeAccepted, sock);
              return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        default: NetFsmBadSource(sock->state(), src, type);
      }

    // Accetped state:
    // connection was accepted, it can be tuned now. Next step it'll
    // move to active state
    case kNetSockStateAccepted:
      switch (src) {
        case kNetFsmAction:
          switch (type) {
            case kNetSockActionActivate:
              sock->worker()->AddFd(sock->s(), &sock->wfd());
              sock->state() = kNetSockStateActive;
              return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        default: NetFsmBadSource(sock->state(), src, type);
      }

    // connecting state:
    // Asynchronous connecting is going on
    case kNetSockStateConnecting:
      switch (src) {
        case kNetFsmAction:
          switch (type) {
            case kNetSockActionDone:
              sock->state() = kNetSockStateActive;
              sock->worker()->Execute(&sock->task_connected());
              sock->fsm().Raise(&sock->event_established(),
                                kNetSockEventTypeConnected, sock);
              return;
            case kNetSockActionError:
              NetCloseFd(sock->s());
              sock->s() = -1;
              sock->state() = kNetSockStateDone;
              sock->fsm().Raise(&sock->event_error(), kNetSockEventTypeError,
                                sock);
              return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        case kNetSockSrcFd:
          switch (type) {
            case kNetWorkerFdOut:
              sock->worker()->ResetOut(&sock->wfd());
              sock->state() = kNetSockStateActive;
              sockerr = sock->GetErr();
              if (sockerr == 0) {
                sock->fsm().Raise(&sock->event_established(),
                                  kNetSockEventTypeConnected, sock);
              } else {
                sock->errnum() = sockerr;
                sock->worker()->RmFd(&sock->wfd());
                rc = close(sock->s());
                CHECK_EQ(rc, 0);
                sock->s() = -1;
                sock->state() = kNetSockStateDone;
                sock->fsm().Raise(&sock->event_error(), kNetSockEventTypeError,
                                  sock);
              }
              return;
            case kNetWorkerFdErr:
              sock->worker()->RmFd(&sock->wfd());
              NetCloseFd(sock->s());
              sock->s() = -1;
              sock->state() = kNetSockStateDone;
              sock->fsm().Raise(&sock->event_error(), kNetSockEventTypeError,
                                sock);
              return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        default: NetFsmBadSource(sock->state(), src, type);
      }

    // Active state:
    // Socket is connected and can be used for sending and receiving data
    case kNetSockStateActive:
      switch (src) {
        case kNetSockSrcFd:
          switch (type) {
            case kNetWorkerFdIn:
              rc = sock->RecvRaw();
              LOG(INFO) << "Recving raw by socket: " << sock << std::endl;
              if (likely(rc == 0)) {
                NetMsg *msg = NetMsg::NewNetMsg();
                sock->ExtractNetMsg(msg);
                sock->fsm().Raise(&sock->event_received(),
                                  kNetSockEventTypeReceived, msg);
                return;
              }
              if (likely(rc == -EAGAIN)) { return; }
              CHECK_EQ(rc, -ECONNRESET);
              return;

            case kNetWorkerFdOut:
              if (!sock->is_out_buf_busy()) {
                std::unique_lock<std::mutex> lock(sock->msq_sync());
                if (sock->DequeueNetMsgToBuf()) {
                  sock->is_out_buf_busy() = true;
                } else {
                  sock->worker()->ResetOut(&sock->wfd());
                  return;
                }
                lock.unlock();
              }
              rc = sock->SendRaw();
              LOG(INFO) << "Sending raw by socket: " << sock << std::endl;
              if (likely(rc == 0)) {
                sock->is_out_buf_busy() = false;
                sock->fsm().Raise(&sock->event_sent(), kNetSockEventTypeSent,
                                  sock);
                return;
              }
              if (likely(rc == -EAGAIN)) { return; }
              CHECK_EQ(rc, -ECONNRESET);
              return;

            case kNetWorkerFdErr:
              sock->worker()->RmFd(&sock->wfd());
              NetCloseFd(sock->s());
              sock->s() = -1;
              sock->state() = kNetSockStateDone;
              LOG(INFO) << "Remote possibility... worker fd error\n";
              sock->fsm().Raise(&sock->event_error(), kNetSockEventTypeError,
                                sock);
              return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        case kNetFsmAction:
          switch (type) {
            case kNetSockActionError:
              sock->state() = kNetSockStateRemovingFd;
              sock->AsyncStop();
              return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        default: NetFsmBadSource(sock->state(), src, type);
      }

    // Removing_fd state:
    case kNetSockStateRemovingFd:
      switch (src) {
        case kNetSockSrcTaskStop:
          switch (type) {
            case kNetWorkerTaskExecute:
              sock->worker()->RmFd(&sock->wfd());
              NetCloseFd(sock->s());
              sock->s() = -1;
              sock->state() = kNetSockStateDone;
              sock->fsm().Raise(&sock->event_error(), kNetSockEventTypeError,
                                sock);
              return;
            default: NetFsmBadAction(sock->state(), src, type);
          }

        //  Events from the file descriptor are ignored while it is being
        //  removed.
        case kNetSockSrcFd: return;

        case kNetFsmAction:
          switch (type) {
            case kNetSockActionError: return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        default: NetFsmBadSource(sock->state(), src, type);
      }

    // Done state:
    // Socket is closed. We can only stop the socket.
    case kNetSockStateDone: return;

    // Listening state:
    // Socket is listening for incoming connections but user is not accepting
    // a new connection
    case kNetSockStateListening:
      switch (src) {
        case kNetFsmAction:
          switch (type) {
            case kNetSockActionAccept:
              sock->state() = kNetSockStateAccepting;
              return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        default: NetFsmBadSource(sock->state(), src, type);
      }

    //  Accepting state:
    //  listener is waiting asynchronously for a new inbound connection to be
    //  accepted.
    case kNetSockStateAccepting:
      switch (src) {
        case kNetFsmAction:
          switch (type) {
            case kNetSockActionDone:
              sock->state() = kNetSockStateListening;
              return;
            case kNetSockActionCancel:
              sock->state() = kNetSockStateCancelling;
              sock->worker()->Execute(&sock->task_stop());
              return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        case kNetSockSrcFd:
          switch (type) {
            case kNetWorkerFdIn:
              s = accept4(sock->s(), nullptr, nullptr, SOCK_CLOEXEC);

              if (unlikely(s < 0 && errno == ECONNABORTED)) return;

              //  Resource allocation errors.
              if (unlikely(s < 0
                           && (errno == ENFILE || errno == EMFILE
                               || errno == ENOBUFS || errno == ENOMEM))) {
                sock->errnum() = errno;
                sock->state() = kNetSockStateAcceptingError;

                //  Wait till the user starts accepting once again.
                sock->worker()->RmFd(&sock->wfd());
                sock->fsm().Raise(&sock->event_error(),
                                  kNetSockEventTypeAcceptError, sock);
                return;
              }

              // Any other error is unexpected.
              CHECK_GT(s, 0);

              //  Initialise the new sock object.
              sock->asock()->InitFromFd(s);
              sock->asock()->state() = kNetSockStateAccepted;

              //  Notify the user that connection was accepted.
              sock->fsm().RaiseTo(&sock->asock()->fsm(),
                                  &sock->asock()->event_established(),
                                  kNetDeviceSrcSocketEvent,
                                  kNetSockEventTypeAccepted, sock->asock());

              //  Disassociate the listener socket from the accepted socket
              sock->asock()->asock() = nullptr;
              sock->asock() = nullptr;

              //  Wait till the user starts accepting once again.
              //  TODO: should automatically get new socket and listening,
              //  or just ask the device to set commands;
              sock->worker()->RmFd(&sock->wfd());
              sock->state() = kNetSockStateListening;
              return;

            default: NetFsmBadAction(sock->state(), src, type);
          }
        default: NetFsmBadSource(sock->state(), src, type);
      }

    // Accepting error state
    // waiting the socket to accept error and restart
    case kNetSockStateAcceptingError:
      switch (src) {
        case kNetFsmAction:
          switch (type) {
            case kNetSockActionAccept:
              sock->state() = kNetSockStateAccepting;
              return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        default: NetFsmBadSource(sock->state(), src, type);
      }

    // Cancelling state
    case kNetSockStateCancelling:
      switch (src) {
        case kNetSockSrcTaskStop:
          switch (type) {
            case kNetWorkerTaskExecute:
              sock->worker()->RmFd(&sock->wfd());
              sock->state() = kNetSockStateListening;

              //  Notify the accepted socket that it was stopped.
              sock->asock()->fsm().Action(kNetSockActionDone);
              return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        case kNetSockSrcFd:
          switch (type) {
            case kNetWorkerFdIn: return;
            default: NetFsmBadAction(sock->state(), src, type);
          }
        default: NetFsmBadSource(sock->state(), src, type);
      }

    // Invalid state
    default: NetFsmBadState(sock->state(), src, type);
  }
}

void NetSocket::AsyncSendMsg(const NetMsg &msg) {
  NetMsgWrapper *msg_wrapper = NetMsgWrapper::New(msg);
  msg_wrapper->Init();

  std::unique_lock<std::mutex> lock(msq_sync_);
  msg_buffer_queue_.Push(&msg_wrapper->item);
  lock.unlock();

  worker_->Execute(&task_send_);
  return;
}

int NetSocket::SendRaw() {
  LOG(INFO) << "Calling raw msg...\n";
  const NetMsg &msg = *(NetMsg *)(&out_buf[0]);
  const int64_t total_msg_sz =
      sizeof(NetMsg) + ((msg.msg_type == 1) ? msg.extra.data_len : 0);

  // send msg header
  if (out_offset < sizeof(NetMsg)) {
    ssize_t nbytes =
        write(s_, out_buf + out_offset, sizeof(NetMsg) - out_offset);
    if (unlikely(nbytes < 0)) {
      if (likely(errno == EAGAIN || errno == EWOULDBLOCK)) {
        nbytes = 0;
      } else {
        return -ECONNRESET;
      }
    }
    out_offset += nbytes;
  }

  // send appending msg data with address stored in msg
  if (out_offset >= sizeof(NetMsg) && out_offset < total_msg_sz) {
    ssize_t nbytes = write(
        s_, static_cast<char *>(msg.src.address) + out_offset - sizeof(NetMsg),
        total_msg_sz - out_offset);
    if (unlikely(nbytes < 0)) {
      if (likely(errno == EAGAIN || errno == EWOULDBLOCK)) {
        nbytes = 0;
      } else {
        return -ECONNRESET;
      }
    }
    out_offset += nbytes;
  }

  if (out_offset < total_msg_sz) { return -EAGAIN; }

  return 0;
}

int NetSocket::RecvRaw() {
  LOG(INFO) << "Calling RecvRaw ...\n";

  // try to receive the header
  if (in_offset < sizeof(NetMsg)) {
    ssize_t nbytes = read(s_, in_buf + in_offset, sizeof(NetMsg) - in_offset);
    if (unlikely(nbytes < 0)) {
      if (likely(errno == EAGAIN || errno == EWOULDBLOCK)) {
        nbytes = 0;
      } else {
        return -ECONNRESET;
      }
    }
    in_offset += nbytes;
  }

  if (in_offset < sizeof(NetMsg)) { return -EAGAIN; }

  // The header is received, check if there is appending data
  const NetMsg &msg = *(NetMsg *)(&in_buf[0]);
  const int64_t total_msg_sz =
      sizeof(NetMsg) + ((msg.msg_type == 1) ? msg.extra.data_len : 0);

  if (in_offset < total_msg_sz) {
    char *dst_address =
        static_cast<char *>(NetDevice::Singleton()->Token2Dptr(msg.dst.token));
    ssize_t nbytes = read(s_, dst_address + in_offset - sizeof(NetMsg),
                          msg.extra.data_len + (sizeof(NetMsg) - in_offset));
    if (unlikely(nbytes < 0)) {
      if (likely(errno == EAGAIN || errno == EWOULDBLOCK)) {
        nbytes = 0;
      } else {
        return -ECONNRESET;
      }
    }
    in_offset += nbytes;
  }

  if (in_offset < total_msg_sz) { return -EAGAIN; }

  // reset input offset
  in_offset = 0;
  return 0;
}

int NetSocket::GetErr() {
  int rc;
  int opt;
  socklen_t optsz;

  opt = 0;
  optsz = sizeof(opt);
  rc = getsockopt(s(), SOL_SOCKET, SO_ERROR, &opt, &optsz);

  if (rc == -1) return errno;
  CHECK_EQ(rc, 0);
  CHECK(optsz == sizeof(opt));
  return opt;
}

bool NetSocket::DequeueNetMsgToBuf() {
  if (msg_buffer_queue_.Empty()) { return false; }

  NetQueueItem *item = msg_buffer_queue_.Pop();

  NetMsgWrapper *wrapper = container_of(item, NetMsgWrapper, item);
  memcpy(out_buf, &wrapper->msg, sizeof(wrapper->msg));
  out_offset = 0;
  NetMsgWrapper::Free(wrapper);

  LOG(INFO) << "Dequeued a msg to out_buf...\n";
  return true;
}

}  // namespace klibmsg
