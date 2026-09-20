#pragma once

#include <cstdint>
#include <cstddef>

namespace headless_tty {

// Frame layout: [1 byte type][4 byte little endian length][payload]
enum class FrameType : uint8_t {
    Data   = 1,  // both directions, raw terminal bytes
    Resize = 2,  // client to server, payload is uint16 cols then uint16 rows
};

constexpr size_t FRAME_HEADER_SIZE = 5;
constexpr size_t MAX_FRAME_PAYLOAD = 1024 * 1024;

}
