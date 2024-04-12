//===- relay.cpp - TCP Relay ------------------------------------*- C++ -*-===//
//
/// \file
/// TCP connection relay.
//
// Author:  zxh
// Date:    2024/04/09 22:15:08
//===----------------------------------------------------------------------===//

#include "relay.h"
#include "conn.h"
#include "logrus.h"
#include "netutil.h"

#include <assert.h>
#include <sys/eventfd.h>

#include <thread>

static const size_t kReusableBufferSize = 1024 * 64;

static void accept_cb(struct ev_loop *loop, ev_io *w, int revents);
static void read_cb(struct ev_loop *loop, ev_io *w, int revents);
static void eventfd_read_cb(struct ev_loop *loop, ev_io *w, int revents);

EventLoop::EventLoop(size_t id, EventLoopPool *p, int efd)
    : id_(id), pool_(p), loop_(ev_loop_new()), buf_(kReusableBufferSize, 0) {
  ev_set_userdata(loop_, this);
  ev_io_init(&efd_io_, eventfd_read_cb, efd, EV_READ);
  ev_io_start(loop_, &efd_io_);
}

struct TCPConnStat : public TCPConn {
  uint64_t read_count;
  uint64_t write_count;
  bool read_done;
  bool write_done;

  TCPConnStat(const IPAddr &laddr, const IPAddr &raddr, int fd)
      : TCPConn(laddr, raddr, fd), read_count(0), write_count(0),
        read_done(false), write_done(false) {
    LOG_TRACE("TCPConnStat::TCPConnStat", KV("laddr", laddr.format()),
              KV("raddr", raddr.format()));
  }

  ~TCPConnStat() {
    LOG_TRACE("TCPConnStat::~TCPConnStat", KV("laddr", local_addr().format()),
              KV("raddr", remote_addr().format()), KV("read", read_count),
              KV("write", write_count));
  }
};

static void read_cb(struct ev_loop *loop, ev_io *w, int revents) {
  EventLoop *el = static_cast<EventLoop *>(ev_userdata(loop));
  TCPConnStat *from = static_cast<TCPConnStat *>(w->data);
  std::shared_ptr<TCPConnStat> to =
      std::any_cast<std::shared_ptr<TCPConnStat>>(from->get_context());

  ssize_t nread = from->read(el->buf_.data(), el->buf_.capacity());
  switch (nread) {
  case -1:
    LOG_ERROR("Fail to read", KV("laddr", from->local_addr().format()),
              KV("raddr", from->remote_addr().format()), KV("fd", w->fd),
              KERR(errno));

    from->close();
    from->detach_loop();
    from->set_context(std::any());

    to->close();
    to->detach_loop();
    to->set_context(std::any());
    break;
  case 0:
    LOG_DEBUG("Close half conn",
              KV("read_remote", from->remote_addr().format()),
              KV("write_remote", to->remote_addr().format()),
              KV("read_fd", from->fd()), KV("write_fd", to->fd()));

    from->shutdown(SHUT_RD);
    from->detach_loop();
    from->read_done = true;
    if (from->read_done && from->write_done) {
      from->close();
      from->set_context(std::any());
    }

    to->shutdown(SHUT_WR);
    to->write_done = true;
    if (to->read_done && to->write_done) {
      to->close();
      to->set_context(std::any());
    }
    break;
  default:
    ssize_t n = to->write(el->buf_.data(), nread);
    if (n < 0) {
      LOG_ERROR("Fail to write", KERR(errno),
                KV("laddr", to->local_addr().format()),
                KV("raddr", to->remote_addr().format()));
      return;
    }
    from->read_count += nread;
    to->write_count += n;
    break;
  }
}

static void eventfd_read_cb(struct ev_loop *loop, ev_io *w, int revents) {
  eventfd_t value;
  if (::eventfd_read(w->fd, &value) < 0) {
    LOG_ERROR("Fail to eventfd_read", KERR(errno));
    return;
  }

  int listen_fd = static_cast<int>((value & 0xffffffff00000000) >> 32);
  int client_fd = static_cast<int>(value & 0x00000000ffffffff);

  EventLoop *el = static_cast<EventLoop *>(ev_userdata(loop));
  const auto &it = el->relay_addrs_.find(listen_fd);
  if (it == el->relay_addrs_.end()) {
    ::close(client_fd);
    LOG_WARN("Not found relay addr tuple", KV("listen_fd", listen_fd),
             KV("client_fd", client_fd));
    return;
  }
  const auto &addr_tuple = it->second;

  std::error_code ec;
  IPAddr client_raddr = get_remote_addr(client_fd, ec);
  if (ec) {
    LOG_ERROR("Fail to get client remote addr", KERR(ec.value()),
              KV("fd", client_fd));
    ::close(client_fd);
    return;
  }
  LOG_INFO("New conn", KV("from", client_raddr.format()));

  IPAddr client_laddr = get_local_addr(client_fd, ec);
  if (ec) {
    LOG_ERROR("Fail to get local addr", KERR(ec.value()), KV("fd", client_fd),
              KV("from", client_raddr.format()));
    ::close(client_fd);
    return;
  }

  int server_fd = create_connection(addr_tuple.src, addr_tuple.dst, ec);
  if (ec) {
    LOG_ERROR("Fail to connect", KERR(errno),
              KV("saddr", addr_tuple.src.format()),
              KV("daddr", addr_tuple.dst.format()));
    ::close(client_fd);
    return;
  }

  IPAddr server_laddr = get_local_addr(server_fd, ec);
  if (ec) {
    LOG_ERROR("Fail to get local addr", KERR(ec.value()),
              KV("daddr", addr_tuple.dst.format()));
    ::close(client_fd);
    ::close(server_fd);
    return;
  }

  LOG_DEBUG("Connected to server", KV("from", client_raddr.format()),
            KV("laddr", server_laddr.format()),
            KV("raddr", addr_tuple.dst.format()), KV("client_fd", client_fd),
            KV("server_fd", server_fd));

  if (set_nonblocking(server_fd) < 0) {
    LOG_ERROR("Fail to set non block", KERR(ec.value()),
              KV("dst", addr_tuple.dst.format()));
    ::close(client_fd);
    ::close(server_fd);
    return;
  }

  std::shared_ptr<TCPConnStat> client_conn =
      std::make_shared<TCPConnStat>(client_laddr, client_raddr, client_fd);
  std::shared_ptr<TCPConnStat> server_conn =
      std::make_shared<TCPConnStat>(server_laddr, addr_tuple.dst, server_fd);

  client_conn->set_context(server_conn);
  server_conn->set_context(client_conn);
  client_conn->attach_loop(loop, read_cb, EV_READ);
  server_conn->attach_loop(loop, read_cb, EV_READ);
}

static void accept_cb(struct ev_loop *loop, ev_io *w, int revents) {
  int client_fd = ::accept(w->fd, nullptr, nullptr);
  if (client_fd < 0) {
    LOG_ERROR("Fail to accept", KERR(errno), KV("fd", w->fd));
    return;
  }

  if (set_nonblocking(client_fd) < 0) {
    LOG_ERROR("Fail to set non block", KERR(errno), KV("client_fd", client_fd));
    ::close(client_fd);
    return;
  }

  EventLoopPool *p = static_cast<EventLoop *>(ev_userdata(loop))->pool_;
  p->curr_loop_idx++;
  const auto &el = p->loops[p->curr_loop_idx % p->loops.size()];
  LOG_TRACE("Notify event loop to handle", KV("id", el->id_));

  eventfd_t value = 0;
  value |= static_cast<eventfd_t>(w->fd) << 32 & 0xffffffff00000000;
  value |= static_cast<eventfd_t>(client_fd & 0x00000000ffffffff);
  ::eventfd_write(el->efd_io_.fd, value);
}

EventLoopPool create_event_loop_pool(size_t n) {
  size_t nloop = std::max(n, size_t(1));
  LOG_DEBUG("Create event loop pool", KV("size", nloop));
  EventLoopPool p{.curr_loop_idx = 0};
  for (size_t i = 0; i < nloop; i++) {
    int efd = ::eventfd(0, 0);
    if (efd < 0)
      throw std::system_error(std::error_code(errno, std::system_category()),
                              "eventfd");
    LOG_TRACE("Create event loop", KV("id", i));
    p.loops.emplace_back(std::make_unique<EventLoop>(i, &p, efd));
  }

  return p;
}

void attach_listener(EventLoopPool &p, int listenfd,
                     const RelayIPAddrTuple &addr_tuple) {
  assert(p.loops.size() > 0);

  for (auto &loop : p.loops)
    loop->relay_addrs_[listenfd] = addr_tuple;

  p.watchers[listenfd] = {};
  ev_io_init(&p.watchers[listenfd], accept_cb, listenfd, EV_READ);
  ev_io_start(p.loops[0]->loop_, &p.watchers[listenfd]);
}

void run_event_loop_pool(EventLoopPool &p) {
  assert(p.loops.size() > 0);

  std::vector<std::thread> threads;
  for (const auto &el : p.loops) {
    LOG_DEBUG("Run event loop", KV("id", el->id_));
    threads.emplace_back(std::thread([&]() { ev_loop(el->loop_, 0); }));
  }
  for (auto &t : threads)
    t.join();
}