#include "core/common.hpp"
#include "relay/relay.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <string>

namespace {

#ifndef KIKO_VERSION
#define KIKO_VERSION "dev"
#endif

std::string next_arg(int& index, int argc, char** argv, const char* name) {
  if (index + 1 >= argc) throw kiko::KikoError(std::string("missing value for ") + name);
  return argv[++index];
}

void print_help() {
  std::cout
      << "Usage: kiko-relayd [relay] [--listen HOST:PORT] [--pass PASS] [--room-ttl SEC] "
         "[--room-cleanup-interval SEC]\n\n"
      << "Run the kiko rendezvous relay without the full CLI/TUI frontend.\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string listen = "0.0.0.0:9000";
  kiko::RelayServerConfig config;

  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "relay") {
        continue;
      }
      if (arg == "--listen") {
        listen = next_arg(i, argc, argv, "--listen");
      } else if (arg == "--pass") {
        config.password = next_arg(i, argc, argv, "--pass");
      } else if (arg == "--room-ttl") {
        const auto value = kiko::parse_u64_strict(next_arg(i, argc, argv, "--room-ttl"));
        if (!value || *value == 0) throw kiko::KikoError("invalid --room-ttl");
        config.room_ttl = std::chrono::seconds(*value);
      } else if (arg == "--room-cleanup-interval") {
        const auto value = kiko::parse_u64_strict(next_arg(i, argc, argv, "--room-cleanup-interval"));
        if (!value || *value == 0) throw kiko::KikoError("invalid --room-cleanup-interval");
        config.cleanup_interval = std::chrono::seconds(*value);
      } else if (arg == "--help" || arg == "-h") {
        print_help();
        return 0;
      } else if (arg == "--version") {
        std::cout << "kiko-relayd " << KIKO_VERSION << "\n";
        return 0;
      } else {
        throw kiko::KikoError("unknown argument: " + arg);
      }
    }

    return kiko::run_relay(kiko::parse_bind_endpoint(listen, 9000), config, /*announce_lan=*/false);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
