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

enum StreamBufCapcity : size_t {
  kSmall = 1024,
  kMedium = 1024 * 4,
  kLarge = 1024 * 16,
  kXLarge = 1024 * 64,
};

static asio::streambuf::mutable_buffers_type
make_prepare_buf(Relay::SharedBuffer buf, bool need_grow) {
  if (!need_grow)
    return buf->prepare(buf->capacity() - buf->size());

  size_t new_cap;
  if (buf->capacity() < StreamBufCapcity::kSmall)
    new_cap = StreamBufCapcity::kSmall;
  else if (buf->capacity() < kMedium)
    new_cap = StreamBufCapcity::kMedium;
  else if (buf->capacity() < kLarge)
    new_cap = StreamBufCapcity::kLarge;
  else
    new_cap = StreamBufCapcity::kXLarge;

  return buf->prepare(std::min(new_cap, buf->max_size()) - buf->size());
}

Relay::Relay(tcp::socket client_conn, tcp::socket server_conn,
             const tcp::endpoint &client_laddr,
             const tcp::endpoint &client_raddr,
             const tcp::endpoint &server_laddr,
             const tcp::endpoint &server_raddr)
    : client_(std::move(client_conn), client_laddr, client_raddr),
      server_(std::move(server_conn), server_laddr, server_raddr),
      start_(std::chrono::system_clock::now()) {
  LOG_INFO("Forward", KV("from", to_string(client_raddr)),
           KV("via", to_string(client_laddr)),
           KV("to", to_string(server_raddr)));
}

Relay::~Relay() {
  auto dur = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now() - start_)
                 .count();
  LOG_INFO("Forward done", KV("from", to_string(client_.raddr_)),
           KV("via", to_string(client_.laddr_)),
           KV("to", to_string(server_.raddr_)),
           KV("in_bytes", client_.read_count_),
           KV("out_bytes", client_.write_count_), KV("dur", dur));
}

void Relay::start() noexcept {
  auto client_buf = std::make_shared<asio::streambuf>(1024 * 128);
  auto server_buf = std::make_shared<asio::streambuf>(1024 * 128);
  io_copy(client_, server_, client_buf, false);
  io_copy(server_, client_, server_buf, false);
}

void Relay::io_copy(RelayConn &from, RelayConn &to, SharedBuffer buf,
                    bool grow) noexcept {
  auto self = shared_from_this();
  from.conn_.async_read_some(
      make_prepare_buf(buf, grow),
      [this, self, &from, &to, buf](asio::error_code ec, size_t n) {
        if (ec) {
          if (ec == asio::error::eof) {
            LOG_DEBUG("Closed by", KV("laddr", to_string(from.laddr_)),
                      KV("raddr", to_string(from.raddr_)));
            from.conn_.shutdown(asio::socket_base::shutdown_receive, ec);
            to.conn_.shutdown(asio::socket_base::shutdown_send, ec);
          } else {
            LOG_DEBUG("Fail to read from", KV("error", ec.message()),
                      KV("laddr", to_string(from.laddr_)),
                      KV("raddr", to_string(from.raddr_)));
          }
          return;
        }
        LOG_TRACE("Read", KV("laddr", to_string(from.laddr_)),
                  KV("raddr", to_string(from.raddr_)), KV("n", n));
        from.read_count_ += n;
        buf->commit(n);
        write_all(from, to, buf, buf->capacity() == n);
      });
}

void Relay::write_all(RelayConn &from, RelayConn &to, SharedBuffer buf,
                      bool need_grow) noexcept {
  auto self = shared_from_this();
  asio::async_write(
      to.conn_, buf->data(),
      [this, self, &from, &to, buf, need_grow](std::error_code ec, size_t n) {
        if (ec) {
          LOG_ERROR("Fail to write", KV("error", ec.message()),
                    KV("laddr", to_string(to.laddr_)),
                    KV("raddr", to_string(to.raddr_)));
          from.conn_.close(ec);
          to.conn_.close(ec);
          return;
        }
        LOG_TRACE("Write", KV("laddr", to_string(to.laddr_)),
                  KV("raddr", to_string(to.raddr_)), KV("n", n));
        to.write_count_ += n;
        buf->consume(n);
        if (buf->size() > 0)
          write_all(from, to, buf, need_grow);
        if (buf->size() < buf->capacity())
          io_copy(from, to, buf, need_grow);
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
