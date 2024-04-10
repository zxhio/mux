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
#include "proxy.h"

#include <getopt.h>
#include <stdio.h>

#define INVALID_ARG_CHECK_EXIT(arg_k, arg_v, err_num)                          \
  if (err_num) {                                                               \
    fprintf(stderr, "Invalid %s '%s'\n", arg_k, arg_v);                        \
    exit(1);                                                                   \
  }

static struct option opts[] = {
    {"listen", required_argument, NULL, 'l'},
    {"dst", required_argument, NULL, 'd'},
    {"src", optional_argument, NULL, 's'},
    {"file", optional_argument, NULL, 'f'},
    {"verbose", no_argument, NULL, 'V'},
    {"help", no_argument, NULL, 'h'},
    {0, 0, 0, 0},
};

static void usage(char *argv1) {
  fprintf(stderr, "Usage: %s\n", argv1);
  fprintf(stderr, "  -l,  --listen     Listen address or port\n");
  fprintf(stderr, "  -d,  --dst        Destination address\n");
  fprintf(stderr, "  -s,  --src        Source address or ip\n");
  fprintf(stderr, "  -f,  --file       Log file path\n");
  fprintf(stderr, "  -V,  --verbose    Verbose output\n");
  fprintf(stderr, "  -h,  --help       Help\n");
}

static IPAddr must_parse_addr_args(const char *s) {
  std::string addr_text(s);
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

struct CommandArgs {
  IPAddr listen_addr;
  IPAddr dst_addr;
  IPAddr src_addr;
  std::string logfile;
  bool verbose;
};

static void must_parse_command_line(int argc, char *argv[], CommandArgs &args) {
  while (1) {
    int longidnd;
    int c = getopt_long(argc, argv, "l:d:s:f:Vh", opts, &longidnd);
    if (c < 0)
      break;
    char *arg = optarg ? optarg : argv[optind];

    try {
      switch (c) {
      case 'l':
        args.listen_addr = must_parse_addr_args(arg);
        break;
      case 'd':
        args.dst_addr = must_parse_addr_args(arg);
        break;
      case 's':
        args.src_addr = must_parse_addr_args(arg);
        break;
      case 'f':
        args.logfile = arg;
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
    } catch (const std::exception &e) {
      fprintf(stderr, "Invalid option (-%c/--%s) value (%s) %s\n", c,
              opts[longidnd].name, arg, e.what());
      exit(1);
    }
  }
}

static void init_logging(const CommandArgs &args) {
  if (!args.logfile.empty())
    logrus::set_rotating(args.logfile, 1024 * 1024 * 10, 10);
  if (args.verbose)
    logrus::set_level(logrus::kTrace);
  logrus::set_pattern("%^%l%$ %Y%m%d %H:%M:%S %t %v");
  logrus::flush_every(std::chrono::seconds(1));
}

int main(int argc, char *argv[]) {
  CommandArgs args;
  must_parse_command_line(argc, argv, args);

  init_logging(args);
  LOG_INFO("=== mux start ===");

  int sockfd = create_and_bind_listener(args.listen_addr, true);
  if (sockfd < 0)
    LOG_FATAL("Fatal to listen", KERR(errno));
  LOG_INFO("Listen on", KV("addr", args.listen_addr.format()));

  run_event_loop(sockfd, args.src_addr, args.dst_addr);
  return 0;
}
