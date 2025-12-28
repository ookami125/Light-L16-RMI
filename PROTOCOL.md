# RMI Protocol

This document describes the TCP protocol used by the RMI server in `server/rmi.c`.

## Transport

- TCP, server listens on `0.0.0.0:<port>` (default `1234`).
- Single client at a time; the server handles one connection, then accepts the next.
- All messages are framed: a 4-byte `uint32_t` length prefix (network byte order),
  followed by exactly that many bytes of payload.
- Plaintext; no encryption or integrity checks.

## Authentication

- The server reads credentials from `/data/local/tmp/rmi.config` on startup.
- Clients must authenticate before commands are accepted.
- Request payload:
  - `AUTH <username> <password>`
- Response:
  - `OK` on success
  - `ERR auth required` on failure (up to 3 attempts)
  - `ERR auth failed` on the 3rd failure; connection is closed

## Commands

Commands are case-sensitive and sent as a single framed payload with no extra
whitespace.

### `QUIT`

Request payload:
- `QUIT`

Response:
- `OK`

Notes:
- The server closes the listener after acknowledging the request.

### `RESTART`

Request payload:
- `RESTART`

Response:
- `OK`

Notes:
- The server re-execs the same binary in root context after acknowledging the request.

### `VERSION`

Request payload:
- `VERSION`

Response:
- `VERSION <number>`

Errors:
- `ERR version` if the server cannot format the response

### `PRESS`

Request payload:
- `PRESS <keycode>`

Response:
- `OK`

Errors:
- `ERR press` if the key event fails or the keycode is invalid

### `PRESS_INPUT`

Request payload:
- `PRESS_INPUT <keycode>`

Response:
- `OK`

Errors:
- `ERR press` if the key event fails or the keycode is invalid

Notes:
- This variant uses `/system/bin/input keyevent` instead of writing to `/dev/input/eventX`.

### `OPEN`

Request payload:
- `OPEN <target>`

Response:
- `OK`

Errors:
- `ERR open` if the app cannot be launched

Notes:
- `<target>` may be a component (`package/.Activity`) or a package name.

### `UPLOAD`

Request payload:
- `UPLOAD <path> <size>`

Followed by:
- A single framed payload containing exactly `<size>` bytes of file content.

Response:
- `OK`

Errors:
- `ERR upload` if the path is invalid, the size is invalid, or the transfer fails

### `LIST`

Request payload:
- `LIST <path>`

Response:
- A single framed payload with one entry per line:
  - `D\t<name>` for directories
  - `F\t<name>\t<size>` for files (size in bytes)

Errors:
- `ERR list` if the path is invalid or listing fails

Notes:
- File/folder names must not contain tabs.

### `DOWNLOAD`

Request payload:
- `DOWNLOAD <path>`

Response:
- `OK` then a second framed payload containing the file bytes

Errors:
- `ERR download` if the path is invalid or the file cannot be read

### `DELETE`

Request payload:
- `DELETE <path>`

Response:
- `OK`

Errors:
- `ERR delete` if the path is invalid or removal fails

### `HEARTBEAT`

Request payload:
- `HEARTBEAT`

Response:
- `OK`

### `SCREENCAP`

Request payload:
- `SCREENCAP`

Response:
- Raw PNG bytes in a single framed response (length prefix + PNG data).

Errors:
- `ERR screencap` if the capture fails
- `ERR unknown command` for unsupported commands

## Heartbeats

- If the connection is idle, the server sends a `HEARTBEAT` frame about every 5 seconds.
- Clients may also send a `HEARTBEAT` command and receive `OK`.
- Heartbeats can arrive before or after commands; ignore them unless you need
  keepalive logic.

## Config File Formats

The server accepts any of these `rmi.config` layouts:

1. Key/value:
   - `username=USER`
   - `password=PASS`

2. Single-line:
   - `USER:PASS`
   - `USER PASS` (whitespace-separated)

3. Two lines:
   - `USER`
   - `PASS`
