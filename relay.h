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

#include <map>
#include <memory>
#include <vector>

#include <ev.h>

struct RelayIPAddrTuple {
  IPAddr listen; // listen address
  IPAddr dst;    // server address
  IPAddr src;    // source address for connect
};

struct EventLoopPool;

struct EventLoop {
  size_t id_;
  EventLoopPool *pool_;
  struct ev_loop *loop_;
  struct ev_io efd_io_;
  std::map<int, RelayIPAddrTuple> relay_addrs_; // relay addresses
  std::vector<char> buf_;                       // reuse buf for read

  EventLoop(size_t id, EventLoopPool *pool, int efd);
};

struct EventLoopPool {
  std::vector<std::unique_ptr<EventLoop>> loops;
  std::map<int, struct ev_io> watchers; // listener watchers
  size_t curr_loop_idx;
};

void attach_listener(EventLoopPool &p, int listenfd,
                     const RelayIPAddrTuple &addr_tuple);

EventLoopPool create_event_loop_pool(size_t n);

void run_event_loop_pool(EventLoopPool &p);