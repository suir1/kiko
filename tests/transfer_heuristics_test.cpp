#include "transfer/transfer_heuristics.hpp"

#include <iostream>

int main() {
  using namespace kiko;

  if (should_compress_path("photo.jpg")) {
    std::cerr << "FAIL: jpg should skip compression\n";
    return 1;
  }
  if (!should_compress_path("data.bin")) {
    std::cerr << "FAIL: bin should use compression\n";
    return 1;
  }
  if (!should_compress_path("archive.tar")) {
    std::cerr << "FAIL: tar should still compress (uncompressed tarball)\n";
    return 1;
  }

  if (recommend_connections(20, 1024) != 1) {
    std::cerr << "FAIL: tiny file should use 1 connection\n";
    return 1;
  }
  if (recommend_connections(200, 50 * 1024 * 1024) != 2) {
    std::cerr << "FAIL: high RTT medium file should use 2 connections\n";
    return 1;
  }
  if (recommend_connections(30, 600 * 1024 * 1024) != 8) {
    std::cerr << "FAIL: low RTT large file should use 8 connections\n";
    return 1;
  }

  std::cout << "transfer_heuristics_test ok\n";
  return 0;
}
