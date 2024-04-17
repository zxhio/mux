//===- netutil.cpp - Network utility ----------------------------*- C++ -*-===//
//
/// \file
/// Network utility.
//
// Author:  zxh
// Date:    2024/04/10 18:33:22
//===----------------------------------------------------------------------===//

#include "netutil.h"
#include "errors.h"

#include <algorithm>

std::string to_string(const asio::ip::tcp::endpoint &endpoint) {
  std::stringstream ss;
  ss << endpoint;
  return ss.str();
}

std::pair<std::string, std::string> split_host_port(const std::string &hostport,
                                                    std::error_code &ec) {
  size_t i = hostport.rfind(':');
  if (i == std::string::npos) {
    ec = std::error_code(AddrErrMissingPort, address_category());
    return {};
  }
  size_t j = 0;
  size_t k = 0;

  std::string host;
  if (hostport[0] == '[') {
    // Expect the first ']' just before the last ':'.
    size_t end = hostport.rfind(']');
    if (end == std::string::npos) {
      ec = std::error_code(AddrErrMissingClosedBrackets, address_category());
      return {};
    }

    // There can't be a ':' behind the ']' now.
    if (end + 1 == hostport.length()) {
      ec = std::error_code(AddrErrMissingPort, address_category());
      return {};
    } else if (end + 1 != i) {
      // Either ']' isn't followed by a colon, or it is
      // followed by a colon that is not the last one.
      if (hostport[end + 1] == ':')
        ec = std::error_code(AddrErrTooManyColons, address_category());
      else
        ec = std::error_code(AddrErrMissingPort, address_category());
      return {};
    }
    host = hostport.substr(1, end);
    j = 1;
    k = end + 1;
  } else {
    host = hostport.substr(0, i);
    if (host.find(':') != std::string::npos) {
      ec = std::error_code(AddrErrTooManyColons, address_category());
      return {};
    }
  }

  if (hostport.find('[', j) != std::string::npos) {
    ec = std::error_code(AddrErrUnexpectedOpenBrackets, address_category());
    return {};
  }
  if (hostport.find(']', k) != std::string::npos) {
    ec = std::error_code(AddrPortErrUnexpectedClosedBrackets,
                         address_category());
    return {};
  }

  ec = std::error_code();
  return std::make_pair(host, hostport.substr(i + 1));
}
