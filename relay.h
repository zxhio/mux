//===- relay.h - Relay interface --------------------------------*- C++ -*-===//
//
/// \file
/// Define relay command interface.
//
// Author:  zxh
// Date:    2024/04/09 22:16:32
//===----------------------------------------------------------------------===//

#pragma once

#include "addr.h"

#include <ev.h>

struct RelayIPAddrTuple {
  IPAddr listen; // listen address
  IPAddr dst;    // server address
  IPAddr src;    // source address for connect
};

void attach_listener(struct ev_loop *loop, int sockfd,
                     const RelayIPAddrTuple &addr_tuple);