// Copyright 2017 Panagiotis Ktistakis <panktist@gmail.com>
//
// This file is part of crocks.
//
// crocks is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// crocks is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with crocks.  If not, see <http://www.gnu.org/licenses/>.

#include <arpa/inet.h>
#include <getopt.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <string>

#include "src/common/util.h"
#include "src/server/async_server.h"

const std::string version("crocks v0.1.0");
const std::string usage_message(
    "Usage: crocks [options]\n"
    "\n"
    "Start a crocks server.\n"
    "\n"
    "Options:\n"
    "  -p, --path <path>      RocksDB database path.\n"
    "  -o, --options <path>   RocksDB options file path.\n"
    "  -H, --host <hostname>  Node hostname [default: localhost].\n"
    "  -P, --port <port>      Listening port [default: chosen by OS].\n"
    "  -e, --etcd <address>   Etcd address [default: localhost:2379].\n"
    "  -t, --threads <int>    Number of serving threads [default: 2].\n"
    "  -s, --shards <int>     Number of initial shards [default: 10].\n"
    "  -d, --daemon           Daemonize process.\n"
    "  -v, --version          Show version and exit.\n"
    "  -h, --help             Show this help message and exit.\n");

std::string GetIP() {
  struct ifaddrs* head = nullptr;
  struct ifaddrs* ifa = nullptr;
  char buf[INET_ADDRSTRLEN] = "localhost";

  getifaddrs(&head);
  for (ifa = head; ifa != nullptr; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || (std::string(ifa->ifa_name) == "lo"))
      continue;
    if (ifa->ifa_addr->sa_family == AF_INET) {
      inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, buf,
                INET_ADDRSTRLEN);
      break;
    }
  }

  if (head != nullptr)
    freeifaddrs(head);

  return std::string(buf);
}

int main(int argc, char** argv) {
  char* dbpath = nullptr;
  std::string options_path;
  std::string hostname = GetIP();
  std::string port = "0";
  std::string etcd_address = crocks::GetEtcdEndpoint();
  int num_threads = 2;
  int num_shards = 10;

  const char* optstring = "p:o:H:P:e:t:s:dvh";
  static struct option longopts[] = {
      // clang-format off
      {"path",    required_argument, 0, 'p'},
      {"options", required_argument, 0, 'o'},
      {"host",    required_argument, 0, 'H'},
      {"port",    required_argument, 0, 'P'},
      {"etcd",    required_argument, 0, 'e'},
      {"threads", required_argument, 0, 't'},
      {"shards",  required_argument, 0, 's'},
      {"daemon",  no_argument,       0, 'd'},
      {"version", no_argument,       0, 'v'},
      {"help",    no_argument,       0, 'h'},
      {0, 0, 0, 0},
      // clang-format on
  };
  int c, index = 0;

  // Command-line options
  while ((c = getopt_long(argc, argv, optstring, longopts, &index)) != -1) {
    switch (c) {
      case 'p':
        dbpath = optarg;
        break;
      case 'o':
        options_path = optarg;
        break;
      case 'H':
        hostname = optarg;
        break;
      case 'P':
        port = optarg;
        break;
      case 'e':
        etcd_address = optarg;
        break;
      case 't':
        num_threads = std::stoi(optarg);
        break;
      case 's':
        num_shards = std::stoi(optarg);
        break;
      case 'd':
        if (daemon(0, 0) < 0) {
          perror("daemon");
          exit(EXIT_FAILURE);
        }
        break;
      case 'v':
        std::cout << version << std::endl;
        exit(EXIT_SUCCESS);
      case 'h':
        std::cout << usage_message;
        exit(EXIT_SUCCESS);
      default:
        std::cout << usage_message;
        exit(EXIT_FAILURE);
    }
  }

  if (dbpath == nullptr) {
    char dbpath_template[] = "/tmp/testdb_XXXXXX";
    dbpath = mkdtemp(dbpath_template);
    if (dbpath == nullptr) {
      perror("mkdtemp");
      exit(EXIT_FAILURE);
    }
  }

  std::string listening_address = "0.0.0.0:" + port;

  // Start server
  crocks::AsyncServer server(etcd_address, dbpath, options_path, num_threads);
  server.Init(listening_address, hostname, num_shards);
  server.Run();

  return 0;
}
