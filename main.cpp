#include "addr.h"
#include "logrus.h"

#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>

#include <ev.h>

static struct option opts[] = {
    {"listen", required_argument, NULL, 'l'},
    {"dst", required_argument, NULL, 'd'},
    {"src", optional_argument, NULL, 's'},
    {"verbose", no_argument, NULL, 'V'},
    {"help", no_argument, NULL, 'h'},
    {0, 0, 0, 0},
};

static void usage(char *argv1) {
  fprintf(stderr, "Usage: %s\n", argv1);
  fprintf(stderr, "  -l,  --listen     Listen address or port\n");
  fprintf(stderr, "  -d,  --dst        Destination address\n");
  fprintf(stderr, "  -s,  --src        Source address or ip\n");
  fprintf(stderr, "  -V,  --verbose    Verbose output\n");
  fprintf(stderr, "  -h,  --help       Help\n");
}

static IPAddr parse_addr_args(const char *s, int *err_num) {
  std::string addr_text(s);
  std::string::size_type n = addr_text.find_last_of(':');
  if (n == std::string::npos) {
    // Only ip
    if (!std::all_of(addr_text.begin(), addr_text.end(),
                     [](char c) { return std::isdigit(c); }))
      return parse_iptext_port(addr_text, 0, err_num);

    // Only port
    int port = std::stoi(addr_text);
    if (port >= 0 && port <= 65536) {
      return parse_iptext_port("0.0.0.0", uint16_t(port), err_num);
    } else {
      SET_ERRNO(err_num, EINVAL);
      return IPAddr();
    }
  }

  std::string ip_part = addr_text.substr(0, n);
  std::string port_part = addr_text.substr(n + 1);
  int port = std::stoi(port_part);
  if (port < 0) {
    SET_ERRNO(err_num, errno);
    return IPAddr();
  }
  return parse_iptext_port(ip_part, uint16_t(port), err_num);
}

#define INVALID_ARG_CHECK_RET(arg_k, arg_v, err_num)                           \
  if (err_num) {                                                               \
    fprintf(stderr, "Invalid %s '%s'\n", arg_k, arg_v);                        \
    return 1;                                                                  \
  }

static int set_nonblock(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return -1;
  if (flags & O_NONBLOCK)
    return 0;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_listener(const IPAddr &addr) {
  int sockfd = ::socket(addr.addr()->sa_family,
                        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (sockfd < 0)
    return -1;

  const int x = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x)) < 0) {
    ::close(sockfd);
    return -1;
  }

  if (::bind(sockfd, addr.addr(), addr.len()) < 0) {
    ::close(sockfd);
    return -1;
  }

  if (::listen(sockfd, 1024) < 0) {
    ::close(sockfd);
    return -1;
  }

  return sockfd;
}

static int create_connection(const IPAddr &saddr, const IPAddr &daddr) {
  int sockfd = ::socket(daddr.addr()->sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sockfd < 0)
    return -1;

  if (saddr.addr()->sa_family != AF_UNSPEC) {
    if (::bind(sockfd, saddr.addr(), saddr.len()) < 0) {
      ::close(sockfd);
      return -1;
    }
  }

  if (::connect(sockfd, daddr.addr(), daddr.len()) < 0) {
    ::close(sockfd);
    return -1;
  }

  return sockfd;
}

struct TCPConn {
  IPAddr laddr;
  IPAddr raddr;
  struct ev_io io;
  uint64_t read_count;
  uint64_t write_count;
  bool read_done;
  bool write_done;
};

struct TCPConnPair : public std::enable_shared_from_this<TCPConnPair> {
  TCPConn from;
  TCPConn to;

  TCPConnPair(const TCPConn &from_conn, const TCPConn &to_conn)
      : from(from_conn), to(to_conn) {
    LOG_TRACE("TCPConnPair::TCPConnPair",
              KV("from_remote", from_conn.raddr.format()),
              KV("from_local", from_conn.laddr.format()),
              KV("to_local", to_conn.laddr.format()),
              KV("to_remote", to_conn.raddr.format()));
  }

  ~TCPConnPair() {
    LOG_TRACE("TCPConnPair::~TCPConnPair", KV("from", from.raddr.format()),
              KV("to", to.raddr.format()), KV("read", from.read_count),
              KV("write", from.write_count));
  }
};

std::map<IPAddrPair, std::shared_ptr<TCPConnPair>, IPAddrPairCompare>
    connections;

struct LoopContext {
  IPAddr laddr;
  IPAddr raddr;
};

void close_conn(TCPConn *from, TCPConn *to) {
  if (from->read_done && from->write_done)
    ::close(from->io.fd);

  if (to->read_done && to->write_done)
    ::close(to->io.fd);

  if (from->read_done && from->write_done && to->read_done && to->write_done) {
    LOG_DEBUG("Close conn", KV("from", from->raddr.format()),
              KV("to", to->raddr.format()), KV("from_fd", from->io.fd),
              KV("to_fd", to->io.fd));

    connections.erase({to->laddr, to->raddr});
    connections.erase({from->laddr, from->raddr});
  }
}

static void read_cb(struct ev_loop *loop, ev_io *w, int revents) {
  TCPConn *to = static_cast<TCPConn *>(w->data);
  TCPConn *from = static_cast<TCPConn *>(to->io.data);
  assert(w == &from->io);
  assert(w->fd == from->io.fd);

  char buf[1024 * 32];
  ssize_t nread = ::read(w->fd, buf, sizeof(buf) - 1);
  switch (nread) {
  case -1:
    LOG_ERROR("Fail to read", KV("raddr", from->raddr.format()),
              KV("laddr", from->laddr.format()), KV("fd", from->io.fd),
              KERR(errno));

    from->read_done = true;
    from->write_done = true;
    to->read_done = true;
    to->write_done = true;

    ::close(from->io.fd);
    ::close(to->io.fd);
    ev_io_stop(loop, &from->io);
    ev_io_stop(loop, &to->io);
    close_conn(from, to);
    return;
  case 0:
    LOG_DEBUG("Close half conn", KV("read", from->raddr.format()),
              KV("write", to->raddr.format()));

    from->read_done = true;
    to->write_done = true;

    ::shutdown(to->io.fd, SHUT_WR);
    ::shutdown(from->io.fd, SHUT_RD);
    ev_io_stop(loop, &from->io);
    close_conn(from, to);
    return;
  default:
    ssize_t nwrite = ::write(to->io.fd, buf, nread);
    if (nwrite < 0) {
      LOG_ERROR("Fail to write", KV("raddr", to->raddr.format()),
                KV("laddr", to->laddr.format()));
      // TODO: handle error
      return;
    }
    from->read_count += nread;
    to->write_count += nwrite;
    return;
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

  int err_num = 0;
  IPAddr client_laddr = get_local_addr(client_fd, &err_num);
  if (err_num) {
    LOG_ERROR("Fail to get local addr", KERR(err_num),
              KV("from", client_raddr.format()));
    ::close(client_fd);
    return;
  }

  if (set_nonblock(client_fd) < 0) {
    LOG_ERROR("Fail to set non block", KERR(err_num),
              KV("from", client_raddr.format()));
    ::close(client_fd);
    return;
  }

  LoopContext *loop_ctx = static_cast<LoopContext *>(ev_userdata(loop));

  int server_fd = create_connection(loop_ctx->laddr, loop_ctx->raddr);
  if (server_fd < 0) {
    LOG_ERROR("Fail to connect", KERR(errno),
              KV("src", loop_ctx->laddr.format()),
              KV("dst", loop_ctx->raddr.format()));
    ::close(client_fd);
    return;
  }

  IPAddr server_laddr = get_local_addr(server_fd, &err_num);
  if (err_num) {
    LOG_ERROR("Fail to get local addr", KERR(err_num),
              KV("dst", loop_ctx->raddr.format()));
    ::close(client_fd);
    ::close(server_fd);
    return;
  }

  LOG_DEBUG("Connected server", KV("from", client_raddr.format()),
            KV("laddr", server_laddr.format()),
            KV("raddr", loop_ctx->raddr.format()), KV("client_fd", client_fd),
            KV("server_fd", server_fd));

  if (set_nonblock(server_fd) < 0) {
    LOG_ERROR("Fail to set non block", KERR(err_num),
              KV("dst", loop_ctx->raddr.format()));
    ::close(client_fd);
    ::close(server_fd);
    return;
  }

  TCPConn client_conn{.laddr = client_laddr,
                      .raddr = client_raddr,
                      .read_count = 0,
                      .write_count = 0};
  TCPConn server_conn{.laddr = server_laddr,
                      .raddr = loop_ctx->raddr,
                      .read_count = 0,
                      .write_count = 0};

  std::shared_ptr<TCPConnPair> cp =
      std::make_shared<TCPConnPair>(client_conn, server_conn);
  connections[{client_laddr, client_raddr}] = cp;

  cp->from.io.data = &cp->to;
  cp->to.io.data = &cp->from;

  ev_io_init(&cp->from.io, read_cb, client_fd, EV_READ);
  ev_io_init(&cp->to.io, read_cb, server_fd, EV_READ);
  ev_io_start(loop, &cp->from.io);
  ev_io_start(loop, &cp->to.io);
}

int main(int argc, char *argv[]) {
  int err_num;
  bool verbose = false;
  IPAddr listen_addr;
  IPAddr src_addr;
  IPAddr dst_addr;

  while (1) {
    int this_option_optind = optind ? optind : 1;
    int c = getopt_long(argc, argv, "l:d:s:Vh", opts, NULL);
    if (c < 0)
      break;

    char *arg = optarg ? optarg : argv[optind];
    switch (c) {
    case 'l':
      listen_addr = parse_addr_args(arg, &err_num);
      INVALID_ARG_CHECK_RET("listen address", arg, err_num);
      break;
    case 'd':
      dst_addr = parse_addr_args(arg, &err_num);
      INVALID_ARG_CHECK_RET("dst address", arg, err_num);
      break;
    case 's':
      src_addr = parse_addr_args(arg, &err_num);
      INVALID_ARG_CHECK_RET("src address", arg, err_num);
      break;
    case 'V':
      verbose = true;
      break;
    case 'h':
      usage(argv[0]);
      return 0;
    default:
      usage(argv[0]);
      return 1;
    }
  }

  if (verbose)
    logrus::set_level(logrus::kTrace);
  logrus::set_pattern("%^%l%$ %Y%m%d %H:%M:%S %t %v");

  LOG_INFO("=== mux start ===");
  LOG_INFO("Parsed args", KV("listen", listen_addr.format()),
           KV("dst", dst_addr.format()), KV("src", src_addr.format()));

  int sockfd = create_listener(listen_addr);
  if (sockfd < 0)
    LOG_FATAL("Fatal to listen", KERR(errno));
  LOG_INFO("Listen on", KV("addr", listen_addr.format()));

  struct ev_io io;
  ev_io_init(&io, accept_cb, sockfd, EV_READ);

  LoopContext ctx{.laddr = src_addr, .raddr = dst_addr};
  struct ev_loop *loop = ev_default_loop();
  ev_set_userdata(loop, &ctx);
  ev_io_start(loop, &io);
  ev_loop(loop, 0);

  return 0;
}