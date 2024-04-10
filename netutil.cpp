//===- netutil.cpp - Network utility ----------------------------*- C++ -*-===//
//
/// \file
/// Network utility.
//
// Author:  zxh
// Date:    2024/04/10 18:33:22
//===----------------------------------------------------------------------===//

#include "netutil.h"

int set_nonblocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return -1;
  if (flags & O_NONBLOCK)
    return 0;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int create_and_bind_listener(const IPAddr &addr, bool nonblock,
                             std::error_code &ec) noexcept {
  ec = std::error_code();

  int sockfd = ::socket(addr.addr()->sa_family,
                        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (sockfd < 0) {
    ec = std::error_code(errno, std::system_category());
    return -1;
  }

  if (nonblock) {
    int x = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x)) < 0) {
      ec = std::error_code(errno, std::system_category());
      ::close(sockfd);
      return -1;
    }
  }

  if (::bind(sockfd, addr.addr(), addr.len()) < 0) {
    ec = std::error_code(errno, std::system_category());
    ::close(sockfd);
    return -1;
  }

  if (::listen(sockfd, 1024) < 0) {
    ec = std::error_code(errno, std::system_category());
    ::close(sockfd);
    return -1;
  }

  return sockfd;
}

int create_and_bind_listener(const IPAddr &addr, bool nonblock) {
  std::error_code ec;
  int sockfd = create_and_bind_listener(addr, nonblock, ec);
  if (ec)
    throw std::system_error(ec);
  return sockfd;
}

int create_connection(const IPAddr &saddr, const IPAddr &daddr,
                      std::error_code &ec) noexcept {
  ec = std::error_code();
  int sockfd = ::socket(daddr.addr()->sa_family, SOCK_STREAM, 0);
  if (sockfd < 0) {
    ec = std::error_code(errno, std::system_category());
    return -1;
  }

  if (saddr.addr()->sa_family != AF_UNSPEC) {
    if (::bind(sockfd, saddr.addr(), saddr.len()) < 0) {
      ec = std::error_code(errno, std::system_category());
      ::close(sockfd);
      return -1;
    }
  }

  if (::connect(sockfd, daddr.addr(), daddr.len()) < 0) {
    ec = std::error_code(errno, std::system_category());
    ::close(sockfd);
    return -1;
  }
  return sockfd;
}

int create_connection(const IPAddr &saddr, const IPAddr &daddr) {
  std::error_code ec;
  int fd = create_connection(saddr, daddr, ec);
  if (ec)
    throw std::system_error(ec);
  return fd;
}
