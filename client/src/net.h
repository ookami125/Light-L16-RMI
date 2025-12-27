#pragma once

#include <cstddef>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
namespace net {
using SocketHandle = SOCKET;
const SocketHandle kInvalidSocket = INVALID_SOCKET;
}
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
namespace net {
using SocketHandle = int;
const SocketHandle kInvalidSocket = -1;
}
#endif

namespace net {

class TcpConnection {
 public:
  TcpConnection();
  ~TcpConnection();

  bool connectTo(const std::string& host, const std::string& port, std::string* error);
  bool sendAll(const std::string& message, std::string* error);
  enum class ReceiveStatus {
    Ok,
    Timeout,
    Closed,
    Error
  };
  ReceiveStatus receive(void* buffer, size_t max_bytes, size_t* bytes_received, int timeout_ms,
                        std::string* error);
  void close();
  bool isConnected() const;

 private:
  SocketHandle socket_;
};

}  // namespace net
