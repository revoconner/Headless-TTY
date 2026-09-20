#pragma once

#include <cstdint>
#include <cstddef>

namespace headless_tty {

// Frame layout: [1 byte type][4 byte little endian length][payload]
enum class FrameType : uint8_t {
    Data   = 1,  // both directions, raw terminal bytes
    Resize = 2,  // client to server, payload is uint16 cols then uint16 rows
    Hello  = 3,  // client to server, always the first frame, payload is uint8 version, uint16 cols, uint16 rows
    Detach = 4,  // client to server, no payload, the session stays alive
    Exit   = 5,  // server to client, the child exited, payload is its int32 exit code
    Replay = 6,  // server to client, recent output for a late attach, the client resets its screen before drawing it
};

constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr size_t FRAME_HEADER_SIZE = 5;
constexpr size_t MAX_FRAME_PAYLOAD = 1024 * 1024;

// Full pipe path is this prefix followed by the session name
constexpr wchar_t PIPE_PREFIX[] = L"\\\\.\\pipe\\htty-";

// Event named with this prefix and the session name. htty -s creates it and the server sets it once the command is running.
constexpr wchar_t READY_EVENT_PREFIX[] = L"Local\\htty-ready-";

// Set inside a session so a client can refuse to attach to the session it runs in
constexpr wchar_t SESSION_ENV_VAR[] = L"HTTY_SESSION";

}
