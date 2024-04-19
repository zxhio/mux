//===- relay.h - Relay interface --------------------------------*- C++ -*-===//
//
/// \file
/// Define relay command interface.
//
// Author:  zxh
// Date:    2024/04/09 22:16:32
//===----------------------------------------------------------------------===//

#pragma once

#include <asio.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/streambuf.hpp>

struct RelayEndpointTuple {
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
  using SharedBuffer = std::shared_ptr<asio::streambuf>;
  using TimePoint = std::chrono::time_point<std::chrono::system_clock,
                                            std::chrono::nanoseconds>;

  Relay(asio::ip::tcp::socket client_conn, asio::ip::tcp::socket server_conn,
        const asio::ip::tcp::endpoint &client_laddr,
        const asio::ip::tcp::endpoint &client_raddr,
        const asio::ip::tcp::endpoint &server_laddr,
        const asio::ip::tcp::endpoint &server_raddr);

  ~Relay();

  void start() noexcept;

private:
  void io_copy(RelayConn &from, RelayConn &to, SharedBuffer buf,
               bool need_grow) noexcept;

  void write_all(RelayConn &from, RelayConn &to, SharedBuffer buf,
                 bool need_grow) noexcept;

  RelayConn client_;
  RelayConn server_;
  TimePoint start_time_;
};

class RelayIOContext : private asio::noncopyable {
public:
  RelayIOContext() = delete;
  RelayIOContext(size_t id,
                 const std::vector<RelayEndpointTuple> &endpoint_tuples);

  void run() { context_.run(); }
  void run(std::error_code &ec) { context_.run(ec); }

  void notify(int fd, const RelayEndpointTuple &endpoint_tuple);

  asio::io_context &context() { return context_; }

private:
  void wait_eventfd() noexcept;

  void new_conn(int connfd, const RelayEndpointTuple &endpoint_tuple) noexcept;

  size_t id_;
  asio::io_context context_;
  asio::posix::stream_descriptor eventfd_stream_;
  std::vector<RelayEndpointTuple> endpoint_tuples_;
};

class RelayServer {
public:
  RelayServer(std::vector<RelayEndpointTuple> endpoint_tuples);

  void run(size_t context_num);

private:
  struct Acceptor {
    RelayEndpointTuple endpoint_tuple_;
    asio::ip::tcp::acceptor acceptor_;

    Acceptor(asio::io_context &context,
             const RelayEndpointTuple &endpoint_tuple)
        : endpoint_tuple_(endpoint_tuple),
          acceptor_(context, endpoint_tuple.listen) {}
  };

  void do_accept(Acceptor &acceptor) noexcept;

  std::vector<RelayEndpointTuple> endpoint_tuples_;
  std::vector<std::shared_ptr<Acceptor>> acceptors_;
  std::vector<std::shared_ptr<RelayIOContext>> relay_contexts_;
  size_t relay_context_idx_;
};