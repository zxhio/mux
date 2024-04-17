//===- relay.cpp - TCP Relay ------------------------------------*- C++ -*-===//
//
/// \file
/// TCP connection relay.
//
// Author:  zxh
// Date:    2024/04/09 22:15:08
//===----------------------------------------------------------------------===//

#include "relay.h"
#include "logrus.h"
#include "netutil.h"

#include <thread>

#include <asio.hpp>

using asio::ip::tcp;

Relay::Relay(tcp::socket client_conn, tcp::socket server_conn,
             const tcp::endpoint &client_laddr,
             const tcp::endpoint &client_raddr,
             const tcp::endpoint &server_laddr,
             const tcp::endpoint &server_raddr)
    : client_(std::move(client_conn), client_laddr, client_raddr),
      server_(std::move(server_conn), server_laddr, server_raddr) {
  LOG_INFO("Forward", KV("from", to_string(client_raddr)),
           KV("via", to_string(client_laddr)),
           KV("to", to_string(server_raddr)));
}

Relay::~Relay() {
  LOG_INFO("Forward done", KV("from", to_string(client_.raddr_)),
           KV("via", to_string(client_.laddr_)),
           KV("to", to_string(server_.raddr_)),
           KV("in_bytes", client_.read_count_),
           KV("out_bytes", client_.write_count_));
}

void Relay::start() noexcept {
  auto client_buf = std::make_shared<std::vector<char>>(1024 * 32);
  auto server_buf = std::make_shared<std::vector<char>>(1024 * 32);
  io_copy(client_, server_, client_buf);
  io_copy(server_, client_, server_buf);
}

void Relay::io_copy(RelayConn &from, RelayConn &to, SharedBuffer buf) noexcept {
  auto self = shared_from_this();
  from.conn_.async_read_some(
      asio::buffer(*buf, buf->capacity()),
      [this, self, &from, &to, buf](asio::error_code ec, size_t nread) {
        if (ec) {
          if (ec == asio::error::eof) {
            LOG_DEBUG("Closed by", KV("laddr", to_string(from.laddr_)),
                      KV("remote", to_string(from.raddr_)));
            from.conn_.shutdown(asio::socket_base::shutdown_receive, ec);
            to.conn_.shutdown(asio::socket_base::shutdown_send, ec);
          } else {
            LOG_DEBUG("Fail to read from", KV("error", ec.message()),
                      KV("remote", to_string(from.raddr_)),
                      KV("laddr", to_string(from.laddr_)));
          }
          return;
        }
        LOG_TRACE("Read from", KV("raddr", to_string(from.raddr_)),
                  KV("laddr", to_string(from.laddr_)), KV("n", nread));
        from.read_count_ += nread;

        asio::async_write(
            to.conn_, asio::buffer(*buf, nread),
            [this, self, &from, &to, buf, nread](std::error_code ec,
                                                 size_t nwrite) {
              if (ec) {
                LOG_ERROR("Fail to write", KV("error", ec.message()),
                          KV("laddr", to_string(to.laddr_)),
                          KV("raddr", to_string(to.raddr_)));
                from.conn_.close(ec);
                to.conn_.close(ec);
                return;
              }
              LOG_TRACE("Write to", KV("laddr", to_string(to.laddr_)),
                        KV("raddr", to_string(to.raddr_)), KV("n", nwrite));
              to.write_count_ += nwrite;
              io_copy(from, to, buf);
            });
      });
}

RelayServer::RelayServer(asio::io_context &context,
                         const RelayEndpoints &endpoints)
    : endpoints_(endpoints), acceptor_(context, endpoints.listen) {
  do_accept();
}

void RelayServer::new_conn(tcp::socket client_conn) noexcept {
  std::error_code ec;
  tcp::endpoint client_laddr = client_conn.local_endpoint(ec);
  if (ec) {
    LOG_INFO("Fail to get client local addr", KV("err", ec.message()),
             KV("fd", client_conn.native_handle()));
    return;
  }
  tcp::endpoint client_raddr = client_conn.remote_endpoint(ec);
  if (ec) {
    LOG_INFO("Fail to get client remote addr", KV("err", ec.message()),
             KV("laddr", to_string(client_laddr)),
             KV("fd", client_conn.native_handle()));
    return;
  }

  LOG_INFO("New conn", KV("laddr", to_string(client_laddr)),
           KV("raddr", to_string(client_raddr)));

  tcp::socket server_conn(acceptor_.get_executor());
  if (endpoints_.src.port() > 0 || !endpoints_.src.address().is_unspecified()) {
    server_conn.bind(endpoints_.src, ec);
    if (ec) {
      LOG_ERROR("Fail to bind", KV("err", ec.message()),
                KV("src", to_string(endpoints_.src)));
      return;
    }
  }

  server_conn.connect(endpoints_.dst, ec);
  if (ec) {
    LOG_ERROR("Fail to connect", KV("error", ec.message()),
              KV("src", to_string(endpoints_.src)),
              KV("dst", to_string(endpoints_.dst)));
    return;
  }
  tcp::endpoint server_laddr = server_conn.local_endpoint(ec);
  if (ec) {
    LOG_ERROR("Fail to get server local addr", KV("err", ec.message()),
              KV("fd", server_conn.native_handle()),
              KV("client_raddr", to_string(client_raddr)));
    return;
  }

  LOG_DEBUG("Connected to", KV("laddr", to_string(server_laddr)),
            KV("raddr", to_string(endpoints_.dst)));
  std::make_shared<Relay>(std::move(client_conn), std::move(server_conn),
                          client_laddr, client_raddr, server_laddr,
                          endpoints_.dst)
      ->start();
}

void RelayServer::do_accept() {
  acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
    if (!ec) {
      new_conn(std::move(socket));
    } else {
      LOG_ERROR("Fail to accept", KV("error", ec.message()),
                KV("fd", acceptor_.native_handle()));
      return;
    }
    do_accept();
  });
}
