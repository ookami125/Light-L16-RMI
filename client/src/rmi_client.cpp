#include "rmi_client.h"

#include "net.h"
#include "rmi_protocol.h"
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace {

constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
constexpr size_t kMaxFrameBytes = 0;
constexpr uint64_t kMaxScreencapPixels = 4096ull * 4096ull;
constexpr size_t kMaxUploadBytes = std::numeric_limits<uint32_t>::max();
constexpr int kAuthTimeoutMs = 5000;
constexpr int kVersionTimeoutMs = 3000;
constexpr int kScreencapTimeoutMs = 15000;
constexpr int kReadStepTimeoutMs = 1000;
constexpr int kHeartbeatIntervalMs = 5000;
constexpr int kHeartbeatTimeoutMs = 2000;

uint32_t ReadBe32(const uint8_t* data) {
  return rmi_read_be32(data);
}

bool PayloadEquals(const std::vector<uint8_t>& payload, const char* text) {
  return rmi_payload_equals(payload.data(), payload.size(), text) != 0;
}

bool PayloadStartsWith(const std::vector<uint8_t>& payload, const char* text) {
  return rmi_payload_starts_with(payload.data(), payload.size(), text) != 0;
}

std::string PayloadToString(const std::vector<uint8_t>& payload) {
  return std::string(payload.begin(), payload.end());
}

bool ContainsWhitespace(const std::string& value) {
  for (char c : value) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      return true;
    }
  }
  return false;
}

}  // namespace

RmiClient::RmiClient() : status_(ClientStatus::Disconnected), stop_(false) {
  static std::atomic<uint32_t> next_id{1};
  client_id_ = next_id.fetch_add(1);
}

RmiClient::~RmiClient() {
  disconnect();
}

bool RmiClient::connect(const ClientConfig& config) {
  const ClientStatus current = status_.load();
  if (current == ClientStatus::Connecting || current == ClientStatus::Connected) {
    return false;
  }

  joinWorker();
  clearError();
  stop_ = false;
  setStatus(ClientStatus::Connecting);
  worker_ = std::thread(&RmiClient::workerLoop, this, config);
  return true;
}

void RmiClient::disconnect() {
  stop_ = true;
  outbox_cv_.notify_all();
  joinWorker();
  if (status_.load() != ClientStatus::Error) {
    setStatus(ClientStatus::Disconnected);
  }
}

void RmiClient::sendScreencap() {
  if (status_.load() != ClientStatus::Connected) {
    return;
  }
  OutboundMessage message;
  message.message = RMI_CMD_SCREENCAP;
  message.response = ResponseType::Screencap;
  queueMessage(message);
}

void RmiClient::sendQuit() {
  if (status_.load() != ClientStatus::Connected) {
    return;
  }
  OutboundMessage message;
  message.message = RMI_CMD_QUIT;
  message.response = ResponseType::Ok;
  message.disconnect_after_ok = true;
  queueMessage(message);
}

void RmiClient::sendRestart() {
  if (status_.load() != ClientStatus::Connected) {
    return;
  }
  OutboundMessage message;
  message.message = RMI_CMD_RESTART;
  message.response = ResponseType::Ok;
  message.disconnect_after_ok = true;
  queueMessage(message);
}

void RmiClient::sendPress(int keycode) {
  if (status_.load() != ClientStatus::Connected) {
    return;
  }
  OutboundMessage message;
  message.message = std::string(RMI_CMD_PRESS) + " " + std::to_string(keycode);
  message.response = ResponseType::Ok;
  queueMessage(message);
}

void RmiClient::sendPressInput(int keycode) {
  if (status_.load() != ClientStatus::Connected) {
    return;
  }
  OutboundMessage message;
  message.message = std::string(RMI_CMD_PRESS_INPUT) + " " + std::to_string(keycode);
  message.response = ResponseType::Ok;
  queueMessage(message);
}

void RmiClient::sendUpload(const std::string& local_path, const std::string& remote_path) {
  if (status_.load() != ClientStatus::Connected) {
    return;
  }
  OutboundMessage message;
  message.is_upload = true;
  message.upload_local_path = local_path;
  message.upload_remote_path = remote_path;
  queueMessage(message);
}

void RmiClient::sendUploadAndRestart(const std::string& local_path, const std::string& remote_path) {
  if (status_.load() != ClientStatus::Connected) {
    return;
  }
  OutboundMessage message;
  message.is_upload = true;
  message.restart_after_upload = true;
  message.upload_local_path = local_path;
  message.upload_remote_path = remote_path;
  queueMessage(message);
}

void RmiClient::sendVersion() {
  if (status_.load() != ClientStatus::Connected) {
    return;
  }
  OutboundMessage message;
  message.message = RMI_CMD_VERSION;
  message.response = ResponseType::Version;
  queueMessage(message);
}

void RmiClient::requestFileList(const std::string& path) {
  if (status_.load() != ClientStatus::Connected) {
    return;
  }
  if (path.empty()) {
    setError("File list path is empty.");
    return;
  }
  if (ContainsWhitespace(path)) {
    setError("File list path must not contain whitespace.");
    return;
  }
  OutboundMessage message;
  message.message = std::string(RMI_CMD_LIST) + " " + path;
  message.response = ResponseType::List;
  message.list_path = path;
  queueMessage(message);
}

bool RmiClient::getFileList(const std::string& path,
                            std::vector<FileEntry>* entries,
                            std::string* error,
                            uint64_t* version) const {
  std::lock_guard<std::mutex> lock(file_mutex_);
  auto it = file_lists_.find(path);
  if (it == file_lists_.end()) {
    return false;
  }
  if (entries) {
    *entries = it->second.entries;
  }
  if (error) {
    *error = it->second.error;
  }
  if (version) {
    *version = it->second.version;
  }
  return true;
}

void RmiClient::requestDownload(const std::string& path) {
  if (status_.load() != ClientStatus::Connected) {
    return;
  }
  if (path.empty()) {
    setError("Download path is empty.");
    return;
  }
  if (ContainsWhitespace(path)) {
    setError("Download path must not contain whitespace.");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(file_mutex_);
    DownloadResult& result = downloads_[path];
    result.data.clear();
    result.error.clear();
    result.total = 0;
    result.received = 0;
    result.in_progress = true;
  }
  OutboundMessage message;
  message.message = std::string(RMI_CMD_DOWNLOAD) + " " + path;
  message.response = ResponseType::Download;
  message.download_path = path;
  queueMessage(message);
}

bool RmiClient::getDownloadResult(const std::string& path,
                                  std::vector<uint8_t>* data,
                                  std::string* error,
                                  uint64_t* version) {
  std::lock_guard<std::mutex> lock(file_mutex_);
  auto it = downloads_.find(path);
  if (it == downloads_.end()) {
    return false;
  }
  if (data) {
    *data = std::move(it->second.data);
    it->second.data.clear();
  }
  if (error) {
    *error = it->second.error;
  }
  if (version) {
    *version = it->second.version;
  }
  return true;
}

bool RmiClient::getDownloadProgress(const std::string& path,
                                    uint64_t* received,
                                    uint64_t* total,
                                    bool* in_progress) const {
  std::lock_guard<std::mutex> lock(file_mutex_);
  auto it = downloads_.find(path);
  if (it == downloads_.end()) {
    return false;
  }
  if (received) {
    *received = it->second.received;
  }
  if (total) {
    *total = it->second.total;
  }
  if (in_progress) {
    *in_progress = it->second.in_progress;
  }
  return true;
}

ClientStatus RmiClient::status() const {
  return status_.load();
}

std::string RmiClient::statusLabel() const {
  switch (status_.load()) {
    case ClientStatus::Disconnected:
      return "Disconnected";
    case ClientStatus::Connecting:
      return "Connecting";
    case ClientStatus::Connected:
      return "Connected";
    case ClientStatus::Error:
      return "Error";
  }
  return "Unknown";
}

std::string RmiClient::lastError() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return last_error_;
}

std::string RmiClient::lastScreencapPath() const {
  std::lock_guard<std::mutex> lock(screencap_mutex_);
  return last_screencap_path_;
}

bool RmiClient::saveLastScreencap(std::string* out_path) {
  std::vector<uint8_t> png;
  uint64_t capture_index = 0;
  uint32_t client_id = 0;
  bool has_data = false;
  {
    std::lock_guard<std::mutex> lock(screencap_mutex_);
    if (!last_screencap_png_.empty()) {
      has_data = true;
      png = last_screencap_png_;
      capture_index = ++screencap_counter_;
      client_id = client_id_;
    }
  }
  if (!has_data) {
    setError("No screencap data to save.");
    return false;
  }

  std::error_code fs_error;
  std::filesystem::path capture_dir = std::filesystem::current_path() / "captures";
  std::filesystem::create_directories(capture_dir, fs_error);
  if (fs_error) {
    setError("Failed to create captures directory: " + fs_error.message());
    return false;
  }

  const std::string filename =
      "screencap_client" + std::to_string(client_id) + "_" + std::to_string(capture_index) + ".png";
  std::filesystem::path file_path = capture_dir / filename;
  std::filesystem::path absolute_path = std::filesystem::absolute(file_path, fs_error);
  if (fs_error) {
    absolute_path = file_path;
  }

  std::ofstream out(file_path, std::ios::binary);
  if (!out) {
    setError("Failed to open screencap file for writing.");
    return false;
  }
  out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  if (!out.good()) {
    setError("Failed to write screencap file.");
    return false;
  }

  setLastScreencapPath(absolute_path.string());
  clearError();
  if (out_path) {
    *out_path = absolute_path.string();
  }
  return true;
}

uint64_t RmiClient::screencapVersion() const {
  std::lock_guard<std::mutex> lock(screencap_mutex_);
  return last_screencap_version_;
}

bool RmiClient::getScreencapImage(std::vector<uint8_t>* pixels,
                                  int* width,
                                  int* height,
                                  uint64_t* version) const {
  std::lock_guard<std::mutex> lock(screencap_mutex_);
  if (last_screencap_pixels_.empty() || last_screencap_width_ <= 0 || last_screencap_height_ <= 0) {
    return false;
  }
  if (pixels) {
    *pixels = last_screencap_pixels_;
  }
  if (width) {
    *width = last_screencap_width_;
  }
  if (height) {
    *height = last_screencap_height_;
  }
  if (version) {
    *version = last_screencap_version_;
  }
  return true;
}

bool RmiClient::getScreencapPng(std::vector<uint8_t>* png, uint64_t* version) const {
  std::lock_guard<std::mutex> lock(screencap_mutex_);
  if (last_screencap_png_.empty()) {
    return false;
  }
  if (png) {
    *png = last_screencap_png_;
  }
  if (version) {
    *version = last_screencap_version_;
  }
  return true;
}

bool RmiClient::getVersionInfo(int64_t* version, std::string* status) const {
  std::lock_guard<std::mutex> lock(version_mutex_);
  if (!has_version_) {
    if (status) {
      *status = version_status_;
    }
    return false;
  }
  if (version) {
    *version = last_version_;
  }
  if (status) {
    *status = version_status_;
  }
  return true;
}

void RmiClient::requestDelete(const std::string& path) {
  if (status_.load() != ClientStatus::Connected) {
    return;
  }
  if (path.empty()) {
    setError("Delete path is empty.");
    return;
  }
  if (ContainsWhitespace(path)) {
    setError("Delete path must not contain whitespace.");
    return;
  }
  OutboundMessage message;
  message.message = std::string(RMI_CMD_DELETE) + " " + path;
  message.response = ResponseType::Ok;
  queueMessage(message);
}

void RmiClient::workerLoop(ClientConfig config) {
  net::TcpConnection connection;
  std::string error;

  if (!connection.connectTo(config.host, config.port, &error)) {
    setError(error);
    setStatus(ClientStatus::Error);
    return;
  }

  const std::string login_message =
      std::string(RMI_CMD_AUTH) + " " + config.username + " " + config.password;
  if (!sendFrame(connection, login_message, &error)) {
    setError(error);
    setStatus(ClientStatus::Error);
    return;
  }
  std::vector<uint8_t> auth_response;
  if (!receiveFrameSkippingHeartbeats(connection,
                                      &auth_response,
                                      kAuthTimeoutMs,
                                      256,
                                      &error)) {
    setError(error);
    setStatus(ClientStatus::Error);
    return;
  }
  if (!PayloadEquals(auth_response, RMI_RESP_OK)) {
    if (PayloadStartsWith(auth_response, RMI_RESP_ERR_PREFIX)) {
      setError(PayloadToString(auth_response));
    } else {
      setError("Unexpected auth response: " + PayloadToString(auth_response));
    }
    setStatus(ClientStatus::Error);
    return;
  }

  setStatus(ClientStatus::Connected);

  auto last_heartbeat = std::chrono::steady_clock::now();

  while (!stop_) {
    OutboundMessage message;
    bool has_message = false;
    {
      std::unique_lock<std::mutex> lock(outbox_mutex_);
      outbox_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
        return stop_.load() || !outbox_.empty();
      });
      if (stop_) {
        break;
      }
      if (!outbox_.empty()) {
        message = std::move(outbox_.front());
        outbox_.pop();
        has_message = true;
      }
    }

    if (has_message && !message.message.empty()) {
      if (!sendFrame(connection, message.message, &error)) {
        setError(error);
        setStatus(ClientStatus::Error);
        return;
      }
      last_heartbeat = std::chrono::steady_clock::now();
      if (message.response == ResponseType::Screencap) {
        if (!receiveScreencap(connection)) {
          setStatus(ClientStatus::Error);
          return;
        }
      } else if (message.response == ResponseType::Ok) {
        std::vector<uint8_t> response;
        if (!receiveFrameSkippingHeartbeats(connection,
                                            &response,
                                            kAuthTimeoutMs,
                                            256,
                                            &error)) {
          setError(error);
          setStatus(ClientStatus::Error);
          return;
        }
        if (PayloadEquals(response, RMI_RESP_OK)) {
          if (message.disconnect_after_ok) {
            setStatus(ClientStatus::Disconnected);
            stop_ = true;
            break;
          }
        } else if (PayloadStartsWith(response, RMI_RESP_ERR_PREFIX)) {
          setError(PayloadToString(response));
        } else {
          setError("Unexpected response: " + PayloadToString(response));
        }
      } else if (message.response == ResponseType::Version) {
        std::vector<uint8_t> response;
        if (!receiveFrameSkippingHeartbeats(connection,
                                            &response,
                                            kVersionTimeoutMs,
                                            256,
                                            &error)) {
          setError(error);
          continue;
        }
        int64_t version = -1;
        std::string parse_error;
        if (!parseVersionPayload(response, &version, &parse_error)) {
          setError(parse_error);
          continue;
        }
        setVersionInfo(version);
      } else if (message.response == ResponseType::List) {
        std::vector<uint8_t> response;
        if (!receiveFrameSkippingHeartbeats(connection,
                                            &response,
                                            kAuthTimeoutMs,
                                            kMaxFrameBytes,
                                            &error)) {
          setError(error);
          setStatus(ClientStatus::Error);
          return;
        }
        std::vector<FileEntry> entries;
        std::string list_error;
        if (!parseFileListPayload(response, &entries, &list_error)) {
          std::lock_guard<std::mutex> lock(file_mutex_);
          FileListResult& result = file_lists_[message.list_path];
          result.entries.clear();
          result.error = list_error.empty() ? "Failed to parse file list." : list_error;
          ++result.version;
        } else {
          std::lock_guard<std::mutex> lock(file_mutex_);
          FileListResult& result = file_lists_[message.list_path];
          result.entries = std::move(entries);
          result.error.clear();
          ++result.version;
        }
      } else if (message.response == ResponseType::Download) {
        std::vector<uint8_t> response;
        if (!receiveFrameSkippingHeartbeats(connection,
                                            &response,
                                            kAuthTimeoutMs,
                                            256,
                                            &error)) {
          setError(error);
          setStatus(ClientStatus::Error);
          return;
        }
        if (PayloadEquals(response, RMI_RESP_OK)) {
          std::vector<uint8_t> file_data;
          if (!receiveFrameSkippingHeartbeatsWithProgress(connection,
                                                          &file_data,
                                                          kScreencapTimeoutMs,
                                                          kMaxFrameBytes,
                                                          message.download_path,
                                                          &error)) {
            setError(error);
            setStatus(ClientStatus::Error);
            return;
          }
          std::lock_guard<std::mutex> lock(file_mutex_);
          DownloadResult& result = downloads_[message.download_path];
          result.data = std::move(file_data);
          result.error.clear();
          result.total = result.data.size();
          result.received = result.total;
          result.in_progress = false;
          ++result.version;
        } else if (PayloadStartsWith(response, RMI_RESP_ERR_PREFIX)) {
          std::lock_guard<std::mutex> lock(file_mutex_);
          DownloadResult& result = downloads_[message.download_path];
          result.data.clear();
          result.error = PayloadToString(response);
          result.total = 0;
          result.received = 0;
          result.in_progress = false;
          ++result.version;
        } else {
          std::lock_guard<std::mutex> lock(file_mutex_);
          DownloadResult& result = downloads_[message.download_path];
          result.data.clear();
          result.error = "Unexpected response: " + PayloadToString(response);
          result.total = 0;
          result.received = 0;
          result.in_progress = false;
          ++result.version;
        }
      }
    }

    if (has_message && message.is_upload) {
      if (message.upload_local_path.empty() || message.upload_remote_path.empty()) {
        setError("Upload requires local and remote paths.");
        continue;
      }
      if (ContainsWhitespace(message.upload_remote_path)) {
        setError("Upload remote path must not contain whitespace.");
        continue;
      }
      std::vector<uint8_t> file_data;
      uint32_t size = 0;
      if (!loadUploadFile(message.upload_local_path, &file_data, &size, &error)) {
        setError(error);
        continue;
      }
      const std::string command = std::string(RMI_CMD_UPLOAD) + " " +
                                  message.upload_remote_path + " " +
                                  std::to_string(size);
      if (!sendFrame(connection, command, &error)) {
        setError(error);
        setStatus(ClientStatus::Error);
        return;
      }
      if (!sendFrameBytes(connection, file_data.data(), file_data.size(), &error)) {
        setError(error);
        setStatus(ClientStatus::Error);
        return;
      }
      std::vector<uint8_t> response;
      if (!receiveFrameSkippingHeartbeats(connection,
                                          &response,
                                          kAuthTimeoutMs,
                                          256,
                                          &error)) {
        setError(error);
        setStatus(ClientStatus::Error);
        return;
      }
      if (!PayloadEquals(response, RMI_RESP_OK)) {
        if (PayloadStartsWith(response, RMI_RESP_ERR_PREFIX)) {
          setError(PayloadToString(response));
        } else {
          setError("Unexpected response: " + PayloadToString(response));
        }
        continue;
      }
      if (message.restart_after_upload) {
        if (!sendFrame(connection, RMI_CMD_RESTART, &error)) {
          setError(error);
          setStatus(ClientStatus::Error);
          return;
        }
        std::vector<uint8_t> restart_response;
        if (!receiveFrameSkippingHeartbeats(connection,
                                            &restart_response,
                                            kAuthTimeoutMs,
                                            256,
                                            &error)) {
          setError(error);
          setStatus(ClientStatus::Error);
          return;
        }
        if (PayloadEquals(restart_response, RMI_RESP_OK)) {
          setStatus(ClientStatus::Disconnected);
          stop_ = true;
          break;
        }
        if (PayloadStartsWith(restart_response, RMI_RESP_ERR_PREFIX)) {
          setError(PayloadToString(restart_response));
        } else {
          setError("Unexpected response: " + PayloadToString(restart_response));
        }
        continue;
      }
      last_heartbeat = std::chrono::steady_clock::now();
    }

    if (!has_message) {
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - last_heartbeat).count();
      if (elapsed_ms >= kHeartbeatIntervalMs) {
        if (!sendHeartbeat(connection, &error)) {
          setError(error);
          setStatus(ClientStatus::Error);
          return;
        }
        last_heartbeat = std::chrono::steady_clock::now();
      }
    }
  }

  connection.close();
  if (status_.load() != ClientStatus::Error) {
    setStatus(ClientStatus::Disconnected);
  }
}

void RmiClient::queueMessage(const OutboundMessage& message) {
  {
    std::lock_guard<std::mutex> lock(outbox_mutex_);
    outbox_.push(message);
  }
  outbox_cv_.notify_one();
}

void RmiClient::setStatus(ClientStatus status) {
  status_.store(status);
}

void RmiClient::setError(const std::string& error) {
  std::lock_guard<std::mutex> lock(error_mutex_);
  last_error_ = error;
}

void RmiClient::clearError() {
  std::lock_guard<std::mutex> lock(error_mutex_);
  last_error_.clear();
}

void RmiClient::setLastScreencapPath(const std::string& path) {
  std::lock_guard<std::mutex> lock(screencap_mutex_);
  last_screencap_path_ = path;
}

void RmiClient::setScreencapData(std::vector<uint8_t> png,
                                 std::vector<uint8_t> pixels,
                                 int width,
                                 int height) {
  std::lock_guard<std::mutex> lock(screencap_mutex_);
  last_screencap_png_ = std::move(png);
  last_screencap_pixels_ = std::move(pixels);
  last_screencap_width_ = width;
  last_screencap_height_ = height;
  last_screencap_path_.clear();
  ++last_screencap_version_;
}

void RmiClient::setVersionInfo(int64_t version) {
  std::lock_guard<std::mutex> lock(version_mutex_);
  last_version_ = version;
  has_version_ = true;
}

void RmiClient::setVersionStatus(const std::string& status) {
  std::lock_guard<std::mutex> lock(version_mutex_);
  version_status_ = status;
}

void RmiClient::setDownloadProgress(const std::string& path,
                                    uint64_t received,
                                    uint64_t total,
                                    bool in_progress) {
  if (path.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(file_mutex_);
  DownloadResult& result = downloads_[path];
  result.received = received;
  result.total = total;
  result.in_progress = in_progress;
}

bool RmiClient::parseVersionPayload(const std::vector<uint8_t>& payload,
                                    int64_t* version,
                                    std::string* error) const {
  if (PayloadStartsWith(payload, RMI_RESP_ERR_PREFIX)) {
    if (error) {
      *error = PayloadToString(payload);
    }
    return false;
  }
  const std::string text = PayloadToString(payload);
  const std::string prefix = RMI_RESP_VERSION_PREFIX;
  if (text.size() <= prefix.size() || text.compare(0, prefix.size(), prefix) != 0) {
    if (error) {
      *error = "Unexpected VERSION response: " + text;
    }
    return false;
  }
  const std::string number_text = text.substr(prefix.size());
  try {
    size_t idx = 0;
    long long parsed = std::stoll(number_text, &idx, 10);
    if (idx != number_text.size() || parsed < 0) {
      if (error) {
        *error = "Invalid version number: " + number_text;
      }
      return false;
    }
    if (version) {
      *version = parsed;
    }
    return true;
  } catch (const std::exception&) {
    if (error) {
      *error = "Invalid version number: " + number_text;
    }
    return false;
  }
}

bool RmiClient::parseFileListPayload(const std::vector<uint8_t>& payload,
                                     std::vector<FileEntry>* entries,
                                     std::string* error) const {
  if (PayloadStartsWith(payload, RMI_RESP_ERR_PREFIX)) {
    if (error) {
      *error = PayloadToString(payload);
    }
    return false;
  }
  if (!entries) {
    return false;
  }
  entries->clear();
  const std::string text = PayloadToString(payload);
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty()) {
      continue;
    }
    if (line.size() < 3 || line[1] != '\t') {
      if (error) {
        *error = "Malformed list entry.";
      }
      return false;
    }
    FileEntry entry;
    if (line[0] == 'D') {
      entry.is_dir = true;
      entry.name = line.substr(2);
    } else if (line[0] == 'F') {
      const size_t tab = line.find('\t', 2);
      if (tab == std::string::npos) {
        if (error) {
          *error = "Malformed file entry.";
        }
        return false;
      }
      entry.is_dir = false;
      entry.name = line.substr(2, tab - 2);
      const std::string size_text = line.substr(tab + 1);
      try {
        entry.size = std::stoull(size_text);
      } catch (const std::exception&) {
        if (error) {
          *error = "Invalid file size.";
        }
        return false;
      }
    } else {
      if (error) {
        *error = "Unknown list entry type.";
      }
      return false;
    }
    if (entry.name.empty()) {
      continue;
    }
    entries->push_back(std::move(entry));
  }
  return true;
}

bool RmiClient::sendFrame(net::TcpConnection& connection,
                          const std::string& payload,
                          std::string* error) {
  if (payload.size() > std::numeric_limits<uint32_t>::max()) {
    if (error) {
      *error = "Payload too large to send.";
    }
    return false;
  }
  const uint32_t length = static_cast<uint32_t>(payload.size());
  std::string framed;
  framed.resize(RMI_FRAME_HEADER_SIZE);
  rmi_write_be32(reinterpret_cast<uint8_t*>(&framed[0]), length);
  framed += payload;
  return connection.sendAll(framed, error);
}

bool RmiClient::sendFrameBytes(net::TcpConnection& connection,
                               const uint8_t* data,
                               size_t size,
                               std::string* error) {
  if (size > std::numeric_limits<uint32_t>::max()) {
    if (error) {
      *error = "Payload too large to send.";
    }
    return false;
  }
  std::string header;
  header.resize(RMI_FRAME_HEADER_SIZE);
  const uint32_t length = static_cast<uint32_t>(size);
  rmi_write_be32(reinterpret_cast<uint8_t*>(&header[0]), length);
  if (!connection.sendAll(header, error)) {
    return false;
  }
  if (size == 0) {
    return true;
  }
  std::string payload(reinterpret_cast<const char*>(data), size);
  return connection.sendAll(payload, error);
}

bool RmiClient::loadUploadFile(const std::string& path,
                               std::vector<uint8_t>* data,
                               uint32_t* size,
                               std::string* error) const {
  if (!data) {
    if (error) {
      *error = "Upload buffer missing.";
    }
    return false;
  }
  std::error_code fs_error;
  const std::filesystem::path file_path(path);
  if (!std::filesystem::exists(file_path, fs_error)) {
    if (error) {
      *error = "Upload file not found.";
    }
    return false;
  }
  const auto file_size = std::filesystem::file_size(file_path, fs_error);
  if (fs_error) {
    if (error) {
      *error = "Unable to determine upload file size.";
    }
    return false;
  }
  if (file_size > kMaxUploadBytes) {
    if (error) {
      *error = "Upload file exceeds size limit.";
    }
    return false;
  }
  if (file_size > std::numeric_limits<uint32_t>::max()) {
    if (error) {
      *error = "Upload file too large.";
    }
    return false;
  }
  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    if (error) {
      *error = "Unable to open upload file.";
    }
    return false;
  }
  data->resize(static_cast<size_t>(file_size));
  if (file_size > 0) {
    in.read(reinterpret_cast<char*>(data->data()), static_cast<std::streamsize>(file_size));
    if (!in.good()) {
      if (error) {
        *error = "Failed to read upload file.";
      }
      return false;
    }
  }
  if (size) {
    *size = static_cast<uint32_t>(file_size);
  }
  return true;
}
bool RmiClient::readExact(net::TcpConnection& connection,
                          uint8_t* buffer,
                          size_t size,
                          int timeout_ms,
                          std::string* error) {
  size_t offset = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  while (offset < size && !stop_) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      if (error) {
        *error = "Timed out waiting for server response.";
      }
      return false;
    }
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int step_timeout = static_cast<int>(std::min<long long>(remaining_ms, kReadStepTimeoutMs));
    size_t received = 0;
    const auto status =
        connection.receive(buffer + offset, size - offset, &received, step_timeout, error);
    if (status == net::TcpConnection::ReceiveStatus::Timeout) {
      continue;
    }
    if (status == net::TcpConnection::ReceiveStatus::Closed) {
      if (error) {
        *error = "Connection closed by server.";
      }
      return false;
    }
    if (status == net::TcpConnection::ReceiveStatus::Error) {
      return false;
    }
    if (received == 0) {
      continue;
    }
    offset += received;
  }

  if (stop_) {
    if (error) {
      *error = "Operation cancelled.";
    }
    return false;
  }
  return true;
}

bool RmiClient::readExactWithProgress(net::TcpConnection& connection,
                                      uint8_t* buffer,
                                      size_t size,
                                      int timeout_ms,
                                      const std::string& download_path,
                                      size_t* received_total,
                                      std::string* error) {
  size_t offset = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  while (offset < size && !stop_) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      if (error) {
        *error = "Timed out waiting for server response.";
      }
      break;
    }
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int step_timeout = static_cast<int>(std::min<long long>(remaining_ms, kReadStepTimeoutMs));
    size_t received = 0;
    const auto status =
        connection.receive(buffer + offset, size - offset, &received, step_timeout, error);
    if (status == net::TcpConnection::ReceiveStatus::Timeout) {
      continue;
    }
    if (status == net::TcpConnection::ReceiveStatus::Closed) {
      if (error) {
        *error = "Connection closed by server.";
      }
      break;
    }
    if (status == net::TcpConnection::ReceiveStatus::Error) {
      break;
    }
    if (received == 0) {
      continue;
    }
    offset += received;
    setDownloadProgress(download_path, offset, size, true);
  }

  if (received_total) {
    *received_total = offset;
  }
  if (stop_) {
    if (error) {
      *error = "Operation cancelled.";
    }
    return false;
  }
  return offset == size;
}

bool RmiClient::receiveFrame(net::TcpConnection& connection,
                             std::vector<uint8_t>* payload,
                             int timeout_ms,
                             size_t max_bytes,
                             std::string* error) {
  const auto start = std::chrono::steady_clock::now();
  uint8_t length_bytes[4] = {};
  if (!readExact(connection, length_bytes, sizeof(length_bytes), timeout_ms, error)) {
    return false;
  }
  const uint32_t length = ReadBe32(length_bytes);
  if (max_bytes > 0 && length > max_bytes) {
    if (error) {
      *error = "Frame size exceeds limit.";
    }
    return false;
  }
  payload->assign(length, 0);
  if (length == 0) {
    return true;
  }
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
  const int remaining_ms = timeout_ms - static_cast<int>(elapsed_ms);
  if (remaining_ms <= 0) {
    if (error) {
      *error = "Timed out waiting for server response.";
    }
    return false;
  }
  return readExact(connection, payload->data(), payload->size(), remaining_ms, error);
}

bool RmiClient::receiveFrameSkippingHeartbeats(net::TcpConnection& connection,
                                               std::vector<uint8_t>* payload,
                                               int timeout_ms,
                                               size_t max_bytes,
                                               std::string* error) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!stop_) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      if (error) {
        *error = "Timed out waiting for server response.";
      }
      return false;
    }
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int remaining_timeout = static_cast<int>(remaining_ms);
    if (!receiveFrame(connection, payload, remaining_timeout, max_bytes, error)) {
      return false;
    }
    if (PayloadEquals(*payload, RMI_CMD_HEARTBEAT)) {
      continue;
    }
    return true;
  }
  if (error) {
    *error = "Operation cancelled.";
  }
  return false;
}

bool RmiClient::receiveFrameSkippingHeartbeatsWithProgress(net::TcpConnection& connection,
                                                           std::vector<uint8_t>* payload,
                                                           int timeout_ms,
                                                           size_t max_bytes,
                                                           const std::string& download_path,
                                                           std::string* error) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  const size_t heartbeat_len = std::strlen(RMI_CMD_HEARTBEAT);
  while (!stop_) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      if (error) {
        *error = "Timed out waiting for server response.";
      }
      return false;
    }
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int remaining_timeout = static_cast<int>(remaining_ms);
    uint8_t length_bytes[4] = {};
    if (!readExact(connection, length_bytes, sizeof(length_bytes), remaining_timeout, error)) {
      setDownloadProgress(download_path, 0, 0, false);
      return false;
    }
    const uint32_t length = ReadBe32(length_bytes);
    if (max_bytes > 0 && length > max_bytes) {
      if (error) {
        *error = "Frame size exceeds limit.";
      }
      setDownloadProgress(download_path, 0, 0, false);
      return false;
    }
    payload->assign(length, 0);
    if (length == 0) {
      setDownloadProgress(download_path, 0, 0, false);
      return true;
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - now).count();
    const int payload_timeout = remaining_timeout - static_cast<int>(elapsed_ms);
    if (payload_timeout <= 0) {
      if (error) {
        *error = "Timed out waiting for server response.";
      }
      setDownloadProgress(download_path, 0, 0, false);
      return false;
    }
    if (length == heartbeat_len) {
      if (!readExact(connection, payload->data(), payload->size(), payload_timeout, error)) {
        setDownloadProgress(download_path, 0, 0, false);
        return false;
      }
      if (PayloadEquals(*payload, RMI_CMD_HEARTBEAT)) {
        continue;
      }
      setDownloadProgress(download_path, length, length, false);
      return true;
    }
    size_t received_total = 0;
    setDownloadProgress(download_path, 0, length, true);
    if (!readExactWithProgress(connection,
                               payload->data(),
                               payload->size(),
                               payload_timeout,
                               download_path,
                               &received_total,
                               error)) {
      setDownloadProgress(download_path, received_total, length, false);
      return false;
    }
    setDownloadProgress(download_path, length, length, false);
    return true;
  }
  if (error) {
    *error = "Operation cancelled.";
  }
  return false;
}

bool RmiClient::sendHeartbeat(net::TcpConnection& connection, std::string* error) {
  if (!sendFrame(connection, RMI_CMD_HEARTBEAT, error)) {
    return false;
  }
  std::vector<uint8_t> response;
  if (!receiveFrameSkippingHeartbeats(connection,
                                      &response,
                                      kHeartbeatTimeoutMs,
                                      256,
                                      error)) {
    return false;
  }
  if (PayloadEquals(response, RMI_RESP_OK)) {
    return true;
  }
  if (PayloadStartsWith(response, RMI_RESP_ERR_PREFIX)) {
    if (error) {
      *error = PayloadToString(response);
    }
    return false;
  }
  if (error) {
    *error = "Unexpected heartbeat response: " + PayloadToString(response);
  }
  return false;
}

bool RmiClient::receiveScreencap(net::TcpConnection& connection) {
  std::vector<uint8_t> data;
  std::string error;
  if (!receiveFrameSkippingHeartbeats(connection,
                                      &data,
                                      kScreencapTimeoutMs,
                                      kMaxFrameBytes,
                                      &error)) {
    setError(error);
    return false;
  }

  if (PayloadStartsWith(data, RMI_RESP_ERR_PREFIX)) {
    setError(PayloadToString(data));
    return true;
  }
  if (data.size() < sizeof(kPngSignature) ||
      std::memcmp(data.data(), kPngSignature, sizeof(kPngSignature)) != 0) {
    setError("Unexpected screencap payload (not a PNG).");
    return true;
  }

  int width = 0;
  int height = 0;
  int channels = 0;
  if (!stbi_info_from_memory(data.data(),
                             static_cast<int>(data.size()),
                             &width,
                             &height,
                             &channels)) {
    const char* reason = stbi_failure_reason();
    setError(reason ? reason : "Failed to parse PNG header.");
    return true;
  }
  if (width <= 0 || height <= 0) {
    setError("Invalid PNG dimensions.");
    return true;
  }
  const uint64_t pixel_count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
  if (pixel_count > kMaxScreencapPixels) {
    setError("PNG dimensions exceed limit.");
    return true;
  }
  stbi_uc* decoded = stbi_load_from_memory(data.data(),
                                           static_cast<int>(data.size()),
                                           &width,
                                           &height,
                                           &channels,
                                           4);
  if (!decoded) {
    const char* reason = stbi_failure_reason();
    setError(reason ? reason : "Failed to decode PNG screencap.");
    return true;
  }
  std::vector<uint8_t> pixels(decoded, decoded + (width * height * 4));
  stbi_image_free(decoded);

  setScreencapData(std::move(data), std::move(pixels), width, height);
  return true;
}

void RmiClient::joinWorker() {
  if (worker_.joinable()) {
    worker_.join();
  }
}
