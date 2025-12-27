#pragma once

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <mutex>
#include <cstdint>
#include <queue>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

struct ClientConfig {
  std::string host;
  std::string port;
  std::string username;
  std::string password;
};

namespace net {
class TcpConnection;
}  // namespace net

enum class ClientStatus {
  Disconnected,
  Connecting,
  Connected,
  Error
};

class RmiClient {
 public:
  struct FileEntry {
    std::string name;
    bool is_dir = false;
    uint64_t size = 0;
  };

  RmiClient();
  ~RmiClient();

  bool connect(const ClientConfig& config);
  void disconnect();
  void sendScreencap();
  void sendQuit();
  void sendRestart();
  void sendPress(int keycode);
  void sendVersion();
  void sendPressInput(int keycode);
  void sendUpload(const std::string& local_path, const std::string& remote_path);
  void sendUploadAndRestart(const std::string& local_path, const std::string& remote_path);

  ClientStatus status() const;
  std::string statusLabel() const;
  std::string lastError() const;
  std::string lastScreencapPath() const;
  uint64_t screencapVersion() const;
  bool getScreencapImage(std::vector<uint8_t>* pixels,
                         int* width,
                         int* height,
                         uint64_t* version) const;
  bool getScreencapPng(std::vector<uint8_t>* png, uint64_t* version) const;
  bool saveLastScreencap(std::string* out_path);
  bool getVersionInfo(int64_t* version, std::string* status) const;
  void requestFileList(const std::string& path);
  bool getFileList(const std::string& path,
                   std::vector<FileEntry>* entries,
                   std::string* error,
                   uint64_t* version) const;
  void requestDownload(const std::string& path);
  bool getDownloadResult(const std::string& path,
                         std::vector<uint8_t>* data,
                         std::string* error,
                         uint64_t* version);
  bool getDownloadProgress(const std::string& path,
                           uint64_t* received,
                           uint64_t* total,
                           bool* in_progress) const;
  void requestDelete(const std::string& path);

 private:
  enum class ResponseType {
    None,
    Ok,
    Screencap,
    Version,
    List,
    Download
  };

  struct OutboundMessage {
    std::string message;
    ResponseType response = ResponseType::None;
    bool disconnect_after_ok = false;
    bool is_upload = false;
    bool restart_after_upload = false;
    std::string upload_local_path;
    std::string upload_remote_path;
    std::string list_path;
    std::string download_path;
  };

  void workerLoop(ClientConfig config);
  void queueMessage(const OutboundMessage& message);
  void setStatus(ClientStatus status);
  void setError(const std::string& error);
  void clearError();
  void setLastScreencapPath(const std::string& path);
  void setScreencapData(std::vector<uint8_t> png,
                        std::vector<uint8_t> pixels,
                        int width,
                        int height);
  void setVersionInfo(int64_t version);
  void setVersionStatus(const std::string& status);
  bool parseVersionPayload(const std::vector<uint8_t>& payload, int64_t* version, std::string* error) const;
  bool sendFrame(class net::TcpConnection& connection,
                 const std::string& payload,
                 std::string* error);
  bool sendFrameBytes(class net::TcpConnection& connection,
                      const uint8_t* data,
                      size_t size,
                      std::string* error);
  bool sendHeartbeat(class net::TcpConnection& connection, std::string* error);
  bool loadUploadFile(const std::string& path,
                      std::vector<uint8_t>* data,
                      uint32_t* size,
                      std::string* error) const;
  bool readExact(class net::TcpConnection& connection,
                 uint8_t* buffer,
                 size_t size,
                 int timeout_ms,
                 std::string* error);
  bool readExactWithProgress(class net::TcpConnection& connection,
                             uint8_t* buffer,
                             size_t size,
                             int timeout_ms,
                             const std::string& download_path,
                             size_t* received_total,
                             std::string* error);
  bool receiveFrame(class net::TcpConnection& connection,
                    std::vector<uint8_t>* payload,
                    int timeout_ms,
                    size_t max_bytes,
                    std::string* error);
  bool receiveFrameSkippingHeartbeats(class net::TcpConnection& connection,
                                      std::vector<uint8_t>* payload,
                                      int timeout_ms,
                                      size_t max_bytes,
                                      std::string* error);
  bool receiveFrameSkippingHeartbeatsWithProgress(class net::TcpConnection& connection,
                                                  std::vector<uint8_t>* payload,
                                                  int timeout_ms,
                                                  size_t max_bytes,
                                                  const std::string& download_path,
                                                  std::string* error);
  bool receiveScreencap(class net::TcpConnection& connection);
  bool parseFileListPayload(const std::vector<uint8_t>& payload,
                            std::vector<FileEntry>* entries,
                            std::string* error) const;
  void setDownloadProgress(const std::string& path,
                           uint64_t received,
                           uint64_t total,
                           bool in_progress);
  void joinWorker();

  mutable std::mutex error_mutex_;
  std::string last_error_;

  std::mutex outbox_mutex_;
  std::condition_variable outbox_cv_;
  std::queue<OutboundMessage> outbox_;

  mutable std::mutex screencap_mutex_;
  std::string last_screencap_path_;
  std::vector<uint8_t> last_screencap_png_;
  std::vector<uint8_t> last_screencap_pixels_;
  int last_screencap_width_ = 0;
  int last_screencap_height_ = 0;
  uint64_t last_screencap_version_ = 0;
  uint64_t screencap_counter_ = 0;
  uint32_t client_id_ = 0;

  mutable std::mutex version_mutex_;
  int64_t last_version_ = -1;
  bool has_version_ = false;
  std::string version_status_;

  std::atomic<ClientStatus> status_;
  std::atomic<bool> stop_;
  std::thread worker_;

  mutable std::mutex file_mutex_;
  struct FileListResult {
    std::vector<FileEntry> entries;
    std::string error;
    uint64_t version = 0;
  };
  struct DownloadResult {
    std::vector<uint8_t> data;
    std::string error;
    uint64_t version = 0;
    uint64_t total = 0;
    uint64_t received = 0;
    bool in_progress = false;
  };
  std::unordered_map<std::string, FileListResult> file_lists_;
  std::unordered_map<std::string, DownloadResult> downloads_;
};
