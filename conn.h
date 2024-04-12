//===- conn.h - Conn --------------------------------------------*- C++ -*-===//
//
/// \file
/// Connection interface define.
//
// Author:  zxh
// Date:    2024/04/09 23:02:55
//===----------------------------------------------------------------------===//

#pragma once

#include "addr.h"

#include <any>
#include <memory>

#include <ev.h>

/// Connection common interface.
template <typename ConnT, typename AddrT>
struct Conn {
  using ConnType = ConnT;
  using AddrType = AddrT;

  ssize_t read(char *buf, size_t buf_len) {
    return static_cast<ConnType *>(this)->read_impl(buf, buf_len);
  }

  ssize_t write(const char *buf, size_t buf_len) {
    return static_cast<ConnType *>(this)->write_impl(buf, buf_len);
  }

  int close() { return static_cast<ConnType *>(this)->close_impl(); }

  AddrType local_addr() const {
    return static_cast<const ConnType *>(this)->local_addr_impl();
  }

  AddrType remote_addr() const {
    return static_cast<const ConnType *>(this)->remote_addr_impl();
  }
};

using TCPConnIOCallback = void(struct ev_loop *, ev_io *, int);

/// TCP connection.
class TCPConn : public Conn<TCPConn, IPAddr>,
                public std::enable_shared_from_this<TCPConn> {
public:
  TCPConn(const TCPConn &) = delete;
  TCPConn &operator=(const TCPConn &) = delete;

  TCPConn() { ev_io_init(&read_io_, nullptr, -1, EV_NONE); }

  TCPConn(const AddrType &laddr, const AddrType &raddr, int fd)
      : laddr_(laddr), raddr_(raddr) {
    ev_io_init(&read_io_, nullptr, fd, EV_NONE);
    ev_io_init(&write_io_, nullptr, fd, EV_NONE);
  }

  void shutdown(int how) { ::shutdown(read_io_.fd, how); }

  void set_read_event(struct ev_loop *loop, TCPConnIOCallback cb, int events) {
    set_event(loop, &read_io_, cb, events);
  }
  void set_write_event(struct ev_loop *loop, TCPConnIOCallback cb, int events) {
    set_event(loop, &write_io_, cb, events);
  }

  void enable_read() { ev_io_start(loop_, &read_io_); }
  void enable_write() { ev_io_start(loop_, &write_io_); }
  void disable_read() { ev_io_stop(loop_, &read_io_); }
  void disable_write() { ev_io_stop(loop_, &write_io_); }

  void set_context(const std::any &context) { context_ = context; }
  void set_context(std::any &&ctx) { context_ = std::move(ctx); }
  std::any get_context() const { return context_; }

private:
  friend Conn<TCPConn, AddrType>;

  ssize_t read_impl(char *buf, size_t buf_len) {
    return ::read(read_io_.fd, buf, buf_len);
  }

  ssize_t write_impl(const char *buf, size_t buf_len) {
    return ::write(read_io_.fd, buf, buf_len);
  }

  int close_impl() { return ::close(read_io_.fd); }

  AddrType local_addr_impl() const { return laddr_; }

  AddrType remote_addr_impl() const { return raddr_; }

  void set_event(struct ev_loop *loop, struct ev_io *io, TCPConnIOCallback cb,
                 int events) {
    loop_ = loop;
    io->data = this;
    ev_set_cb(io, cb);
    ev_io_modify(io, events);
  }

private:
  AddrType laddr_;
  AddrType raddr_;
  struct ev_loop *loop_;
  struct ev_io read_io_;
  struct ev_io write_io_;
  std::any context_;
};

TCPConn connect_to(const IPAddr &laddr, const IPAddr &raddr,
                   std::error_code &ec) noexcept;
TCPConn connect_to(const IPAddr &raddr, std::error_code &ec) noexcept;
TCPConn connect_to(const IPAddr &laddr, const IPAddr &raddr);
TCPConn connect_to(const IPAddr &raddr);
