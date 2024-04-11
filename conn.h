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

  TCPConn() { ev_io_init(&io_, nullptr, -1, EV_NONE); }

  TCPConn(const AddrType &laddr, const AddrType &raddr, struct ev_io io)
      : laddr_(laddr), raddr_(raddr), io_(io) {}

  TCPConn(const AddrType &laddr, const AddrType &raddr, int fd)
      : laddr_(laddr), raddr_(raddr) {
    ev_io_init(&io_, nullptr, fd, EV_NONE);
  }

  void shutdown(int how) { ::shutdown(io_.fd, how); }

  void attach_loop(struct ev_loop *loop, TCPConnIOCallback cb, int revents) {
    io_.data = this;
    ev_set_cb(&io_, cb);
    ev_io_modify(&io_, revents);
    ev_io_start(loop, &io_);
    loop_ = loop;
  }

  void detach_loop() { ev_io_stop(loop_, &io_); }

  void set_context(const std::any &context) { context_ = context; }
  void set_context(std::any &&context) { context_ = std::move(context); }
  std::any get_context() const { return context_; }

  int fd() const { return io_.fd; }

private:
  friend Conn<TCPConn, AddrType>;

  ssize_t read_impl(char *buf, size_t buf_len) {
    return ::read(io_.fd, buf, buf_len);
  }

  ssize_t write_impl(const char *buf, size_t buf_len) {
    return ::write(io_.fd, buf, buf_len);
  }

  int close_impl() { return ::close(io_.fd); }

  AddrType local_addr_impl() const { return laddr_; }

  AddrType remote_addr_impl() const { return raddr_; }

private:
  AddrType laddr_;
  AddrType raddr_;
  struct ev_loop *loop_;
  struct ev_io io_;
  std::any context_;
};

TCPConn connect_to(const IPAddr &laddr, const IPAddr &raddr,
                   std::error_code &ec) noexcept;
TCPConn connect_to(const IPAddr &raddr, std::error_code &ec) noexcept;
TCPConn connect_to(const IPAddr &laddr, const IPAddr &raddr);
TCPConn connect_to(const IPAddr &raddr);
