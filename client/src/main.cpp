#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <deque>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#if defined(RMI_IMGUI_SDLRENDERER2)
#include "imgui_impl_sdlrenderer2.h"
#elif defined(RMI_IMGUI_SDLRENDERER)
#include "imgui_impl_sdlrenderer.h"
#else
#error "Unknown SDL renderer backend for Dear ImGui"
#endif
#include "imgui_stdlib.h"

#include "rmi_client.h"
#include "stb_image.h"

enum class DownloadAction {
  None = 0,
  Save,
  Preview
};

struct FileNode {
  std::string name;
  std::string path;
  bool is_dir = false;
  uint64_t size = 0;
  bool expanded = false;
  bool loading = false;
  std::string error;
  uint64_t list_version = 0;
  std::vector<FileNode> children;
  bool downloading = false;
  DownloadAction download_action = DownloadAction::None;
  uint64_t download_version = 0;
  std::string download_path;
  std::string download_error;
};

struct FileBrowserState {
  struct PreviewTab {
    std::string title;
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
    bool open = true;
    std::string error;
  };

  struct PendingPreview {
    std::string title;
    std::vector<uint8_t> data;
  };

  FileNode root;
  bool visible = false;
  bool pending_select = false;
  std::deque<std::string> console_lines;
  size_t console_last_count = 0;
  struct PendingSave {
    std::string suggested_name;
    std::vector<uint8_t> data;
  };
  std::deque<PendingSave> save_queue;
  std::deque<PendingPreview> preview_queue;
  std::vector<PreviewTab> preview_tabs;
  int preview_pending_select = -1;
  uint64_t preview_counter = 1;
  bool save_popup_open = false;
  std::string save_path_input;
  std::string save_error;
};

struct ScreencapViewState {
  struct Tab {
    std::string title;
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
    uint64_t capture_id = 0;
    std::vector<uint8_t> png;
    std::string saved_path;
    std::string save_error;
    bool open = true;
  };
  std::vector<Tab> tabs;
  uint64_t version = 0;
  uint64_t next_capture_id = 1;
  int pending_select = -1;
  std::string last_error;
};

struct AdbDevice {
  std::string serial;
  std::string state;
};

struct AdbState {
  std::vector<AdbDevice> devices;
  int selected = -1;
  std::string local_port;
  std::string remote_port;
  std::string status;
  std::string error;
  std::mutex start_mutex;
  std::string start_output;
  bool start_running = false;
  bool start_finished = false;
  int start_exit_code = 0;
  bool needs_refresh = true;
  bool needs_forward_check = true;
  std::string last_forward_serial;
  std::string last_forward_remote;
  std::string existing_forward_local;
};

struct ClientSlot {
  ClientConfig config;
  RmiClient client;
  AdbState adb_state;
  ScreencapViewState screencap_view;
  std::string press_keycode;
  std::string press_error;
  std::string upload_local_path;
  std::string upload_remote_path;
  std::string upload_error;
  std::string update_error;
  std::string update_status;
  FileBrowserState file_browser;
  int connect_tab = 1;
  bool connect_tab_pending = false;
  bool show_connect_popup = false;
  Uint64 reconnect_at_ticks = 0;
  bool reconnect_pending = false;
};

struct SettingsState {
  std::string path;
  std::string error;
  bool dirty = false;
  Uint64 last_change_ticks = 0;
};

namespace {

std::string TrimCopy(const std::string& text) {
  const size_t start = text.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(start, end - start + 1);
}

bool ParseKeycode(const std::string& text, int* value) {
  std::string trimmed = TrimCopy(text);
  if (trimmed.empty()) {
    return false;
  }
  size_t index = 0;
  if (trimmed[0] == '+') {
    index = 1;
  }
  if (index >= trimmed.size()) {
    return false;
  }
  for (; index < trimmed.size(); ++index) {
    if (!std::isdigit(static_cast<unsigned char>(trimmed[index]))) {
      return false;
    }
  }
  try {
    long long parsed = std::stoll(trimmed);
    if (parsed < 0 || parsed > std::numeric_limits<int>::max()) {
      return false;
    }
    if (value) {
      *value = static_cast<int>(parsed);
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool ParsePort(const std::string& text, int* value) {
  std::string trimmed = TrimCopy(text);
  if (trimmed.empty()) {
    return false;
  }
  for (char c : trimmed) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  try {
    const int port = std::stoi(trimmed);
    if (port <= 0 || port > 65535) {
      return false;
    }
    if (value) {
      *value = port;
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool RunCommandCapture(const std::string& command, std::string* output) {
#ifdef _WIN32
  FILE* pipe = _popen(command.c_str(), "r");
#else
  FILE* pipe = popen(command.c_str(), "r");
#endif
  if (!pipe) {
    return false;
  }

  std::string data;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), pipe)) {
    data += buffer;
  }

#ifdef _WIN32
  _pclose(pipe);
#else
  pclose(pipe);
#endif

  if (output) {
    *output = data;
  }
  return true;
}

std::vector<AdbDevice> ParseAdbDevices(const std::string& output) {
  std::vector<AdbDevice> devices;
  std::istringstream stream(output);
  std::string line;
  while (std::getline(stream, line)) {
    line = TrimCopy(line);
    if (line.empty()) {
      continue;
    }
    if (line.find("List of devices") == 0) {
      continue;
    }
    std::istringstream line_stream(line);
    AdbDevice device;
    if (!(line_stream >> device.serial >> device.state)) {
      continue;
    }
    devices.push_back(device);
  }
  return devices;
}

struct AdbForward {
  std::string serial;
  std::string local;
  std::string remote;
};

std::vector<AdbForward> ParseAdbForwards(const std::string& output) {
  std::vector<AdbForward> forwards;
  std::istringstream stream(output);
  std::string line;
  while (std::getline(stream, line)) {
    line = TrimCopy(line);
    if (line.empty()) {
      continue;
    }
    std::istringstream line_stream(line);
    AdbForward entry;
    if (!(line_stream >> entry.serial >> entry.local >> entry.remote)) {
      continue;
    }
    forwards.push_back(entry);
  }
  return forwards;
}

bool RefreshAdbDevices(AdbState* state) {
  if (!state) {
    return false;
  }
  state->error.clear();
  state->devices.clear();
  state->selected = -1;

  std::string output;
  if (!RunCommandCapture("adb devices -l", &output)) {
    state->error = "Failed to run adb devices.";
    return false;
  }
  state->devices = ParseAdbDevices(output);
  if (state->devices.empty()) {
    state->error = "No adb devices detected.";
    return false;
  }
  state->selected = 0;
  state->needs_forward_check = true;
  return true;
}

bool FindExistingForward(const std::string& serial,
                         int remote_port,
                         std::string* local_port,
                         std::string* error) {
  std::string output;
  if (!RunCommandCapture("adb forward --list", &output)) {
    if (error) {
      *error = "Failed to query adb forward list.";
    }
    return false;
  }
  const std::string remote_token = "tcp:" + std::to_string(remote_port);
  const auto forwards = ParseAdbForwards(output);
  for (const auto& entry : forwards) {
    if (entry.serial != serial) {
      continue;
    }
    if (entry.remote != remote_token) {
      continue;
    }
    if (entry.local.rfind("tcp:", 0) == 0) {
      if (local_port) {
        *local_port = entry.local.substr(4);
      }
      return true;
    }
  }
  return false;
}

bool FindOpenPort(int* port, std::string* error) {
#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    if (error) {
      *error = "WSAStartup failed.";
    }
    return false;
  }
  SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == INVALID_SOCKET) {
    if (error) {
      *error = "socket() failed.";
    }
    WSACleanup();
    return false;
  }
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (error) {
      *error = "bind() failed.";
    }
    closesocket(sock);
    WSACleanup();
    return false;
  }
  int len = sizeof(addr);
  if (getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    if (error) {
      *error = "getsockname() failed.";
    }
    closesocket(sock);
    WSACleanup();
    return false;
  }
  const int chosen = ntohs(addr.sin_port);
  closesocket(sock);
  WSACleanup();
  if (port) {
    *port = chosen;
  }
  return chosen > 0;
#else
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    if (error) {
      *error = "socket() failed.";
    }
    return false;
  }
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (error) {
      *error = "bind() failed.";
    }
    close(sock);
    return false;
  }
  socklen_t len = sizeof(addr);
  if (getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    if (error) {
      *error = "getsockname() failed.";
    }
    close(sock);
    return false;
  }
  const int chosen = ntohs(addr.sin_port);
  close(sock);
  if (port) {
    *port = chosen;
  }
  return chosen > 0;
#endif
}

bool RunAdbForward(const AdbDevice& device,
                   int local_port,
                   int remote_port,
                   std::string* error) {
  std::string existing;
  if (FindExistingForward(device.serial, remote_port, &existing, nullptr)) {
    if (existing == std::to_string(local_port)) {
      return true;
    }
  }
  std::ostringstream cmd;
  cmd << "adb -s " << device.serial << " forward tcp:" << local_port << " tcp:" << remote_port;
  const int status = std::system(cmd.str().c_str());
  if (status != 0) {
    if (error) {
      *error = "adb forward failed.";
    }
    return false;
  }
  return true;
}

bool RunAdbShellOnce(const AdbDevice& device,
                     const std::string& command,
                     std::string* error) {
  std::ostringstream cmd;
  cmd << "adb -s " << device.serial << " shell " << command;
  const int status = std::system(cmd.str().c_str());
  if (status != 0) {
    if (error) {
      *error = "adb shell failed.";
    }
    return false;
  }
  return true;
}

bool RunAdbShellStatus(const AdbDevice& device,
                       const std::string& command,
                       int* exit_code) {
  std::ostringstream cmd;
  cmd << "adb -s " << device.serial << " shell " << command;
  const int status = std::system(cmd.str().c_str());
  if (exit_code) {
    *exit_code = status;
  }
  return status == 0;
}

bool AdbFileExecutable(const AdbDevice& device, const std::string& path) {
  int exit_code = 0;
  return RunAdbShellStatus(device, "test -x " + path + " >/dev/null 2>&1", &exit_code);
}

bool AdbCanExecute(const AdbDevice& device, const std::string& path) {
  int exit_code = 0;
  return RunAdbShellStatus(device, path + " >/dev/null 2>&1", &exit_code);
}

void AppendStartOutput(AdbState* state, const std::string& text) {
  if (!state) {
    return;
  }
  std::lock_guard<std::mutex> lock(state->start_mutex);
  state->start_output += text;
  const size_t max_output = 8192;
  if (state->start_output.size() > max_output) {
    state->start_output.erase(0, state->start_output.size() - max_output);
  }
}

bool RunAdbShellCapture(const AdbDevice& device,
                        const std::string& command,
                        std::string* output,
                        std::string* error);

bool AdbGetFileSize(AdbState* state,
                    const AdbDevice& device,
                    const std::string& path,
                    uint64_t* size_out) {
  std::string output;
  std::string error;
  RunAdbShellCapture(device, "wc -c < " + path, &output, &error);
  output = TrimCopy(output);
  if (output.empty()) {
    if (!error.empty()) {
      AppendStartOutput(state, error + "\n");
    }
    return false;
  }
  size_t idx = 0;
  while (idx < output.size() && std::isdigit(static_cast<unsigned char>(output[idx]))) {
    ++idx;
  }
  if (idx == 0) {
    AppendStartOutput(state, "Unexpected adb output: " + output + "\n");
    return false;
  }
  try {
    uint64_t size = std::stoull(output.substr(0, idx));
    if (size_out) {
      *size_out = size;
    }
    return true;
  } catch (...) {
    AppendStartOutput(state, "Failed to parse adb output: " + output + "\n");
    return false;
  }
}

std::string StripCarriageReturns(const std::string& text) {
  std::string cleaned = text;
  cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), '\r'), cleaned.end());
  return cleaned;
}

bool RunAdbShellCapture(const AdbDevice& device,
                        const std::string& command,
                        std::string* output,
                        std::string* error) {
  std::ostringstream cmd;
  cmd << "adb -s " << device.serial << " shell " << command << " 2>&1";
#ifdef _WIN32
  FILE* pipe = _popen(cmd.str().c_str(), "r");
#else
  FILE* pipe = popen(cmd.str().c_str(), "r");
#endif
  if (!pipe) {
    if (error) {
      *error = "Failed to start adb shell.";
    }
    return false;
  }
  std::string result;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), pipe)) {
    result += buffer;
  }
#ifdef _WIN32
  const int status = _pclose(pipe);
#else
  const int status = pclose(pipe);
#endif
  if (output) {
    *output = StripCarriageReturns(result);
  }
  if (status != 0) {
    if (error) {
      *error = "adb shell failed.";
    }
    return false;
  }
  return true;
}

bool OutputHasAny(const std::string& output,
                  const std::initializer_list<const char*>& patterns) {
  for (const char* pattern : patterns) {
    if (pattern && output.find(pattern) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool AdbScreenOn(AdbState* state, const AdbDevice& device) {
  std::string output;
  std::string error;
  if (!RunAdbShellCapture(device, "dumpsys power", &output, &error)) {
    if (state && !error.empty()) {
      AppendStartOutput(state, "ADB dumpsys power failed: " + error + "\n");
    }
  }
  return OutputHasAny(output,
                      {"mWakefulness=Awake",
                       "Display Power: state=ON",
                       "mScreenOnFully=true",
                       "mInteractive=true"});
}

bool AdbIsLocked(AdbState* state, const AdbDevice& device) {
  std::string output;
  std::string error;
  if (!RunAdbShellCapture(device, "dumpsys window policy", &output, &error)) {
    if (state && !error.empty()) {
      AppendStartOutput(state, "ADB dumpsys window policy failed: " + error + "\n");
    }
  }
  return OutputHasAny(output,
                      {"mShowingLockscreen=true",
                       "isStatusBarKeyguard=true",
                       "mDreamingLockscreen=true"});
}

void RunAdbShellBestEffort(AdbState* state,
                           const AdbDevice& device,
                           const std::string& command) {
  std::string error;
  if (!RunAdbShellOnce(device, command, &error)) {
    if (state) {
      AppendStartOutput(state, "ADB shell failed: " + command + "\n");
    }
  }
}

bool AdbFileExists(AdbState* state, const AdbDevice& device, const std::string& path) {
  uint64_t size = 0;
  return AdbGetFileSize(state, device, path, &size);
}

bool RunAdbPush(AdbState* state,
                const AdbDevice& device,
                const std::string& local_path,
                const std::string& remote_path) {
  std::ostringstream cmd;
  cmd << "adb -s " << device.serial << " push " << local_path << " " << remote_path;
  const int status = std::system(cmd.str().c_str());
  if (status != 0) {
    AppendStartOutput(state, "adb push failed.\n");
    return false;
  }
  return true;
}

bool ResolveLocalRmiPath(std::filesystem::path* path_out, std::string* error);

bool EnsureAdbServerBinary(AdbState* state, const AdbDevice& device) {
  const std::string path = "/data/local/tmp/rmi";
  std::filesystem::path local_path;
  std::string resolve_error;
  if (!ResolveLocalRmiPath(&local_path, &resolve_error)) {
    AppendStartOutput(state, resolve_error + "\n");
    return false;
  }
  std::error_code fs_error;
  const uint64_t local_size = std::filesystem::file_size(local_path, fs_error);
  if (fs_error) {
    AppendStartOutput(state, "Local rmi binary not found: " + local_path.string() + "\n");
    return false;
  }

  uint64_t remote_size = 0;
  const bool has_remote = AdbGetFileSize(state, device, path, &remote_size);
  if (has_remote) {
    bool needs_replace = false;
    if (!AdbFileExecutable(device, path)) {
      AppendStartOutput(state, "Server binary not executable. Replacing...\n");
      needs_replace = true;
    } else if (!AdbCanExecute(device, path)) {
      AppendStartOutput(state, "Server binary failed to execute. Replacing...\n");
      needs_replace = true;
    }
    if (remote_size != local_size) {
      AppendStartOutput(state, "Server binary size mismatch. Replacing...\n");
      needs_replace = true;
    }
    if (!needs_replace) {
      AppendStartOutput(state, "Server binary already on device.\n");
      return true;
    }
  } else {
    AppendStartOutput(state, "Server binary missing. Pushing...\n");
  }

  if (!RunAdbPush(state, device, local_path.string(), "/data/local/tmp/rmi")) {
    return false;
  }
  std::string error;
  if (!RunAdbShellOnce(device, "chmod 777 /data/local/tmp/rmi", &error)) {
    AppendStartOutput(state, "chmod failed.\n");
    return false;
  }
  AppendStartOutput(state, "Server binary pushed to /data/local/tmp/rmi.\n");
  return true;
}

bool EnsureAdbConfig(AdbState* state, const AdbDevice& device) {
  if (AdbFileExists(state, device, "/data/local/tmp/rmi.config")) {
    AppendStartOutput(state, "rmi.config already on device.\n");
    return true;
  }
  AppendStartOutput(state, "rmi.config missing. Pushing...\n");
  std::error_code fs_error;
  const std::filesystem::path local_path =
      std::filesystem::current_path() / "rmi.config";
  if (!std::filesystem::exists(local_path, fs_error) || fs_error) {
    AppendStartOutput(state, "Local rmi.config not found: " + local_path.string() + "\n");
    return false;
  }
  if (!RunAdbPush(state, device, local_path.string(), "/data/local/tmp/rmi.config")) {
    return false;
  }
  std::string error;
  if (!RunAdbShellOnce(device, "chmod 666 /data/local/tmp/rmi.config", &error)) {
    AppendStartOutput(state, "chmod failed.\n");
    return false;
  }
  AppendStartOutput(state, "rmi.config pushed to /data/local/tmp/rmi.config.\n");
  return true;
}

void RunBluetoothSetup(AdbState* state, const AdbDevice& device) {
  AppendStartOutput(state, "Bluetooth prompt detected. Preparing device...\n");

  if (!AdbScreenOn(state, device)) {
    RunAdbShellBestEffort(state, device, "input keyevent 224");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  if (AdbIsLocked(state, device)) {
    RunAdbShellBestEffort(state, device, "input keyevent 82");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  RunAdbShellBestEffort(state, device, "am start -a android.settings.BLUETOOTH_SETTINGS");
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  RunAdbShellBestEffort(state, device, "input tap 1658 278");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  RunAdbShellBestEffort(state, device, "input tap 1658 278");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  RunAdbShellBestEffort(state, device, "input keyevent 4");
  AppendStartOutput(state, "Bluetooth setup sequence finished.\n");
}

void StartAdbServerAsync(AdbState* state, const AdbDevice& device) {
  if (!state) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(state->start_mutex);
    if (state->start_running) {
      return;
    }
    state->start_output.clear();
    state->start_running = true;
    state->start_finished = false;
    state->start_exit_code = 0;
  }
  std::thread([state, device]() {
    std::ostringstream cmd;
    cmd << "adb -s " << device.serial << " shell /data/local/tmp/rmi start 2>&1";
    AppendStartOutput(state, "Starting server...\n");
    if (!EnsureAdbServerBinary(state, device)) {
      std::lock_guard<std::mutex> lock(state->start_mutex);
      state->start_running = false;
      state->start_finished = true;
      state->start_exit_code = -1;
      return;
    }
    if (!EnsureAdbConfig(state, device)) {
      AppendStartOutput(state, "rmi.config not available; server will use defaults.\n");
    }
    const std::string bluetooth_prompt = "Enable bluetooth to start the interface.";
    bool bluetooth_ran = false;
#ifdef _WIN32
    FILE* pipe = _popen(cmd.str().c_str(), "r");
#else
    FILE* pipe = popen(cmd.str().c_str(), "r");
#endif
    if (!pipe) {
      AppendStartOutput(state, "Failed to start adb shell.\n");
      std::lock_guard<std::mutex> lock(state->start_mutex);
      state->start_running = false;
      state->start_finished = true;
      state->start_exit_code = -1;
      return;
    }
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
      std::string line = StripCarriageReturns(buffer);
      AppendStartOutput(state, line);
      if (!bluetooth_ran && line.find(bluetooth_prompt) != std::string::npos) {
        bluetooth_ran = true;
        std::thread([state, device]() {
          RunBluetoothSetup(state, device);
        }).detach();
      }
    }
#ifdef _WIN32
    const int status = _pclose(pipe);
#else
    const int status = pclose(pipe);
#endif
    {
      std::lock_guard<std::mutex> lock(state->start_mutex);
      state->start_running = false;
      state->start_finished = true;
      state->start_exit_code = status;
    }
  }).detach();
}

std::string EscapeSetting(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '=':
        out += "\\=";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

std::string UnescapeSetting(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    char c = value[i];
    if (c == '\\' && i + 1 < value.size()) {
      char next = value[i + 1];
      switch (next) {
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case '=':
          out += '=';
          break;
        case '\\':
          out += '\\';
          break;
        default:
          out += next;
          break;
      }
      ++i;
    } else {
      out += c;
    }
  }
  return out;
}

std::filesystem::path SettingsPath() {
  std::error_code fs_error;
  std::filesystem::path cwd = std::filesystem::current_path(fs_error);
  if (fs_error) {
    return std::filesystem::path("client_settings.ini");
  }
  return cwd / "client_settings.ini";
}

bool ResolveLocalRmiPath(std::filesystem::path* path_out, std::string* error) {
  std::error_code fs_error;
  std::filesystem::path cwd = std::filesystem::current_path(fs_error);
  std::filesystem::path base_dir;
  char* base = SDL_GetBasePath();
  if (base) {
    base_dir = std::filesystem::path(base);
    SDL_free(base);
  } else {
    base_dir = cwd;
  }

  std::vector<std::filesystem::path> candidates;
  auto add_candidate = [&candidates](const std::filesystem::path& path) {
    if (path.empty()) {
      return;
    }
    for (const auto& existing : candidates) {
      if (existing == path) {
        return;
      }
    }
    candidates.push_back(path);
  };

  add_candidate(base_dir / "build" / "rmi");
  add_candidate(base_dir / "rmi");
  if (!cwd.empty()) {
    add_candidate(cwd / "build" / "rmi");
    add_candidate(cwd / "rmi");
  }

  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate, fs_error) &&
        std::filesystem::is_regular_file(candidate, fs_error)) {
      if (path_out) {
        *path_out = candidate;
      }
      return true;
    }
  }

  if (error) {
    std::string joined;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (i > 0) {
        joined += ", ";
      }
      joined += candidates[i].string();
    }
    *error = "Local rmi binary not found. Checked: " + joined;
  }
  return false;
}

bool LoadSettings(ClientConfig* config, int* connect_tab, std::string* error) {
  if (!config) {
    if (error) {
      *error = "Invalid config pointer.";
    }
    return false;
  }

  const std::filesystem::path path = SettingsPath();
  std::error_code fs_error;
  if (!std::filesystem::exists(path, fs_error)) {
    return true;
  }

  std::ifstream file(path);
  if (!file) {
    if (error) {
      *error = "Failed to open settings file.";
    }
    return false;
  }

  std::string line;
  while (std::getline(file, line)) {
    line = TrimCopy(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') {
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = TrimCopy(line.substr(0, eq));
    const std::string value = UnescapeSetting(line.substr(eq + 1));
    if (key == "host") {
      config->host = value;
    } else if (key == "port") {
      config->port = value;
    } else if (key == "username") {
      config->username = value;
    } else if (key == "password") {
      config->password = value;
    } else if (key == "connect_tab") {
      try {
        int tab = std::stoi(value);
        if (connect_tab) {
          *connect_tab = (tab == 1) ? 1 : 0;
        }
      } catch (...) {
      }
    }
  }

  return true;
}

bool SaveSettings(const ClientConfig& config, int connect_tab, std::string* error) {
  const std::filesystem::path path = SettingsPath();
  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    if (error) {
      *error = "Failed to write settings file.";
    }
    return false;
  }

  file << "host=" << EscapeSetting(config.host) << "\n";
  file << "port=" << EscapeSetting(config.port) << "\n";
  file << "username=" << EscapeSetting(config.username) << "\n";
  file << "password=" << EscapeSetting(config.password) << "\n";
  file << "connect_tab=" << connect_tab << "\n";
  if (!file.good()) {
    if (error) {
      *error = "Failed to save settings file.";
    }
    return false;
  }
  return true;
}

}  // namespace

static void UpdateScreencapTexture(SDL_Renderer* renderer,
                                   RmiClient& client,
                                   ScreencapViewState* view) {
  const uint64_t latest_version = client.screencapVersion();
  if (latest_version == 0 || latest_version == view->version) {
    return;
  }

  std::vector<uint8_t> png;
  std::vector<uint8_t> pixels;
  int width = 0;
  int height = 0;
  uint64_t version = 0;
  if (!client.getScreencapImage(&pixels, &width, &height, &version)) {
    return;
  }
  uint64_t png_version = 0;
  if (!client.getScreencapPng(&png, &png_version)) {
    return;
  }
  if (png_version != version) {
    return;
  }
  if (version != latest_version) {
    return;
  }

  const Uint32 format =
      (SDL_BYTEORDER == SDL_LIL_ENDIAN) ? SDL_PIXELFORMAT_ABGR8888 : SDL_PIXELFORMAT_RGBA8888;
  SDL_Texture* texture = SDL_CreateTexture(renderer,
                                           format,
                                           SDL_TEXTUREACCESS_STREAMING,
                                           width,
                                           height);
  if (!texture) {
    view->last_error = std::string("SDL_CreateTexture failed: ") + SDL_GetError();
    return;
  }
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  if (SDL_UpdateTexture(texture, nullptr, pixels.data(), width * 4) != 0) {
    SDL_DestroyTexture(texture);
    view->last_error = std::string("SDL_UpdateTexture failed: ") + SDL_GetError();
    return;
  }

  ScreencapViewState::Tab tab;
  tab.capture_id = view->next_capture_id++;
  tab.title = "Screencap " + std::to_string(tab.capture_id);
  tab.texture = texture;
  tab.width = width;
  tab.height = height;
  tab.png = std::move(png);
  view->tabs.push_back(std::move(tab));
  view->pending_select = static_cast<int>(view->tabs.size()) - 1;
  view->version = version;
  view->last_error.clear();
}

static bool SavePngToFile(const std::vector<uint8_t>& png,
                          uint64_t capture_id,
                          std::string* out_path,
                          std::string* error) {
  if (png.empty()) {
    if (error) {
      *error = "No screencap data to save.";
    }
    return false;
  }
  std::error_code fs_error;
  std::filesystem::path capture_dir = std::filesystem::current_path() / "captures";
  std::filesystem::create_directories(capture_dir, fs_error);
  if (fs_error) {
    if (error) {
      *error = "Failed to create captures directory: " + fs_error.message();
    }
    return false;
  }
  const std::string filename = "screencap_" + std::to_string(capture_id) + ".png";
  std::filesystem::path file_path = capture_dir / filename;
  std::filesystem::path absolute_path = std::filesystem::absolute(file_path, fs_error);
  if (fs_error) {
    absolute_path = file_path;
  }
  std::ofstream out(file_path, std::ios::binary);
  if (!out) {
    if (error) {
      *error = "Failed to open screencap file for writing.";
    }
    return false;
  }
  out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  if (!out.good()) {
    if (error) {
      *error = "Failed to write screencap file.";
    }
    return false;
  }
  if (out_path) {
    *out_path = absolute_path.string();
  }
  return true;
}

static std::string JoinRemotePath(const std::string& parent, const std::string& name) {
  if (parent.empty() || parent == "/") {
    return "/" + name;
  }
  if (parent.back() == '/') {
    return parent + name;
  }
  return parent + "/" + name;
}

static void AddFileBrowserLog(FileBrowserState& state, const std::string& text) {
  state.console_lines.push_back(text);
  const size_t max_lines = 8;
  while (state.console_lines.size() > max_lines) {
    state.console_lines.pop_front();
  }
}

static bool IsPreviewSupported(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) {
    return false;
  }
  std::string ext = name.substr(dot + 1);
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return ext == "png" || ext == "jpg" || ext == "jpeg";
}

static void QueuePreview(FileBrowserState& state,
                         const std::string& title,
                         std::vector<uint8_t> data) {
  FileBrowserState::PendingPreview pending;
  pending.title = title;
  pending.data = std::move(data);
  state.preview_queue.push_back(std::move(pending));
}

static void UpdateFilePreviewTextures(SDL_Renderer* renderer, FileBrowserState& state) {
  while (!state.preview_queue.empty()) {
    auto pending = std::move(state.preview_queue.front());
    state.preview_queue.pop_front();

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(pending.data.data(),
                                             static_cast<int>(pending.data.size()),
                                             &width,
                                             &height,
                                             &channels,
                                             4);
    FileBrowserState::PreviewTab tab;
    tab.title = pending.title;
    if (!decoded || width <= 0 || height <= 0) {
      const char* reason = stbi_failure_reason();
      tab.error = reason ? reason : "Failed to decode image preview.";
    } else {
      const Uint32 format = (SDL_BYTEORDER == SDL_LIL_ENDIAN)
          ? SDL_PIXELFORMAT_ABGR8888
          : SDL_PIXELFORMAT_RGBA8888;
      tab.texture = SDL_CreateTexture(renderer,
                                      format,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      width,
                                      height);
      if (!tab.texture) {
        tab.error = std::string("SDL_CreateTexture failed: ") + SDL_GetError();
      } else if (SDL_UpdateTexture(tab.texture,
                                   nullptr,
                                   decoded,
                                   width * 4) != 0) {
        tab.error = std::string("SDL_UpdateTexture failed: ") + SDL_GetError();
        SDL_DestroyTexture(tab.texture);
        tab.texture = nullptr;
      } else {
        SDL_SetTextureBlendMode(tab.texture, SDL_BLENDMODE_BLEND);
        tab.width = width;
        tab.height = height;
      }
    }
    if (decoded) {
      stbi_image_free(decoded);
    }

    state.preview_tabs.push_back(std::move(tab));
    state.preview_pending_select = static_cast<int>(state.preview_tabs.size()) - 1;
  }
}

static void RequestNodeList(RmiClient& client, FileNode& node, FileBrowserState* state) {
  node.loading = true;
  node.error.clear();
  if (state) {
    AddFileBrowserLog(*state, "LIST " + node.path);
  }
  client.requestFileList(node.path);
}

static void RefreshNodeChildren(RmiClient& client,
                                FileNode& node,
                                const std::vector<RmiClient::FileEntry>& entries,
                                bool is_connected,
                                FileBrowserState* state) {
  std::unordered_map<std::string, FileNode> existing;
  existing.reserve(node.children.size());
  for (auto& child : node.children) {
    existing.emplace(child.path, std::move(child));
  }
  node.children.clear();
  node.children.reserve(entries.size());

  for (const auto& entry : entries) {
    const std::string path = JoinRemotePath(node.path, entry.name);
    FileNode child;
    auto it = existing.find(path);
    if (it != existing.end()) {
      child = std::move(it->second);
    }
    child.name = entry.name;
    child.path = path;
    child.is_dir = entry.is_dir;
    child.size = entry.size;
    if (!child.is_dir) {
      child.children.clear();
      child.expanded = false;
      child.loading = false;
      child.error.clear();
    }
    node.children.push_back(std::move(child));
  }

  if (is_connected) {
    for (auto& child : node.children) {
      if (child.is_dir && child.expanded) {
        RequestNodeList(client, child, state);
      }
    }
  }
}

static void ApplyListResult(RmiClient& client,
                            FileNode& node,
                            bool is_connected,
                            FileBrowserState* state) {
  std::vector<RmiClient::FileEntry> entries;
  std::string error;
  uint64_t version = 0;
  if (!client.getFileList(node.path, &entries, &error, &version)) {
    return;
  }
  if (version <= node.list_version) {
    return;
  }
  node.list_version = version;
  node.loading = false;
  node.error = error;
  if (!error.empty()) {
    node.children.clear();
    return;
  }
  RefreshNodeChildren(client, node, entries, is_connected, state);
}

static void ApplyDownloadResult(RmiClient& client, FileNode& node, FileBrowserState& state) {
  if (node.is_dir) {
    return;
  }
  std::vector<uint8_t> data;
  std::string error;
  uint64_t version = 0;
  if (!client.getDownloadResult(node.path, &data, &error, &version)) {
    return;
  }
  if (version <= node.download_version) {
    return;
  }
  node.download_version = version;
  node.downloading = false;
  node.download_path.clear();
  node.download_error = error;
  if (error.empty()) {
    if (node.download_action == DownloadAction::Preview) {
      std::string title = node.name.empty()
          ? ("Preview " + std::to_string(state.preview_counter++))
          : ("Preview " + node.name);
      QueuePreview(state, title, std::move(data));
    } else {
      FileBrowserState::PendingSave pending;
      pending.suggested_name = node.name;
      pending.data = std::move(data);
      state.save_queue.push_back(std::move(pending));
    }
  }
  node.download_action = DownloadAction::None;
}

static void DrawFileNode(RmiClient& client,
                         FileNode& node,
                         FileNode* parent,
                         bool is_connected,
                         FileBrowserState& state) {
  ApplyListResult(client, node, is_connected, &state);

  ImGui::PushID(node.path.c_str());
  if (node.is_dir) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanAvailWidth;
    const bool opened = ImGui::TreeNodeEx("dir", flags, "%s", node.name.c_str());
    ImGui::OpenPopupOnItemClick("dir_ctx", ImGuiPopupFlags_MouseButtonRight);
    if (ImGui::BeginPopup("dir_ctx")) {
      ImGui::BeginDisabled(!is_connected);
      if (ImGui::MenuItem("Reload")) {
        RequestNodeList(client, node, &state);
      }
      ImGui::EndDisabled();
      if (parent != nullptr) {
        ImGui::BeginDisabled(!is_connected);
        if (ImGui::MenuItem("Delete")) {
          AddFileBrowserLog(state, "DELETE " + node.path);
          client.requestDelete(node.path);
          RequestNodeList(client, *parent, &state);
        }
        ImGui::EndDisabled();
      }
      ImGui::EndPopup();
    }
    if (node.loading) {
      ImGui::SameLine();
      ImGui::TextDisabled("Loading...");
    }
    if (!node.error.empty()) {
      ImGui::TextWrapped("Error: %s", node.error.c_str());
    }
    if (opened) {
      if (!node.expanded) {
        node.expanded = true;
        if (node.children.empty() && !node.loading) {
          if (is_connected) {
            RequestNodeList(client, node, &state);
          } else {
            node.error = "Not connected.";
          }
        }
      }
      for (auto& child : node.children) {
        DrawFileNode(client, child, &node, is_connected, state);
      }
      ImGui::TreePop();
    } else {
      node.expanded = false;
    }
  } else {
    ApplyDownloadResult(client, node, state);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf |
        ImGuiTreeNodeFlags_NoTreePushOnOpen |
        ImGuiTreeNodeFlags_SpanAvailWidth;
    ImGui::TreeNodeEx("file", flags, "%s", node.name.c_str());
    ImGui::OpenPopupOnItemClick("file_ctx", ImGuiPopupFlags_MouseButtonRight);
    ImGui::SameLine();
    ImGui::TextDisabled("%llu bytes", static_cast<unsigned long long>(node.size));
    if (ImGui::BeginPopup("file_ctx")) {
      ImGui::BeginDisabled(!is_connected);
      if (ImGui::MenuItem("Download")) {
        node.downloading = true;
        node.download_error.clear();
        node.download_path.clear();
        node.download_action = DownloadAction::Save;
        AddFileBrowserLog(state, "DOWNLOAD " + node.path);
        client.requestDownload(node.path);
      }
      ImGui::EndDisabled();
      const bool preview_supported = IsPreviewSupported(node.name);
      const bool preview_enabled = is_connected && preview_supported;
      ImGui::BeginDisabled(!preview_enabled);
      if (ImGui::MenuItem("Preview")) {
        node.downloading = true;
        node.download_error.clear();
        node.download_path.clear();
        node.download_action = DownloadAction::Preview;
        AddFileBrowserLog(state, "DOWNLOAD " + node.path + " (preview)");
        client.requestDownload(node.path);
      }
      ImGui::EndDisabled();
      if (!preview_enabled && ImGui::IsItemHovered()) {
        if (!preview_supported) {
          ImGui::SetTooltip("Preview supports .png/.jpg/.jpeg only.");
        }
      }
      ImGui::BeginDisabled(!is_connected);
      if (ImGui::MenuItem("Delete")) {
        AddFileBrowserLog(state, "DELETE " + node.path);
        client.requestDelete(node.path);
        if (parent != nullptr) {
          RequestNodeList(client, *parent, &state);
        }
      }
      ImGui::EndDisabled();
      ImGui::EndPopup();
    }
    if (node.downloading) {
      uint64_t received = 0;
      uint64_t total = 0;
      bool in_progress = false;
      if (client.getDownloadProgress(node.path, &received, &total, &in_progress) &&
          in_progress && total > 0) {
        const float progress = static_cast<float>(received) /
            static_cast<float>(total);
        const std::string overlay = std::to_string(received) + " / " +
            std::to_string(total) + " bytes";
        ImGui::ProgressBar(progress, ImVec2(-1, 0), overlay.c_str());
      } else {
        ImGui::ProgressBar(0.0f, ImVec2(-1, 0), "Downloading...");
      }
    }
    if (!node.download_error.empty()) {
      ImGui::TextWrapped("Download error: %s", node.download_error.c_str());
    }
  }
  ImGui::PopID();
}

static void DrawFileBrowser(RmiClient& client, FileBrowserState& state, bool is_connected) {
  if (state.root.path.empty()) {
    state.root.name = "/";
    state.root.path = "/";
    state.root.is_dir = true;
    state.root.expanded = true;
    if (is_connected) {
      RequestNodeList(client, state.root, &state);
    } else {
      state.root.error = "Not connected.";
    }
  }

  ImGui::Text("Command Log");
  ImGui::BeginChild("file_browser_console", ImVec2(0, 80), true);
  const bool auto_scroll = state.console_lines.size() != state.console_last_count;
  if (state.console_lines.empty()) {
    ImGui::TextDisabled("No commands sent yet.");
  } else {
    for (const auto& line : state.console_lines) {
      ImGui::TextUnformatted(line.c_str());
    }
  }
  if (auto_scroll) {
    ImGui::SetScrollHereY(1.0f);
  }
  ImGui::EndChild();
  ImGui::Spacing();
  state.console_last_count = state.console_lines.size();

  if (!state.save_popup_open && !state.save_queue.empty()) {
    const auto& pending = state.save_queue.front();
    std::filesystem::path suggested = std::filesystem::current_path();
    suggested /= "downloads";
    suggested /= pending.suggested_name;
    state.save_path_input = suggested.string();
    state.save_popup_open = true;
    state.save_error.clear();
  }

  ImGui::BeginChild("file_browser_tree", ImVec2(0, 0), true);
  ApplyListResult(client, state.root, is_connected, &state);
  DrawFileNode(client, state.root, nullptr, is_connected, state);
  ImGui::EndChild();

  if (state.save_popup_open) {
    ImGui::OpenPopup("Save Download");
  }
  if (ImGui::BeginPopupModal("Save Download", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped("Choose a destination for the downloaded file.");
    ImGui::InputText("Save Path", &state.save_path_input);
    if (!state.save_error.empty()) {
      ImGui::TextWrapped("Save error: %s", state.save_error.c_str());
    }
    if (ImGui::Button("Save", ImVec2(120, 0))) {
      std::error_code fs_error;
      const auto& pending = state.save_queue.front();
      std::filesystem::path dest(state.save_path_input);
      if (dest.empty()) {
        state.save_error = "Save path is empty.";
      } else {
        std::filesystem::create_directories(dest.parent_path(), fs_error);
        if (fs_error) {
          state.save_error = fs_error.message();
        } else {
          std::ofstream out(dest, std::ios::binary);
          if (!out) {
            state.save_error = "Failed to open file for writing.";
          } else {
            out.write(reinterpret_cast<const char*>(pending.data.data()),
                      static_cast<std::streamsize>(pending.data.size()));
            if (!out.good()) {
              state.save_error = "Failed to write file.";
            } else {
              AddFileBrowserLog(state, "SAVED " + pending.suggested_name + " -> " + dest.string());
              state.save_queue.pop_front();
              state.save_popup_open = false;
              state.save_error.clear();
              ImGui::CloseCurrentPopup();
            }
          }
        }
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      if (!state.save_queue.empty()) {
        state.save_queue.pop_front();
      }
      state.save_popup_open = false;
      state.save_error.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

static bool DrawClientPanel(int index,
                            ClientSlot& slot,
                            const SettingsState& settings) {
  ImGui::PushID(index);
  ImGui::BeginChild("client_panel", ImVec2(0, 0), true);

  ImGui::Text("Client %d", index + 1);
  ImGui::Separator();

  const ClientStatus status = slot.client.status();
  const bool is_connected = status == ClientStatus::Connected;

  const float total_width = ImGui::GetContentRegionAvail().x;
  float left_width = std::min(360.0f, total_width * 0.45f);
  if (left_width < 220.0f) {
    left_width = std::max(200.0f, total_width * 0.35f);
  }

  ImGui::BeginChild("client_left", ImVec2(left_width, 0), true);
  ImGui::Text("Commands");
  ImGui::Separator();

  ImGui::BeginDisabled(!is_connected);
  if (ImGui::Button("Screencap", ImVec2(-1, 0))) {
    slot.client.sendScreencap();
  }
  if (ImGui::Button("Get Version", ImVec2(-1, 0))) {
    slot.client.sendVersion();
  }
  if (ImGui::Button("Focus", ImVec2(-1, 0))) {
    slot.client.sendPressInput(80);
  }
  if (ImGui::Button("Take Picture", ImVec2(-1, 0))) {
    slot.client.sendPressInput(27);
  }
  if (ImGui::Button("Restart Server", ImVec2(-1, 0))) {
    slot.client.sendRestart();
  }
  if (ImGui::Button("Update Server", ImVec2(-1, 0))) {
    std::filesystem::path local_path;
    std::string resolve_error;
    if (!ResolveLocalRmiPath(&local_path, &resolve_error)) {
      slot.update_error = resolve_error;
      slot.update_status.clear();
    } else {
      slot.client.sendUploadAndRestart(local_path.string(), "/data/local/tmp/rmi");
      slot.reconnect_pending = true;
      slot.reconnect_at_ticks = SDL_GetTicks64() + 2000;
      slot.update_status = "Uploading and restarting server...";
      slot.update_error.clear();
    }
  }
  if (ImGui::Button("File Browser", ImVec2(-1, 0))) {
    slot.file_browser.visible = true;
    slot.file_browser.pending_select = true;
  }
  if (ImGui::Button("Quit Server", ImVec2(-1, 0))) {
    slot.client.sendQuit();
  }
  ImGui::EndDisabled();

  ImGui::Separator();
  ImGui::InputText("Press Keycode", &slot.press_keycode);
  const bool has_keycode = !TrimCopy(slot.press_keycode).empty();
  ImGui::BeginDisabled(!is_connected || !has_keycode);
  if (ImGui::Button("Send PRESS", ImVec2(-1, 0))) {
    int keycode = 0;
    if (ParseKeycode(slot.press_keycode, &keycode)) {
      slot.client.sendPress(keycode);
      slot.press_error.clear();
    } else {
      slot.press_error = "Keycode must be a non-negative integer.";
    }
  }
  ImGui::EndDisabled();
  if (!slot.press_error.empty()) {
    ImGui::TextWrapped("Press error: %s", slot.press_error.c_str());
  }
  if (!slot.update_status.empty()) {
    ImGui::TextWrapped("%s", slot.update_status.c_str());
  }
  if (!slot.update_error.empty()) {
    ImGui::TextWrapped("Update error: %s", slot.update_error.c_str());
  }

  ImGui::Separator();
  ImGui::Text("Upload");
  ImGui::InputText("Local File", &slot.upload_local_path);
  ImGui::InputText("Remote Path", &slot.upload_remote_path);
  const bool has_upload_paths =
      !TrimCopy(slot.upload_local_path).empty() && !TrimCopy(slot.upload_remote_path).empty();
  ImGui::BeginDisabled(!is_connected || !has_upload_paths);
  if (ImGui::Button("Upload File", ImVec2(-1, 0))) {
    if (TrimCopy(slot.upload_local_path).empty() || TrimCopy(slot.upload_remote_path).empty()) {
      slot.upload_error = "Provide both local and remote paths.";
    } else {
      slot.client.sendUpload(slot.upload_local_path, slot.upload_remote_path);
      slot.upload_error.clear();
    }
  }
  ImGui::EndDisabled();
  if (!slot.upload_error.empty()) {
    ImGui::TextWrapped("Upload error: %s", slot.upload_error.c_str());
  }

  ImGui::Separator();
  ImGui::BeginDisabled(status == ClientStatus::Disconnected);
  if (ImGui::Button("Disconnect", ImVec2(-1, 0))) {
    slot.client.disconnect();
  }
  ImGui::EndDisabled();
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("client_right", ImVec2(0, 0), true);
  if (ImGui::BeginTabBar("results_tabs")) {
    if (ImGui::BeginTabItem("Status")) {
      ImGui::Text("Status: %s", slot.client.statusLabel().c_str());
      const std::string error = slot.client.lastError();
      if (!error.empty()) {
        ImGui::TextWrapped("Last error: %s", error.c_str());
      }

      int64_t version = 0;
      std::string version_status;
      const bool has_version = slot.client.getVersionInfo(&version, &version_status);
      if (has_version) {
        ImGui::Text("Server Version: %lld", static_cast<long long>(version));
      } else {
        ImGui::TextDisabled("Server Version: unknown");
      }
      if (!version_status.empty()) {
        ImGui::TextWrapped("Version status: %s", version_status.c_str());
      }

      if (!settings.error.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("Settings error: %s", settings.error.c_str());
      }
      if (!slot.screencap_view.last_error.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("Preview error: %s", slot.screencap_view.last_error.c_str());
      }
      ImGui::EndTabItem();
    }

    if (slot.file_browser.visible) {
      ImGuiTabItemFlags flags = 0;
      if (slot.file_browser.pending_select) {
        flags = ImGuiTabItemFlags_SetSelected;
      }
      if (ImGui::BeginTabItem("Files", &slot.file_browser.visible, flags)) {
        DrawFileBrowser(slot.client, slot.file_browser, is_connected);
        ImGui::EndTabItem();
      }
      if (!slot.file_browser.visible) {
        slot.file_browser.save_popup_open = false;
      }
      if (slot.file_browser.pending_select) {
        slot.file_browser.pending_select = false;
      }
    }

    int preview_select = slot.file_browser.preview_pending_select;
    for (size_t i = 0; i < slot.file_browser.preview_tabs.size();) {
      auto& tab = slot.file_browser.preview_tabs[i];
      ImGuiTabItemFlags tab_flags = 0;
      if (preview_select >= 0 && static_cast<size_t>(preview_select) == i) {
        tab_flags = ImGuiTabItemFlags_SetSelected;
      }
      if (ImGui::BeginTabItem(tab.title.c_str(), &tab.open, tab_flags)) {
        if (!tab.error.empty()) {
          ImGui::TextWrapped("Preview error: %s", tab.error.c_str());
        }
        if (tab.texture && tab.width > 0 && tab.height > 0) {
          ImVec2 avail = ImGui::GetContentRegionAvail();
          const float scale_x = avail.x / static_cast<float>(tab.width);
          const float scale_y = avail.y / static_cast<float>(tab.height);
          float scale = std::min(scale_x, scale_y);
          if (scale <= 0.0f) {
            scale = 1.0f;
          }
          ImVec2 size(tab.width * scale, tab.height * scale);
          ImGui::Image(reinterpret_cast<ImTextureID>(tab.texture), size);
        }
        ImGui::EndTabItem();
      }
      if (!tab.open) {
        if (tab.texture) {
          SDL_DestroyTexture(tab.texture);
          tab.texture = nullptr;
        }
        slot.file_browser.preview_tabs.erase(
            slot.file_browser.preview_tabs.begin() + static_cast<long>(i));
        if (slot.file_browser.preview_pending_select == static_cast<int>(i)) {
          slot.file_browser.preview_pending_select = -1;
        } else if (slot.file_browser.preview_pending_select > static_cast<int>(i)) {
          slot.file_browser.preview_pending_select--;
        }
        if (preview_select > static_cast<int>(i)) {
          preview_select--;
        }
        continue;
      }
      ++i;
    }
    if (preview_select >= 0) {
      slot.file_browser.preview_pending_select = -1;
    }

    int select_index = slot.screencap_view.pending_select;
    for (size_t i = 0; i < slot.screencap_view.tabs.size();) {
      auto& tab = slot.screencap_view.tabs[i];
      ImGuiTabItemFlags flags = 0;
      if (select_index >= 0 && static_cast<size_t>(select_index) == i) {
        flags = ImGuiTabItemFlags_SetSelected;
      }
      if (ImGui::BeginTabItem(tab.title.c_str(), &tab.open, flags)) {
        ImGui::BeginDisabled(tab.png.empty());
        if (ImGui::Button("Save Screencap", ImVec2(-1, 0))) {
          std::string saved_path;
          std::string save_error;
          if (SavePngToFile(tab.png, tab.capture_id, &saved_path, &save_error)) {
            tab.saved_path = saved_path;
            tab.save_error.clear();
          } else {
            tab.save_error = save_error;
          }
        }
        ImGui::EndDisabled();
        if (!tab.saved_path.empty()) {
          ImGui::TextWrapped("Saved to: %s", tab.saved_path.c_str());
          if (ImGui::Button("Copy Path", ImVec2(-1, 0))) {
            ImGui::SetClipboardText(tab.saved_path.c_str());
          }
        }
        if (!tab.save_error.empty()) {
          ImGui::TextWrapped("Save error: %s", tab.save_error.c_str());
        }
        if (tab.texture && tab.width > 0 && tab.height > 0) {
          ImGui::Spacing();
          ImVec2 avail = ImGui::GetContentRegionAvail();
          const float scale_x = avail.x / static_cast<float>(tab.width);
          const float scale_y = avail.y / static_cast<float>(tab.height);
          float scale = std::min(scale_x, scale_y);
          if (scale <= 0.0f) {
            scale = 1.0f;
          }
          ImVec2 size(tab.width * scale, tab.height * scale);
          ImGui::Image(reinterpret_cast<ImTextureID>(tab.texture), size);
        }
        ImGui::EndTabItem();
      }
      if (!tab.open) {
        if (tab.texture) {
          SDL_DestroyTexture(tab.texture);
          tab.texture = nullptr;
        }
        slot.screencap_view.tabs.erase(
            slot.screencap_view.tabs.begin() + static_cast<long>(i));
        if (slot.screencap_view.pending_select == static_cast<int>(i)) {
          slot.screencap_view.pending_select = -1;
        } else if (slot.screencap_view.pending_select > static_cast<int>(i)) {
          slot.screencap_view.pending_select--;
        }
        if (select_index > static_cast<int>(i)) {
          select_index--;
        }
        continue;
      }
      ++i;
    }
    ImGui::EndTabBar();
    if (select_index >= 0) {
      slot.screencap_view.pending_select = -1;
    }
  }
  ImGui::EndChild();

  ImGui::EndChild();
  ImGui::PopID();
  return false;
}

static bool DrawConnectPopup(ClientSlot& slot, int slot_index, bool* open_popup) {
  bool settings_changed = false;
  AdbState& adb_state = slot.adb_state;
  const std::string popup_id = "Connect (Client " + std::to_string(slot_index + 1) +
      ")###connect_popup_" + std::to_string(slot_index);
  if (open_popup && *open_popup) {
    ImGui::OpenPopup(popup_id.c_str());
    *open_popup = false;
    adb_state.needs_refresh = true;
    adb_state.needs_forward_check = true;
    slot.connect_tab_pending = true;
  }

  bool keep_open = true;
  if (ImGui::BeginPopupModal(popup_id.c_str(),
                             &keep_open,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    const ClientStatus status = slot.client.status();
    const bool is_connected = status == ClientStatus::Connected;
    const bool is_connecting = status == ClientStatus::Connecting;

    if (adb_state.remote_port.empty()) {
      adb_state.remote_port = "1234";
    }
    if (adb_state.local_port.empty()) {
      int port = 0;
      std::string port_error;
      if (FindOpenPort(&port, &port_error)) {
        adb_state.local_port = std::to_string(port);
      } else if (!port_error.empty()) {
        adb_state.error = port_error;
      }
    }

    if (ImGui::BeginTabBar("connect_tabs")) {
      ImGuiTabItemFlags manual_flags =
          (slot.connect_tab_pending && slot.connect_tab == 0)
              ? ImGuiTabItemFlags_SetSelected
              : 0;
      if (ImGui::BeginTabItem("Manual", nullptr, manual_flags)) {
        const bool tab_clicked = ImGui::IsItemClicked();
        if (!slot.connect_tab_pending || slot.connect_tab == 0 || tab_clicked) {
          slot.connect_tab = 0;
          slot.connect_tab_pending = false;
          settings_changed = true;
        }
        settings_changed |= ImGui::InputText("Host", &slot.config.host);
        settings_changed |= ImGui::InputText("Port", &slot.config.port);
        settings_changed |= ImGui::InputText("Username", &slot.config.username);
        settings_changed |= ImGui::InputText("Password",
                                             &slot.config.password,
                                             ImGuiInputTextFlags_Password);

        const bool has_target = !slot.config.host.empty() && !slot.config.port.empty();
        const bool has_credentials = !slot.config.username.empty() && !slot.config.password.empty();
        const bool can_connect = has_target && has_credentials && !is_connected && !is_connecting;

        if (!has_target) {
          ImGui::TextDisabled("Enter host and port to connect.");
        }
        if (!has_credentials) {
          ImGui::TextDisabled("Username and password are required.");
        }

        ImGui::BeginDisabled(!can_connect);
        if (ImGui::Button("Connect", ImVec2(120, 0))) {
          slot.client.connect(slot.config);
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        ImGui::EndTabItem();
      }

      ImGuiTabItemFlags adb_flags =
          (slot.connect_tab_pending && slot.connect_tab == 1)
              ? ImGuiTabItemFlags_SetSelected
              : 0;
      if (ImGui::BeginTabItem("ADB", nullptr, adb_flags)) {
        const bool tab_clicked = ImGui::IsItemClicked();
        if (!slot.connect_tab_pending || slot.connect_tab == 1 || tab_clicked) {
          slot.connect_tab = 1;
          slot.connect_tab_pending = false;
          settings_changed = true;
        }
        if (adb_state.needs_refresh) {
          RefreshAdbDevices(&adb_state);
          adb_state.needs_refresh = false;
        }

        if (!adb_state.devices.empty()) {
          std::string current_label = "Select device";
          if (adb_state.selected >= 0 &&
              adb_state.selected < static_cast<int>(adb_state.devices.size())) {
            const auto& device = adb_state.devices[adb_state.selected];
            current_label = device.serial + " (" + device.state + ")";
          }
          if (ImGui::BeginCombo("Device", current_label.c_str())) {
            for (int i = 0; i < static_cast<int>(adb_state.devices.size()); ++i) {
              const auto& device = adb_state.devices[i];
              const std::string label = device.serial + " (" + device.state + ")";
              const bool is_selected = (adb_state.selected == i);
              if (ImGui::Selectable(label.c_str(), is_selected)) {
                adb_state.selected = i;
                adb_state.needs_forward_check = true;
              }
              if (is_selected) {
                ImGui::SetItemDefaultFocus();
              }
            }
            ImGui::EndCombo();
          }
        } else {
          ImGui::TextDisabled("No adb devices detected.");
        }
        if (ImGui::Button("Refresh Devices")) {
          RefreshAdbDevices(&adb_state);
        }

        ImGui::Separator();
        if (ImGui::InputText("Device Port", &adb_state.remote_port)) {
          settings_changed = true;
          adb_state.needs_forward_check = true;
        }
        ImGui::InputText("Local Port", &adb_state.local_port, ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("Pick Port")) {
          int port = 0;
          std::string port_error;
          if (FindOpenPort(&port, &port_error)) {
            adb_state.local_port = std::to_string(port);
            adb_state.existing_forward_local.clear();
            adb_state.needs_forward_check = true;
          } else {
            adb_state.error = port_error.empty() ? "Failed to select port." : port_error;
          }
        }

        settings_changed |= ImGui::InputText("Username", &slot.config.username);
        settings_changed |= ImGui::InputText("Password",
                                             &slot.config.password,
                                             ImGuiInputTextFlags_Password);

        int local_port_value = 0;
        int remote_port_value = 0;
        const bool has_device = adb_state.selected >= 0 &&
            adb_state.selected < static_cast<int>(adb_state.devices.size());
        const bool local_ok = ParsePort(adb_state.local_port, &local_port_value);
        const bool remote_ok = ParsePort(adb_state.remote_port, &remote_port_value);
        const bool has_credentials = !slot.config.username.empty() && !slot.config.password.empty();
        const bool can_forward = has_device && local_ok && remote_ok && !is_connecting;
        const bool can_connect = has_credentials && !is_connected && !is_connecting;
        const bool can_start_server = has_device && !is_connecting;
        const bool can_stop_server = has_device && !is_connecting;

        if (has_device && remote_ok) {
          const std::string serial = adb_state.devices[adb_state.selected].serial;
          if (adb_state.needs_forward_check ||
              serial != adb_state.last_forward_serial ||
              adb_state.remote_port != adb_state.last_forward_remote) {
            adb_state.last_forward_serial = serial;
            adb_state.last_forward_remote = adb_state.remote_port;
            adb_state.existing_forward_local.clear();
            std::string forward_error;
            if (FindExistingForward(serial,
                                    remote_port_value,
                                    &adb_state.existing_forward_local,
                                    &forward_error)) {
              adb_state.local_port = adb_state.existing_forward_local;
              adb_state.status = "Existing forward found on localhost:" + adb_state.local_port;
            } else if (!forward_error.empty()) {
              adb_state.error = forward_error;
            }
            adb_state.needs_forward_check = false;
          }
        }

        ImGui::BeginDisabled(!can_start_server);
        if (ImGui::Button("Start Server", ImVec2(140, 0))) {
          adb_state.error.clear();
          adb_state.status.clear();
          StartAdbServerAsync(&adb_state, adb_state.devices[adb_state.selected]);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!can_stop_server);
        if (ImGui::Button("Stop Server", ImVec2(140, 0))) {
          adb_state.error.clear();
          adb_state.status.clear();
          if (!RunAdbShellOnce(adb_state.devices[adb_state.selected],
                               "/data/local/tmp/rmi stop",
                               &adb_state.error)) {
            adb_state.status.clear();
          } else {
            adb_state.status = "Stop command sent.";
          }
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(!can_forward || !can_connect);
        if (ImGui::Button("Connect", ImVec2(140, 0))) {
          adb_state.error.clear();
          adb_state.status.clear();
          if (!RunAdbForward(adb_state.devices[adb_state.selected],
                             local_port_value,
                             remote_port_value,
                             &adb_state.error)) {
            adb_state.status.clear();
          } else {
            slot.config.host = "127.0.0.1";
            slot.config.port = std::to_string(local_port_value);
            slot.client.connect(slot.config);
            ImGui::CloseCurrentPopup();
          }
        }
        ImGui::EndDisabled();

        if (!adb_state.status.empty()) {
          ImGui::TextWrapped("%s", adb_state.status.c_str());
        }
        if (!adb_state.error.empty()) {
          ImGui::TextWrapped("ADB error: %s", adb_state.error.c_str());
        }

        std::string start_output;
        bool start_running = false;
        bool start_finished = false;
        int start_exit_code = 0;
        {
          std::lock_guard<std::mutex> lock(adb_state.start_mutex);
          start_output = adb_state.start_output;
          start_running = adb_state.start_running;
          start_finished = adb_state.start_finished;
          start_exit_code = adb_state.start_exit_code;
        }
        if (!start_output.empty() || start_running || start_finished) {
          ImGui::Separator();
          ImGui::TextDisabled("Start server output:");
          ImGui::BeginChild("start_server_output", ImVec2(0, 140), true);
          ImGui::TextUnformatted(start_output.c_str());
          if (start_running) {
            const float scroll_y = ImGui::GetScrollY();
            const float scroll_max = ImGui::GetScrollMaxY();
            if (scroll_y >= scroll_max - 5.0f) {
              ImGui::SetScrollHereY(1.0f);
            }
          }
          ImGui::EndChild();
          if (!start_running && start_finished) {
            ImGui::TextDisabled("Start server exit status: %d", start_exit_code);
          }
        }

        ImGui::EndTabItem();
      }

      ImGui::EndTabBar();
    }

    ImGui::EndPopup();
  }

  return settings_changed;
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }

  SDL_Window* window = SDL_CreateWindow(
      "Remote Management Interface",
      SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED,
      1000,
      700,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window) {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_Renderer* renderer = SDL_CreateRenderer(
      window,
      -1,
      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();

  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
#if defined(RMI_IMGUI_SDLRENDERER2)
  ImGui_ImplSDLRenderer2_Init(renderer);
#else
  ImGui_ImplSDLRenderer_Init(renderer);
#endif

  std::vector<std::unique_ptr<ClientSlot>> slots;
  slots.push_back(std::make_unique<ClientSlot>());
  int active_slot = 0;
  SettingsState settings;
  settings.path = SettingsPath().string();
  LoadSettings(&slots[0]->config, &slots[0]->connect_tab, &settings.error);

  bool running = true;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type == SDL_QUIT) {
        running = false;
      }
      if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
          event.window.windowID == SDL_GetWindowID(window)) {
        running = false;
      }
    }

    for (size_t i = 0; i < slots.size(); ++i) {
      ClientSlot& slot = *slots[i];
      UpdateScreencapTexture(renderer, slot.client, &slot.screencap_view);
      UpdateFilePreviewTextures(renderer, slot.file_browser);
      if (slot.reconnect_pending && SDL_GetTicks64() >= slot.reconnect_at_ticks) {
        const ClientStatus reconnect_status = slot.client.status();
        if (reconnect_status == ClientStatus::Disconnected ||
            reconnect_status == ClientStatus::Error) {
          slot.client.connect(slot.config);
          slot.reconnect_pending = false;
        }
      }
    }

#if defined(RMI_IMGUI_SDLRENDERER2)
    ImGui_ImplSDLRenderer2_NewFrame();
#else
    ImGui_ImplSDLRenderer_NewFrame();
#endif
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Remote Management Interface",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_MenuBar);
    if (ImGui::BeginMenuBar()) {
      if (ImGui::BeginMenu("Connection")) {
        ClientSlot& menu_slot = *slots[active_slot];
        const ClientStatus status = menu_slot.client.status();
        const bool is_connected = status == ClientStatus::Connected;
        if (ImGui::MenuItem("Connect...", nullptr, false, !is_connected)) {
          menu_slot.show_connect_popup = true;
        }
        if (ImGui::MenuItem("Disconnect", nullptr, false, is_connected)) {
          menu_slot.client.disconnect();
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenuBar();
    }

    ImGui::Text("Clients");
    ImGui::SameLine();
    if (ImGui::Button("Add Client")) {
      slots.push_back(std::make_unique<ClientSlot>());
      active_slot = static_cast<int>(slots.size() - 1);
    }
    if (ImGui::BeginTabBar("client_tabs")) {
      for (size_t i = 0; i < slots.size();) {
        std::string label = "Client " + std::to_string(i + 1) +
            "###client_tab_" + std::to_string(i);
        bool open = true;
        const bool allow_close = slots.size() > 1;
        if (ImGui::BeginTabItem(label.c_str(), allow_close ? &open : nullptr)) {
          active_slot = static_cast<int>(i);
          ImGui::EndTabItem();
        }
        if (allow_close && !open) {
          slots.erase(slots.begin() + static_cast<long>(i));
          if (active_slot > static_cast<int>(i)) {
            active_slot--;
          } else if (active_slot == static_cast<int>(i)) {
            if (i >= slots.size()) {
              active_slot = static_cast<int>(slots.size()) - 1;
            }
          }
          continue;
        }
        ++i;
      }
      ImGui::EndTabBar();
    }

    ClientSlot& active = *slots[active_slot];
    ImGui::Text("Connect and send AUTH/SCREENCAP/RESTART/QUIT/PRESS/VERSION/UPLOAD framed commands.");
    ImGui::Separator();

    const ClientStatus status = active.client.status();
    const bool is_connected = status == ClientStatus::Connected;
    const bool is_connecting = status == ClientStatus::Connecting;
    if (!is_connected) {
      ImGui::BeginDisabled(is_connecting);
      if (ImGui::Button("Connect", ImVec2(140, 0))) {
        active.show_connect_popup = true;
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (is_connecting) {
        ImGui::TextDisabled("Connecting...");
      } else {
        ImGui::TextDisabled("Not connected");
      }
      ImGui::Separator();
    }

    bool settings_changed = false;
    bool settings_changed_primary = false;
    settings_changed = DrawConnectPopup(active, active_slot, &active.show_connect_popup);
    settings_changed |= DrawClientPanel(active_slot, active, settings);
    if (active_slot == 0 && settings_changed) {
      settings_changed_primary = true;
    }
    if (settings_changed_primary) {
      settings.dirty = true;
      settings.last_change_ticks = SDL_GetTicks64();
    }

    ImGui::End();

    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    SDL_RenderClear(renderer);
#if defined(RMI_IMGUI_SDLRENDERER2)
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
#else
    ImGui_ImplSDLRenderer_RenderDrawData(ImGui::GetDrawData());
#endif
    SDL_RenderPresent(renderer);

    if (settings.dirty && SDL_GetTicks64() - settings.last_change_ticks > 500) {
      if (SaveSettings(slots[0]->config, slots[0]->connect_tab, &settings.error)) {
        settings.dirty = false;
        settings.error.clear();
      }
    }
  }

  for (auto& slot_ptr : slots) {
    ClientSlot& slot = *slot_ptr;
    for (auto& tab : slot.screencap_view.tabs) {
      if (tab.texture) {
        SDL_DestroyTexture(tab.texture);
        tab.texture = nullptr;
      }
    }
    for (auto& tab : slot.file_browser.preview_tabs) {
      if (tab.texture) {
        SDL_DestroyTexture(tab.texture);
        tab.texture = nullptr;
      }
    }
  }

#if defined(RMI_IMGUI_SDLRENDERER2)
  ImGui_ImplSDLRenderer2_Shutdown();
#else
  ImGui_ImplSDLRenderer_Shutdown();
#endif
  if (settings.dirty) {
    SaveSettings(slots[0]->config, slots[0]->connect_tab, &settings.error);
  }

  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
