//===- relay.h - Relay interface --------------------------------*- C++ -*-===//
//
/// \file
/// Define relay command interface.
//
// Author:  zxh
// Date:    2024/04/09 22:16:32
//===----------------------------------------------------------------------===//

#pragma once

#include <asio/ip/tcp.hpp>

struct RelayEndpoints {
  asio::ip::tcp::endpoint listen;
  asio::ip::tcp::endpoint src;
  asio::ip::tcp::endpoint dst;
};

struct RelayConn {
  asio::ip::tcp::socket conn_;
  asio::ip::tcp::endpoint laddr_;
  asio::ip::tcp::endpoint raddr_;
  uint64_t read_count_;
  uint64_t write_count_;

  RelayConn(asio::ip::tcp::socket conn, const asio::ip::tcp::endpoint &laddr,
            const asio::ip::tcp::endpoint &raddr)
      : conn_(std::move(conn)), laddr_(laddr), raddr_(raddr), read_count_(0),
        write_count_(0) {}
};

class Relay : public std::enable_shared_from_this<Relay> {
public:
  using SharedBuffer = std::shared_ptr<std::vector<char>>;

  Relay(asio::ip::tcp::socket client_conn, asio::ip::tcp::socket server_conn,
        const asio::ip::tcp::endpoint &client_laddr,
        const asio::ip::tcp::endpoint &client_raddr,
        const asio::ip::tcp::endpoint &server_laddr,
        const asio::ip::tcp::endpoint &server_raddr);

  ~Relay();

  void start() noexcept;

private:
  void async_write_all(std::shared_ptr<Relay> self, RelayConn &from,
                       RelayConn &to, SharedBuffer buf, size_t n) noexcept;

  void io_copy(RelayConn &from, RelayConn &to, SharedBuffer buf) noexcept;

  RelayConn client_;
  RelayConn server_;
};

class RelayServer {
public:
  RelayServer(asio::io_context &context, const RelayEndpoints &endpoints);

private:
  void new_conn(asio::ip::tcp::socket client_conn) noexcept;
  void do_accept();

  RelayEndpoints endpoints_;
  asio::ip::tcp::acceptor acceptor_;
};
