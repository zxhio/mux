//===- netutil.h - Network utility ------------------------------*- C++ -*-===//
//
/// \file
/// Network utility.
//
// Author:  zxh
// Date:    2024/04/10 18:32:03
//===----------------------------------------------------------------------===//

#pragma once

#include <asio/ip/tcp.hpp>
#include <system_error>

std::string to_string(const asio::ip::tcp::endpoint &endpoint);

std::pair<std::string, std::string> split_host_port(const std::string &hostport,
                                                    std::error_code &ec);
