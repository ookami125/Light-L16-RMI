#include "net.h"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>

#ifndef _WIN32
#include <sys/select.h>
#include <sys/time.h>
#endif

namespace {

std::string GetLastErrorString() {
#ifdef _WIN32
  return "WSA error " + std::to_string(WSAGetLastError());
#else
  return std::string(strerror(errno));
#endif
}

bool EnsureInitialized(std::string* error) {
#ifdef _WIN32
  static std::once_flag once;
  static bool ok = false;
  static std::string init_error;
  std::call_once(once, []() {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) == 0) {
      ok = true;
    } else {
      init_error = GetLastErrorString();
    }
  });
  if (!ok && error) {
    *error = init_error.empty() ? "WSAStartup failed" : init_error;
  }
  return ok;
#else
  (void)error;
  return true;
#endif
}

void CloseSocket(net::SocketHandle socket_handle) {
#ifdef _WIN32
  closesocket(socket_handle);
#else
  close(socket_handle);
#endif
}

}  // namespace

namespace net {

TcpConnection::TcpConnection() : socket_(kInvalidSocket) {}

TcpConnection::~TcpConnection() {
  close();
}

bool TcpConnection::connectTo(const std::string& host, const std::string& port, std::string* error) {
  close();

  std::string init_error;
  if (!EnsureInitialized(&init_error)) {
    if (error) {
      *error = init_error;
    }
    return false;
  }

  addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* result = nullptr;
  const int resolve_status = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
  if (resolve_status != 0) {
    if (error) {
#ifdef _WIN32
      *error = "getaddrinfo failed: " + std::string(gai_strerrorA(resolve_status));
#else
      *error = "getaddrinfo failed: " + std::string(gai_strerror(resolve_status));
#endif
    }
    return false;
  }

  for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
    SocketHandle candidate = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
    if (candidate == kInvalidSocket) {
      continue;
    }

    int connect_result = 0;
#ifdef _WIN32
    connect_result = ::connect(candidate, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen));
    if (connect_result == SOCKET_ERROR) {
      CloseSocket(candidate);
      continue;
    }
#else
    connect_result = ::connect(candidate, ptr->ai_addr, ptr->ai_addrlen);
    if (connect_result != 0) {
      CloseSocket(candidate);
      continue;
    }
#endif

    socket_ = candidate;
    freeaddrinfo(result);
    return true;
  }

  freeaddrinfo(result);
  if (error) {
    *error = "Unable to connect: " + GetLastErrorString();
  }
  return false;
}

bool TcpConnection::sendAll(const std::string& message, std::string* error) {
  if (!isConnected()) {
    if (error) {
      *error = "Socket not connected";
    }
    return false;
  }

  const char* buffer = message.data();
  size_t remaining = message.size();

  while (remaining > 0) {
#ifdef _WIN32
    const int sent = ::send(socket_, buffer, static_cast<int>(remaining), 0);
#else
    const ssize_t sent = ::send(socket_, buffer, remaining, 0);
#endif
    if (sent <= 0) {
      if (error) {
        *error = "Send failed: " + GetLastErrorString();
      }
      return false;
    }
    buffer += sent;
    remaining -= static_cast<size_t>(sent);
  }

  return true;
}

TcpConnection::ReceiveStatus TcpConnection::receive(void* buffer,
                                                     size_t max_bytes,
                                                     size_t* bytes_received,
                                                     int timeout_ms,
                                                     std::string* error) {
  if (!isConnected()) {
    if (error) {
      *error = "Socket not connected";
    }
    return ReceiveStatus::Error;
  }

  if (bytes_received) {
    *bytes_received = 0;
  }

  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(socket_, &read_set);

  timeval timeout;
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;

#ifdef _WIN32
  const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
#else
  const int ready = select(socket_ + 1, &read_set, nullptr, nullptr, &timeout);
#endif
  if (ready == 0) {
    return ReceiveStatus::Timeout;
  }
  if (ready < 0) {
    if (error) {
      *error = "select failed: " + GetLastErrorString();
    }
    return ReceiveStatus::Error;
  }

#ifdef _WIN32
  const int received = ::recv(socket_, static_cast<char*>(buffer), static_cast<int>(max_bytes), 0);
  if (received == 0) {
    return ReceiveStatus::Closed;
  }
  if (received == SOCKET_ERROR) {
    if (error) {
      *error = "Receive failed: " + GetLastErrorString();
    }
    return ReceiveStatus::Error;
  }
#else
  const ssize_t received = ::recv(socket_, buffer, max_bytes, 0);
  if (received == 0) {
    return ReceiveStatus::Closed;
  }
  if (received < 0) {
    if (error) {
      *error = "Receive failed: " + GetLastErrorString();
    }
    return ReceiveStatus::Error;
  }
#endif

  if (bytes_received) {
    *bytes_received = static_cast<size_t>(received);
  }
  return ReceiveStatus::Ok;
}

void TcpConnection::close() {
  if (socket_ == kInvalidSocket) {
    return;
  }
  CloseSocket(socket_);
  socket_ = kInvalidSocket;
}

bool TcpConnection::isConnected() const {
  return socket_ != kInvalidSocket;
}

}  // namespace net
