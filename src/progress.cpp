#include "progress.hpp"

#include "platform.hpp"
#include "qrcode_print.hpp"

#include <iostream>

namespace kiko {

void CliReporter::status(const std::string& message) { std::cout << message << "\n"; }

void CliReporter::connectivity_report(const std::string& report) {
  std::cout << "punch report:\n" << report;
}

void CliReporter::handshake_ok() { std::cout << "pake handshake ok\n"; }

void CliReporter::code_ready(const std::string& code, bool show_qrcode) {
  std::cout << "code: " << code << "\n";
  if (show_qrcode && stdin_is_tty()) print_qrcode(std::cout, code);
}

void CliReporter::transfer_overview(std::size_t file_count, std::uint64_t total_bytes) {
  std::cout << "incoming " << file_count << " file(s), " << total_bytes << " bytes\n";
}

void CliReporter::file_start(const std::string& path, std::uint64_t size) {
  std::cout << "-> " << path << " (" << size << " bytes)\n";
}

void CliReporter::file_complete(const std::string& path, std::uint64_t size, bool verified) {
  if (verified) {
    std::cout << "received " << path << " (" << size << " bytes, sha256 ok)\n";
  } else {
    std::cout << "sent " << path << " (" << size << " bytes)\n";
  }
}

void CliReporter::transfer_complete(std::size_t file_count, std::uint64_t total_bytes) {
  std::cout << "transfer complete: " << file_count << " file(s), " << total_bytes << " bytes\n";
}

}  // namespace kiko
