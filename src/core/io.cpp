#include "core/io.hpp"

namespace kiko {

asio::io_context& io_context() {
  static asio::io_context ctx;
  return ctx;
}

}  // namespace kiko
