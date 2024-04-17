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

struct Relay : public std::enable_shared_from_this<Relay> {
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
  void async_write_all(std::shared_ptr<Relay> self, asio::ip::tcp::socket &from,
                       asio::ip::tcp::socket &to, SharedBuffer buf,
                       size_t n) noexcept;

  void io_copy(asio::ip::tcp::socket &from, asio::ip::tcp::socket &to,
               SharedBuffer buf) noexcept;

  // TODO: Cache addr to avoid call getsockname/getpeername
  // asio::ip::tcp::endpoint client_laddr_;
  // asio::ip::tcp::endpoint client_raddr_;
  // asio::ip::tcp::endpoint server_laddr_;
  // asio::ip::tcp::endpoint server_raddr_;
  asio::ip::tcp::socket client_conn_;
  asio::ip::tcp::socket server_conn_;
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
