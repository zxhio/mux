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
      start_time_(std::chrono::system_clock::now()) {
  LOG_INFO("Forward", KV("from", to_string(client_raddr)),
           KV("via", to_string(client_laddr)),
           KV("to", to_string(server_raddr)));
}

Relay::~Relay() {
  auto dur = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now() - start_time_)
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

static int create_eventfd(std::error_code &ec) noexcept {
  int efd = eventfd(0, O_NONBLOCK);
  if (efd < 0) {
    ec = std::error_code(errno, std::system_category());
    return -1;
  }
  return efd;
}

static int create_eventfd() {
  std::error_code ec;
  int efd = create_eventfd(ec);
  asio::detail::throw_error(ec);
  return efd;
}

RelayIOContext::RelayIOContext(
    size_t id, const std::vector<RelayEndpointTuple> &endpoint_tuples)
    : id_(id), context_(), eventfd_stream_(context_, create_eventfd()),
      endpoint_tuples_(endpoint_tuples) {
  wait_eventfd();
}

void RelayIOContext::notify(int connfd,
                            const RelayEndpointTuple &endpoint_tuple) {
  eventfd_stream_.async_wait(
      asio::posix::stream_descriptor::wait_write, [this](std::error_code ec) {
        if (ec) {
          LOG_ERROR("Fail to async_wait", KV("error", ec.message()));
          return;
        }

        LOG_DEBUG("Notify io_context", KV("id", id_));
        if (::eventfd_write(eventfd_stream_.native_handle(), 1) < 0)
          LOG_ERROR("Fail to write eventfd", KERR(errno));
      });

  new_conn(connfd, endpoint_tuple);
}

void RelayIOContext::wait_eventfd() noexcept {
  eventfd_stream_.async_wait(
      asio::posix::stream_descriptor::wait_read, [this](std::error_code ec) {
        if (ec) {
          LOG_ERROR("Fail to async_wait eventfd", KV("error", ec.message()));
          return;
        }

        uint64_t v;
        ::eventfd_read(eventfd_stream_.native_handle(), &v);

        wait_eventfd();
      });
}

void RelayIOContext::new_conn(
    int connfd, const RelayEndpointTuple &endpoint_tuple) noexcept {
  std::error_code ec;
  auto client_conn = std::make_shared<tcp::socket>(context_);
  client_conn->assign(endpoint_tuple.listen.protocol(), connfd, ec);
  if (ec) {
    LOG_ERROR("Fail to make tcp socket", KV("error", ec.message()),
              KV("fd", connfd), KV("efd", eventfd_stream_.native_handle()),
              KV("id", id_));
    ::close(connfd);
    return;
  }

  tcp::endpoint client_laddr = client_conn->local_endpoint(ec);
  if (ec) {
    LOG_INFO("Fail to get client local addr", KV("err", ec.message()),
             KV("fd", client_conn->native_handle()));
    return;
  }
  tcp::endpoint client_raddr = client_conn->remote_endpoint(ec);
  if (ec) {
    LOG_INFO("Fail to get client remote addr", KV("err", ec.message()),
             KV("laddr", to_string(client_laddr)),
             KV("fd", client_conn->native_handle()));
    return;
  }
  LOG_INFO("New conn", KV("laddr", to_string(client_laddr)),
           KV("raddr", to_string(client_raddr)), KV("fd", connfd));

  auto server_conn = std::make_shared<tcp::socket>(context_);
  if (endpoint_tuple.src.port() > 0 ||
      !endpoint_tuple.src.address().is_unspecified()) {
    server_conn->bind(endpoint_tuple.src, ec);
    if (ec) {
      LOG_ERROR("Fail to bind", KV("err", ec.message()),
                KV("src", to_string(endpoint_tuple.src)));
      return;
    }
  }

  server_conn->async_connect(endpoint_tuple.dst, [=](std::error_code ec) {
    if (ec) {
      LOG_ERROR("Fail to connect", KV("error", ec.message()),
                KV("src", to_string(endpoint_tuple.src)),
                KV("dst", to_string(endpoint_tuple.dst)));
      return;
    }

    tcp::endpoint server_laddr = server_conn->local_endpoint(ec);
    if (ec) {
      LOG_ERROR("Fail to get server local addr", KV("err", ec.message()),
                KV("fd", server_conn->native_handle()),
                KV("client_raddr", to_string(client_raddr)));
      return;
    }
    LOG_DEBUG("Connected to", KV("laddr", to_string(server_laddr)),
              KV("raddr", to_string(endpoint_tuple.dst)));

    std::make_shared<Relay>(std::move(*client_conn), std::move(*server_conn),
                            client_laddr, client_raddr, client_raddr,
                            endpoint_tuple.dst)
        ->start();
  });
}

RelayServer::RelayServer(std::vector<RelayEndpointTuple> endpoint_tuples)
    : endpoint_tuples_(endpoint_tuples), relay_context_idx_(0) {}

void RelayServer::run(size_t co_num) {
  co_num = std::max(co_num, size_t(1));
  LOG_INFO("Relay Server run", KV("co_num", co_num));

  for (size_t i = 0; i < co_num; i++)
    relay_contexts_.emplace_back(
        std::make_shared<RelayIOContext>(i, endpoint_tuples_));

  for (const auto &et : endpoint_tuples_) {
    LOG_INFO("Listen on", KV("addr", to_string(et.listen)),
             KV("via", to_string(et.src)), KV("to", to_string(et.dst)));
    auto a = std::make_shared<Acceptor>(relay_contexts_[0]->context(), et);
    do_accept(*a);
    acceptors_.emplace_back(a);
  }

  std::vector<std::thread> threads;
  for (int i = 1; i < relay_contexts_.size(); i++)
    threads.emplace_back(
        std::thread([i, this]() { relay_contexts_[i]->run(); }));
  relay_contexts_[0]->run();
}

void RelayServer::do_accept(Acceptor &ra) noexcept {
  ra.acceptor_.async_wait(
      asio::socket_base::wait_read, [this, &ra](std::error_code ec) {
        int connfd = ::accept(ra.acceptor_.native_handle(), nullptr, nullptr);
        if (connfd < 0) {
          LOG_ERROR("Fail to accept", KERR(errno));
          return;
        }

        relay_context_idx_++;
        if (relay_context_idx_ % relay_contexts_.size() == 0)
          relay_context_idx_++;
        auto ctx = relay_contexts_[relay_context_idx_ % relay_contexts_.size()];
        ctx->notify(connfd, ra.endpoint_tuple_);

        do_accept(ra);
      });
}
