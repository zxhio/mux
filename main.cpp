//===- main.cpp - Multiplexing routine --------------------------*- C++ -*-===//
//
/// \file
///
//
// Author:  zxh
// Date:    2024/04/09 22:24:02
//===----------------------------------------------------------------------===//

#include "conn.h"
#include "logrus.h"
#include "netutil.h"
#include "relay.h"

#include <getopt.h>
#include <stdio.h>

#include <sstream>

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
  std::vector<RelayIPAddrTuple> addr_tuple_list;
  std::string logfile;
  bool verbose;
};

static IPAddr must_parse_addr(const std::string &addr_text) {
  std::string::size_type n = addr_text.find_last_of(':');
  if (n == std::string::npos) {
    // Only ip
    if (!std::all_of(addr_text.begin(), addr_text.end(),
                     [](char c) { return std::isdigit(c); }))
      return parse_iptext_port(addr_text, 0);

    // Only port
    int port = std::stoi(addr_text);
    if (port >= 0 && port <= 65536) {
      return parse_iptext_port("0.0.0.0", uint16_t(port));
    } else {
      return IPAddr();
    }
  }

  std::string ip_part = addr_text.substr(0, n);
  std::string port_part = addr_text.substr(n + 1);
  int port = std::stoi(port_part);
  if (port < 0) {
    return IPAddr();
  }
  return parse_iptext_port(ip_part, uint16_t(port));
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
// 80,192.168.32.210:8000,192.168.32.251:8000/192.168.32.245:80,192.168.32.251:8000;
static std::vector<RelayIPAddrTuple> must_parse_addr_tuple(const char *s) {
  std::vector<RelayIPAddrTuple> addr_tuple_list;
  std::vector<std::string> tuple_str_list = split(s, '/');
  for (const auto &tuple_str : tuple_str_list) {
    std::vector<std::string> addr_str_list = split(tuple_str, ',');
    if (addr_str_list.size() < 2)
      throw std::logic_error("tuple address count must > 2");

    RelayIPAddrTuple t;
    t.listen = must_parse_addr(addr_str_list[0]);
    if (addr_str_list.size() == 2) {
      t.dst = must_parse_addr(addr_str_list[1]);
    } else {
      t.src = must_parse_addr(addr_str_list[1]);
      t.dst = must_parse_addr(addr_str_list[2]);
    }
    addr_tuple_list.push_back(t);
  }
  return addr_tuple_list;
}

static void
check_addr_tuple_valid(const std::vector<RelayIPAddrTuple> &addr_tuple_list) {
  for (const auto &t : addr_tuple_list) {
    if (t.dst.port() == 0)
      throw t.dst.format() + "dst_addr port can't be 0";

    std::string addr_text = t.dst.format();
    if (addr_text.find("0.0.0.0") != std::string::npos)
      throw std::logic_error(t.dst.format() + " dst_addr ip can't be 0.0.0.0");
    else if (addr_text.find("[::]") != std::string::npos)
      throw std::logic_error(t.dst.format() + " dst_addr ip can't be [::]");
  }
}

static void must_parse_command_line(int argc, char *argv[], CommandArgs &args) {
  RelayIPAddrTuple addr_tuple;
  while (1) {
    int longidnd;
    int c = getopt_long(argc, argv, "l:d:s:r:f:Vh", opts, &longidnd);
    if (c < 0)
      break;
    char *arg = optarg ? optarg : argv[optind];

    switch (c) {
    case 'l':
      addr_tuple.listen = must_parse_addr(arg);
      break;
    case 'd':
      addr_tuple.dst = must_parse_addr(arg);
      break;
    case 's':
      addr_tuple.src = must_parse_addr(arg);
      break;
    case 'f':
      args.logfile = arg;
      break;
    case 'r':
      args.addr_tuple_list = must_parse_addr_tuple(arg);
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

  // address tuple must contains listen_addr and dst_addr
  if (addr_tuple.listen.addr()->sa_family != AF_UNSPEC &&
      addr_tuple.dst.addr()->sa_family != AF_UNSPEC)
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
    must_parse_command_line(argc, argv, args);
  } catch (const std::exception &e) {
    LOG_FATAL("Fatal parse command line", KV("error", e.what()));
  }

  init_logging(args);
  LOG_INFO("=== mux start ===");

  try {
    EventLoopPool p = create_event_loop_pool(get_cpu_count());
    for (const RelayIPAddrTuple &t : args.addr_tuple_list) {
      LOG_INFO("Listen on", KV("addr", t.listen.format()),
               KV("src", t.src.format()), KV("dst", t.dst.format()));
      int sockfd = create_and_bind_listener(t.listen, true);
      attach_listener(p, sockfd, t);
    }
    run_event_loop_pool(p);
  } catch (const std::exception &e) {
    LOG_FATAL("Fatal to run mux", KV("error", e.what()));
  }

  LOG_INFO("=== mux quit ===");
  return 0;
}
