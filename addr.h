//===- addr.h - Addr String--------------------------------------*- C++ -*-===//
//
/// \file
/// Stringify all address struct.
//
// Author:  zxh
// Date:    2023/09/19 17:04:27
//===----------------------------------------------------------------------===//

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <string>
#include <system_error>

inline std::string format_mac(uint8_t addr[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%2x:%2x:%2x:%2x:%2x:%2x", addr[0], addr[1],
           addr[2], addr[3], addr[4], addr[5]);
  return buf;
}

inline std::string format_ip_v4(in_addr_t addr) {
  char ip[INET_ADDRSTRLEN];
  ::inet_ntop(AF_INET, &addr, ip, sizeof(ip));
  return ip;
}

inline std::string format_addr_v4(const struct sockaddr_in *addr) {
  char buf[INET_ADDRSTRLEN + 6]; // "ip" + ':' + "65535"
  snprintf(buf, sizeof(buf), "%s:%d",
           format_ip_v4(addr->sin_addr.s_addr).data(), ntohs(addr->sin_port));
  return buf;
}

inline std::string format_ip_v6(const struct in6_addr *addr) {
  char ip[INET6_ADDRSTRLEN];
  ::inet_ntop(AF_INET6, &addr, ip, sizeof(ip));
  return ip;
}

inline std::string format_addr_v6(const struct sockaddr_in6 *addr) {
  char buf[INET6_ADDRSTRLEN + 8]; // '[' + "ip" + "]:" + "65535"
  snprintf(buf, sizeof(buf), "[%s]:%d", format_ip_v6(&addr->sin6_addr).data(),
           ntohs(addr->sin6_port));
  return buf;
}

template <typename AddrType>
struct Addr {
  struct sockaddr *addr() const {
    return static_cast<const AddrType *>(this)->sock_addr_impl();
  }

  socklen_t len() const {
    return static_cast<const AddrType *>(this)->sock_len_impl();
  }

  std::string format() const {
    return static_cast<const AddrType *>(this)->format_impl();
  }
};

class UnixAddr : public Addr<UnixAddr> {
public:
  friend Addr<UnixAddr>;

  UnixAddr() { un.sun_family = AF_UNSPEC; }
  UnixAddr(const std::string &path) {
    un.sun_family = AF_UNIX;
    memset(un.sun_path, 0, sizeof(un.sun_path));
    memcpy(un.sun_path, path.data(), path.length());
  }

private:
  struct sockaddr *sock_addr_impl() const { return (struct sockaddr *)&un; }
  socklen_t sock_len_impl() const { return sizeof(un); }
  std::string format_impl() const { return un.sun_path; }

  struct sockaddr_un un;
};

class IPAddr : public Addr<IPAddr> {
public:
  friend Addr<IPAddr>;

  IPAddr() { sa.sa_family = AF_UNSPEC; }
  IPAddr(const struct sockaddr_in *addr) : in(*addr) {}
  IPAddr(const struct sockaddr_in6 *addr) : in6(*addr) {}
  IPAddr(const struct sockaddr *addr, socklen_t len) {
    memcpy(&sa, addr, std::min(size_t(len), sizeof(in6)));
  }

  struct in_addr ipv4() const { return in.sin_addr; }

  struct in6_addr ipv6() const { return in6.sin6_addr; }

  uint16_t port() const {
    if (sa.sa_family == AF_INET)
      return ntohs(in.sin_port);
    else if (sa.sa_family == AF_INET6)
      return ntohs(in6.sin6_port);
    return 0;
  }

private:
  struct sockaddr *sock_addr_impl() const {
    return sa.sa_family == AF_INET6 ? (struct sockaddr *)&in6
                                    : (struct sockaddr *)&in;
  }

  socklen_t sock_len_impl() const {
    return sa.sa_family == AF_INET6 ? sizeof(in6) : sizeof(in);
  }

  std::string format_impl() const {
    if (sa.sa_family == AF_INET)
      return format_addr_v4(&in);
    else if (sa.sa_family == AF_INET6)
      return format_addr_v6(&in6);
    return "";
  }

  union {
    struct sockaddr sa;
    struct sockaddr_in in;
    struct sockaddr_in6 in6;
  };
};

struct IPAddrCompare {
  bool operator()(const IPAddr &l, const IPAddr &r) const {
    if (l.len() < r.len())
      return true;
    return memcmp(l.addr(), r.addr(), l.len()) < 0;
  }
};

struct IPAddrPair {
  IPAddr local;
  IPAddr remote;
};

struct IPAddrPairCompare {
  bool operator()(const IPAddrPair &l, const IPAddrPair &r) const {
    if (IPAddrCompare{}(l.remote, r.remote))
      return true;
    return IPAddrCompare{}(l.local, r.local);
  }
};

inline IPAddr parse_iptext_port(const std::string &iptext, uint16_t port,
                                std::error_code &ec) noexcept {
  IPAddr addr;
  for (const char ch : iptext) {
    if (ch == '.') {
      sockaddr_in in;
      in.sin_family = AF_INET;
      in.sin_port = htons(port);
      if (::inet_pton(AF_INET, iptext.data(), &in.sin_addr) < 0)
        ec = std::error_code(errno, std::system_category());
      else
        addr = IPAddr(&in);
      return addr;
    } else if (ch == ':') {
      sockaddr_in6 in6;
      in6.sin6_family = AF_INET6;
      in6.sin6_port = htons(port);
      if (::inet_pton(AF_INET6, iptext.data(), &in6.sin6_addr) < 0)
        ec = std::error_code(errno, std::system_category());
      else
        addr = IPAddr(&in6);
      return addr;
    }
  }

  ec = std::error_code(EINVAL, std::system_category());
  return addr;
}

inline IPAddr parse_iptext_port(const std::string &iptext, uint16_t port) {
  std::error_code ec;
  IPAddr addr = parse_iptext_port(iptext, port, ec);
  if (ec)
    throw std::system_error(ec);
  return addr;
}

inline IPAddr get_local_addr(int fd, std::error_code &ec) noexcept {
  IPAddr addr;
  struct sockaddr_storage ss;
  socklen_t len = sizeof(ss);
  if (getsockname(fd, (struct sockaddr *)&ss, &len) < -1) {
    ec = std::error_code(errno, std::system_category());
    return addr;
  }

  switch (ss.ss_family) {
  case AF_INET:
    addr = IPAddr((struct sockaddr_in *)&ss);
    break;
  case AF_INET6:
    addr = IPAddr((struct sockaddr_in6 *)&ss);
    break;
  default:
    ec = std::error_code(EINVAL, std::system_category());
    break;
  }
  return addr;
}

inline IPAddr get_local_addr(int fd) {
  std::error_code ec;
  IPAddr addr = get_local_addr(fd, ec);
  if (ec)
    throw std::system_error(ec);
  return addr;
}

inline IPAddr get_remote_addr(int fd, std::error_code &ec) noexcept {
  IPAddr addr;
  struct sockaddr_storage ss;
  socklen_t len = sizeof(ss);
  if (getpeername(fd, (struct sockaddr *)&ss, &len) < -1) {
    ec = std::error_code(errno, std::system_category());
    return addr;
  }

  switch (ss.ss_family) {
  case AF_INET:
    addr = IPAddr((struct sockaddr_in *)&ss);
    break;
  case AF_INET6:
    addr = IPAddr((struct sockaddr_in6 *)&ss);
    break;
  default:
    ec = std::error_code(EINVAL, std::system_category());
    break;
  }
  return addr;
}

inline IPAddr get_remote_addr(int fd) {
  std::error_code ec;
  IPAddr addr = get_remote_addr(fd, ec);
  if (ec)
    throw std::system_error(ec);
  return addr;
}