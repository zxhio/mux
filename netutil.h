//===- netutil.h - Network utility ------------------------------*- C++ -*-===//
//
/// \file
/// Network utility.
//
// Author:  zxh
// Date:    2024/04/10 18:32:03
//===----------------------------------------------------------------------===//

#pragma once

#include "addr.h"

#include <fcntl.h>
#include <unistd.h>

#include <system_error>

// Set fd to nonblocking mode.
int set_nonblocking(int fd);

// Create a listener socket and bind it.
int create_and_bind_listener(const IPAddr &addr, bool nonblock,
                             std::error_code &ec) noexcept;
int create_and_bind_listener(const IPAddr &addr, bool nonblock);

// Create a connection socket.
int create_connection(const IPAddr &saddr, const IPAddr &daddr,
                      std::error_code &ec) noexcept;
int create_connection(const IPAddr &saddr, const IPAddr &daddr);
