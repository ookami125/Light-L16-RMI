# RMI Client (ImGui)

ImGui-based remote management interface that connects to a TCP server, frames all
messages with a 4-byte length prefix, sends `AUTH <username> <password>` on connect,
and supports `SCREENCAP`, `RESTART`, `QUIT`, `PRESS <keycode>`, `PRESS_INPUT <keycode>`,
`VERSION`, and `UPLOAD` commands.

The server may emit `HEARTBEAT` frames while idle; the client acknowledges them with `OK`.
The client also sends a `HEARTBEAT` frame every 5 seconds while connected and expects `OK`.

Connection settings are persisted to `client_settings.ini` in the current working directory.

## Dependencies

- SDL2
- Dear ImGui

Place Dear ImGui under `external/imgui` (so `external/imgui/imgui.h` exists), point
CMake at your ImGui checkout with `-DIMGUI_DIR=/path/to/imgui`, or let CMake fetch
ImGui automatically when `IMGUI_DIR` is not set and the fallback path is missing.

## Build

```
cmake -S . -B build
cmake --build build
```

## Run

```
./build/rmi_client
```

Screencap responses are saved as PNG files under `captures/` in the current working
directory, and the GUI previews the most recent capture inline.
