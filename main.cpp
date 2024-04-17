//===- main.cpp - Multiplexing routine --------------------------*- C++ -*-===//
//
/// \file
///
//
// Author:  zxh
// Date:    2024/04/09 22:24:02
//===----------------------------------------------------------------------===//

#include "errors.h"
#include "logrus.h"
#include "netutil.h"
#include "relay.h"

#include <getopt.h>
#include <stdio.h>

#include <sstream>
#include <thread>

#include <asio.hpp>

using namespace asio::ip;

static struct option opts[] = {
    {"listen", required_argument, NULL, 'l'},
    {"dst", required_argument, NULL, 'd'},
    {"src", optional_argument, NULL, 's'},
    {"relay_list", optional_argument, NULL, 'r'},
    {"file", optional_argument, NULL, 'f'},
    {"verbose", no_argument, NULL, 'V'},
    {"help", no_argument, NULL, 'h'},
    {0, 0, 0, 0},
};

#define USAGE_LINE(line) fprintf(stderr, "%s\n", line);

static void usage(char *argv1) {
  fprintf(stderr, "Usage: %s\n", argv1);
  USAGE_LINE("  -l,  --listen      Listen address or port");
  USAGE_LINE("  -d,  --dst         Destination address");
  USAGE_LINE("  -s,  --src         Source address or ip");
  USAGE_LINE("  -r,  --relay_list  Relay address tuple list [-l,-s,-d/]+");
  USAGE_LINE("  -f,  --file        Log file path");
  USAGE_LINE("  -V,  --verbose     Verbose output");
  USAGE_LINE("  -h,  --help        Help");
}

struct CommandArgs {
  std::vector<RelayEndpoints> addr_tuple_list;
  std::string logfile;
  bool verbose;
};

static tcp::endpoint parse_addr(const std::string &hostport) {
  if (std::all_of(hostport.begin(), hostport.end(),
                  [](char c) { return std::isdigit(c); })) {
    int port = std::stoi(hostport);
    if (port < 0 || port > 65535)
      throw std::system_error(
          std::error_code(AddrErrInvalidPort, address_category()), hostport);
    return tcp::endpoint(tcp::v4(), port);
  }

  std::error_code ec;
  std::pair<std::string, std::string> p = split_host_port(hostport, ec);
  if (ec)
    throw std::system_error(ec, hostport);

  int port = std::stoi(p.second);
  if (port < 0 || port > 65535)
    throw std::system_error(
        std::error_code(AddrErrInvalidPort, address_category()), hostport);
  return tcp::endpoint(address::from_string(p.first), std::stoi(p.second));
}

std::vector<std::string> split(const std::string &s, char delim) {
  std::vector<std::string> ans;
  std::istringstream stream(s);
  std::string word;
  while (getline(stream, word, delim))
    ans.push_back(word);
  return ans;
}

// listen_addr,src_addr,dst_addr/
// 80,192.168.32.210:8000,192.168.32.251:8000/192.168.32.245:80,192.168.32.251:8000
static std::vector<RelayEndpoints> parse_addr_tuple(const char *s) {
  std::vector<RelayEndpoints> addr_tuple_list;
  std::vector<std::string> tuple_str_list = split(s, '/');
  for (const auto &tuple_str : tuple_str_list) {
    std::vector<std::string> addr_str_list = split(tuple_str, ',');
    if (addr_str_list.size() < 2)
      throw std::logic_error("tuple address count must > 2");

    RelayEndpoints t;
    t.listen = parse_addr(addr_str_list[0]);
    if (addr_str_list.size() == 2) {
      t.dst = parse_addr(addr_str_list[1]);
    } else {
      t.src = parse_addr(addr_str_list[1]);
      t.dst = parse_addr(addr_str_list[2]);
    }
    addr_tuple_list.push_back(t);
  }
  return addr_tuple_list;
}

static void
check_addr_tuple_valid(const std::vector<RelayEndpoints> &addr_tuple_list) {
  for (const auto &t : addr_tuple_list) {
    std::string dst_desc = "dst_addr (" + to_string(t.dst) + ")";
    if (t.dst.port() == 0)
      throw std::logic_error(dst_desc + " port can't be 0");
    if (t.dst.address().is_unspecified())
      throw std::logic_error(dst_desc + " ip must be specified");
  }
}

static void parse_command_line(int argc, char *argv[], CommandArgs &args) {
  RelayEndpoints addr_tuple;
  while (1) {
    int longidnd;
    int c = getopt_long(argc, argv, "l:d:s:r:f:Vh", opts, &longidnd);
    if (c < 0)
      break;
    char *arg = optarg ? optarg : argv[optind];

    switch (c) {
    case 'l':
      addr_tuple.listen = parse_addr(arg);
      break;
    case 'd':
      addr_tuple.dst = parse_addr(arg);
      break;
    case 's':
      addr_tuple.src = parse_addr(arg);
      break;
    case 'f':
      args.logfile = arg;
      break;
    case 'r':
      args.addr_tuple_list = parse_addr_tuple(arg);
      break;
    case 'V':
      args.verbose = true;
      break;
    case 'h':
      usage(argv[0]);
      exit(0);
    default:
      usage(argv[0]);
      exit(1);
    }
  }

  if (addr_tuple.listen.port() > 0 && addr_tuple.dst.port() > 0)
    args.addr_tuple_list.push_back(addr_tuple);

  check_addr_tuple_valid(args.addr_tuple_list);
}

static void init_logging(const CommandArgs &args) {
  if (!args.logfile.empty())
    logrus::set_rotating(args.logfile, 1024 * 1024 * 10, 10);
  if (args.verbose)
    logrus::set_level(logrus::kTrace);
  logrus::set_pattern("%^%l%$ %Y%m%d %H:%M:%S %t %v");
  logrus::flush_every(std::chrono::seconds(1));
}

static long get_cpu_count() { return sysconf(_SC_NPROCESSORS_CONF); }

int main(int argc, char *argv[]) {
  CommandArgs args;
  try {
    parse_command_line(argc, argv, args);
  } catch (const std::exception &e) {
    LOG_FATAL("Fatal parse command line", KV("error", e.what()));
  }

  init_logging(args);
  LOG_INFO("=== mux start ===");

  try {
    asio::io_context io_context;
    std::vector<std::unique_ptr<RelayServer>> server_list;

    for (const RelayEndpoints &t : args.addr_tuple_list) {
      LOG_INFO("Listen on", KV("addr", to_string(t.listen)),
               KV("src", to_string(t.src)), KV("dst", to_string(t.dst)));
      server_list.emplace_back(std::make_unique<RelayServer>(io_context, t));
    }
    io_context.run();
  } catch (const std::exception &e) {
    LOG_FATAL("Fatal to run mux", KV("error", e.what()));
  }

  LOG_INFO("=== mux quit ===");
  return 0;
}
