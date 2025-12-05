#pragma once

#include <cstdint>
#include <array>
#include <vector>
#include <string>
#include <functional>

namespace headless_tty {

// Buffer sizes
constexpr size_t PTY_BUFFER_SIZE = 4096;
constexpr size_t INPUT_BUFFER_SIZE = 128;

// Terminal dimensions
struct TerminalSize {
    uint16_t cols = 120;
    uint16_t rows = 40;
};

// Configuration
struct Config {
    TerminalSize size = { 120, 40 };
    std::wstring command = L"cmd.exe";
    std::wstring args = L"";
    std::wstring working_dir = L"";
};

// Callback for PTY output
using OutputCallback = std::function<void(const uint8_t*, size_t)>;

}
