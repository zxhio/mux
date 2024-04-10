//===- conn.cpp - Conn ------------------------------------------*- C++ -*-===//
//
/// \file
///
//
// Author:  zxh
// Date:    2024/04/10 10:30:37
//===----------------------------------------------------------------------===//

#include "conn.h"
#include "netutil.h"

TCPConn connect_to(const IPAddr &daddr) {
  IPAddr saddr;
  return connect_to(saddr, daddr);
}

TCPConn connect_to(const IPAddr &saddr, const IPAddr &daddr) {
  int sockfd = create_connection(saddr, daddr);
  IPAddr laddr = get_local_addr(sockfd);
  return TCPConn(laddr, daddr, sockfd);
}

TCPConn connect_to(const IPAddr &daddr, std::error_code &ec) noexcept {
  IPAddr saddr;
  return connect_to(saddr, daddr, ec);
}

TCPConn connect_to(const IPAddr &saddr, const IPAddr &daddr,
                   std::error_code &ec) noexcept {
  int sockfd = create_connection(saddr, daddr, ec);
  if (ec)
    return TCPConn();

  IPAddr laddr = get_local_addr(sockfd, ec);
  if (ec)
    return TCPConn();

  return TCPConn(saddr, daddr, sockfd);
}
