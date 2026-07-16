#pragma once

#include <asio/io_context.hpp>

namespace kiko {

inline asio::io_context& io_context() {
  static asio::io_context context;
  return context;
}

}  // namespace kiko
