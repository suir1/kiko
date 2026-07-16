#pragma once

// Cross-platform networking shim. POSIX is the verified/tested target; the
// Windows (winsock) branch is provided for portability.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// iphlpapi must come after winsock2.
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <io.h>
#endif

namespace kiko {

#ifdef _WIN32
inline constexpr int kErrWouldBlock = WSAEWOULDBLOCK;
inline constexpr int kErrInProgress = WSAEWOULDBLOCK;  // non-blocking connect reports this on Windows
inline constexpr int kErrIntr = WSAEINTR;
#else
inline constexpr int kErrWouldBlock = EWOULDBLOCK;
inline constexpr int kErrInProgress = EINPROGRESS;
inline constexpr int kErrIntr = EINTR;
#endif

// Initializes the networking subsystem once per process (WSAStartup on Windows,
// no-op elsewhere).
inline void net_startup() {
#ifdef _WIN32
  static bool initialized = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  (void)initialized;
#endif
}

inline int net_last_error() {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

inline std::string net_error_string(int err) {
#ifdef _WIN32
  char* buffer = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 static_cast<DWORD>(err), 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
  std::string text = buffer ? buffer : "unknown error";
  if (buffer) LocalFree(buffer);
  return text;
#else
  return std::strerror(err);
#endif
}

inline void net_close(int fd) {
#ifdef _WIN32
  closesocket(static_cast<SOCKET>(fd));
#else
  ::close(fd);
#endif
}

// Returns 0 on success, -1 on failure (with net_last_error set).
inline int net_set_nonblocking(int fd, bool nonblocking) {
#ifdef _WIN32
  u_long mode = nonblocking ? 1 : 0;
  return ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == 0 ? 0 : -1;
#else
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  if (nonblocking) {
    flags |= O_NONBLOCK;
  } else {
    flags &= ~O_NONBLOCK;
  }
  return fcntl(fd, F_SETFL, flags) == 0 ? 0 : -1;
#endif
}

// Waits up to timeout_ms for the socket to be readable/writable.
inline int net_poll(int fd, bool want_read, bool want_write, int timeout_ms) {
#ifdef _WIN32
  WSAPOLLFD pfd{};
  pfd.fd = static_cast<SOCKET>(fd);
#else
  pollfd pfd{};
  pfd.fd = fd;
#endif
  pfd.events = static_cast<short>((want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0));
#ifdef _WIN32
  int rc = WSAPoll(&pfd, 1, timeout_ms);
#else
  int rc = ::poll(&pfd, 1, timeout_ms);
#endif
  if (rc <= 0) return rc;
  short wanted = static_cast<short>((want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0));
  const short terminal = static_cast<short>(POLLERR | POLLHUP | POLLNVAL);
  return (pfd.revents & (wanted | terminal)) ? 1 : 0;
}

inline bool stdin_is_tty() {
#ifdef _WIN32
  return _isatty(_fileno(stdin)) != 0;
#else
  return isatty(fileno(stdin)) != 0;
#endif
}

}  // namespace kiko
