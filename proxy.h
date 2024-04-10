//===- proxy.h - Proxy interface --------------------------------*- C++ -*-===//
//
/// \file
/// Define proxy command interface.
//
// Author:  zxh
// Date:    2024/04/09 22:16:32
//===----------------------------------------------------------------------===//

#pragma once

#include "addr.h"

void run_event_loop(int sockfd, const IPAddr &src_addr, const IPAddr &dst_addr);