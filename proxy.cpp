//===- proxy.cpp - TCP Proxy ------------------------------------*- C++ -*-===//
//
/// \file
/// Provide connection proxy base handler.
//
// Author:  zxh
// Date:    2024/04/09 22:15:08
//===----------------------------------------------------------------------===//

#include "proxy.h"
#include "conn.h"
#include "logrus.h"
#include "netutil.h"

static const size_t kReusableBufferSize = 1024 * 64;

struct LoopContext {
  IPAddr laddr;
  IPAddr raddr;
  std::vector<char> buf; // reuse buf for read
};

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
  LoopContext *ctx = static_cast<LoopContext *>(ev_userdata(loop));
  TCPConnStat *from = static_cast<TCPConnStat *>(w->data);
  std::shared_ptr<TCPConnStat> to =
      std::any_cast<std::shared_ptr<TCPConnStat>>(from->get_context());

  ssize_t nread = from->read(ctx->buf.data(), ctx->buf.capacity());
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
    ssize_t n = to->write(ctx->buf.data(), nread);
    if (n < 0) {
      LOG_ERROR("Fail to write", KV("laddr", to->local_addr().format()),
                KV("raddr", to->remote_addr().format()));
      return;
    }
    from->read_count += nread;
    to->write_count += n;
    break;
  }
}

static void accept_cb(struct ev_loop *loop, ev_io *w, int revents) {
  struct sockaddr_storage addr;
  socklen_t len = sizeof(addr);

  int client_fd = ::accept(w->fd, (struct sockaddr *)&addr, &len);
  if (client_fd < 0)
    return;

  IPAddr client_raddr((struct sockaddr *)&addr, len);
  LOG_INFO("New conn", KV("from", client_raddr.format()));

  std::error_code ec;
  IPAddr client_laddr = get_local_addr(client_fd, ec);
  if (ec) {
    LOG_ERROR("Fail to get local addr", KERR(ec.value()),
              KV("from", client_raddr.format()));
    ::close(client_fd);
    return;
  }

  if (set_nonblocking(client_fd) < 0) {
    LOG_ERROR("Fail to set non block", KERR(ec.value()),
              KV("from", client_raddr.format()));
    ::close(client_fd);
    return;
  }

  LoopContext *loop_ctx = static_cast<LoopContext *>(ev_userdata(loop));

  int server_fd = create_connection(loop_ctx->laddr, loop_ctx->raddr, ec);
  if (ec) {
    LOG_ERROR("Fail to connect", KERR(errno),
              KV("src", loop_ctx->laddr.format()),
              KV("dst", loop_ctx->raddr.format()));
    ::close(client_fd);
    return;
  }

  IPAddr server_laddr = get_local_addr(server_fd, ec);
  if (ec) {
    LOG_ERROR("Fail to get local addr", KERR(ec.value()),
              KV("dst", loop_ctx->raddr.format()));
    ::close(client_fd);
    ::close(server_fd);
    return;
  }

  LOG_DEBUG("Connected server", KV("from", client_raddr.format()),
            KV("laddr", server_laddr.format()),
            KV("raddr", loop_ctx->raddr.format()), KV("client_fd", client_fd),
            KV("server_fd", server_fd));

  if (set_nonblocking(server_fd) < 0) {
    LOG_ERROR("Fail to set non block", KERR(ec.value()),
              KV("dst", loop_ctx->raddr.format()));
    ::close(client_fd);
    ::close(server_fd);
    return;
  }

  std::shared_ptr<TCPConnStat> client_conn =
      std::make_shared<TCPConnStat>(client_laddr, client_raddr, client_fd);
  std::shared_ptr<TCPConnStat> server_conn =
      std::make_shared<TCPConnStat>(server_laddr, loop_ctx->raddr, server_fd);

  client_conn->set_context(server_conn);
  server_conn->set_context(client_conn);
  client_conn->attach_loop(loop, read_cb, EV_READ);
  server_conn->attach_loop(loop, read_cb, EV_READ);
}

void run_event_loop(int sockfd, const IPAddr &src_addr,
                    const IPAddr &dst_addr) {
  LoopContext ctx{.laddr = src_addr,
                  .raddr = dst_addr,
                  .buf = std::vector<char>(kReusableBufferSize, 0)};

  struct ev_loop *loop = ev_default_loop();
  ev_set_userdata(loop, &ctx);

  struct ev_io io;
  ev_io_init(&io, accept_cb, sockfd, EV_READ);
  ev_io_start(loop, &io);

  ev_loop(loop, 0);
}