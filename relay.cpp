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
    : client_conn_(std::move(client_conn)),
      server_conn_(std::move(server_conn)) {
  LOG_INFO("Forward", KV("from", to_string(client_raddr)),
           KV("via", to_string(client_laddr)),
           KV("to", to_string(server_raddr)));
}

Relay::~Relay() {
  std::error_code ec;
  client_conn_.remote_endpoint(ec);
  LOG_INFO("Forward done",
           KV("from", to_string(client_conn_.remote_endpoint(ec))),
           KV("via", to_string(client_conn_.local_endpoint(ec))),
           KV("to", to_string(server_conn_.remote_endpoint(ec))));
}

void Relay::start() noexcept {
  auto client_buf = std::make_shared<std::vector<char>>(1024 * 32);
  auto server_buf = std::make_shared<std::vector<char>>(1024 * 32);
  io_copy(client_conn_, server_conn_, client_buf);
  io_copy(server_conn_, client_conn_, server_buf);
}

void Relay::io_copy(tcp::socket &from, tcp::socket &to,
                    SharedBuffer buf) noexcept {
  auto self = shared_from_this();
  from.async_read_some(
      asio::buffer(*buf, buf->capacity()),
      [this, self, &from, &to, buf](asio::error_code ec, size_t nread) {
        if (ec) {
          if (ec == asio::error::eof) {
            LOG_DEBUG("Closed by",
                      KV("laddr", to_string(from.local_endpoint(ec))),
                      KV("remote", to_string(from.remote_endpoint(ec))));
            from.shutdown(asio::socket_base::shutdown_receive, ec);
            to.shutdown(asio::socket_base::shutdown_send, ec);
          } else {
            LOG_DEBUG("Fail to read from", KV("error", ec.message()),
                      KV("remote", to_string(from.remote_endpoint(ec))),
                      KV("laddr", to_string(from.local_endpoint(ec))));
          }
          return;
        }
        LOG_TRACE("Read from", KV("raddr", to_string(from.remote_endpoint(ec))),
                  KV("laddr", to_string(from.local_endpoint(ec))),
                  KV("n", nread), KV("buf", buf->size()));
        async_write_all(self, from, to, buf, nread);
      });
}

void Relay::async_write_all(std::shared_ptr<Relay> self, tcp::socket &from,
                            tcp::socket &to, SharedBuffer buf,
                            size_t n) noexcept {
  asio::async_write(
      to, asio::buffer(*buf, n),
      [this, self, &from, &to, buf, n](std::error_code ec, size_t nwrite) {
        if (ec) {
          LOG_ERROR("Fail to write", KV("error", ec.message()),
                    KV("laddr", to_string(to.local_endpoint(ec))),
                    KV("raddr", to_string(to.remote_endpoint(ec))));
          from.close(ec);
          to.close(ec);
          return;
        }
        LOG_TRACE("Write to", KV("laddr", to_string(to.local_endpoint(ec))),
                  KV("raddr", to_string(to.remote_endpoint(ec))),
                  KV("n", nwrite), KV("buf", n));
        if (nwrite < n) {
          buf->erase(buf->begin(), buf->begin() + nwrite);
          async_write_all(self, from, to, buf, n - nwrite);
        } else {
          io_copy(from, to, buf);
        }
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
  if (ec)
    return;
  tcp::endpoint client_raddr = client_conn.remote_endpoint(ec);
  if (ec)
    return;

  LOG_INFO("New conn", KV("laddr", to_string(client_laddr)),
           KV("raddr", to_string(client_raddr)));

  tcp::socket server_conn(acceptor_.get_executor());
  if (endpoints_.src.port() > 0 || !endpoints_.src.address().is_unspecified()) {
    server_conn.bind(endpoints_.src, ec);
    if (ec) {
      LOG_ERROR("Fail to bind", KV("src", to_string(endpoints_.src)));
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
  if (ec)
    return;

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
