#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

namespace headless_tty {

// Fixed size byte ring holding recent PTY output.
// A client attaching late replays this to redraw the screen.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : m_buf(capacity), m_cap(capacity) {}

    void push(const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (len >= m_cap) {
            // Chunk alone overflows the ring, keep only its tail
            memcpy(m_buf.data(), data + (len - m_cap), m_cap);
            m_head = 0;
            m_size = m_cap;
            return;
        }

        for (size_t i = 0; i < len; ++i) {
            m_buf[(m_head + m_size) % m_cap] = data[i];
            if (m_size < m_cap) {
                m_size++;
            } else {
                m_head = (m_head + 1) % m_cap;
            }
        }
    }

    std::vector<uint8_t> snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<uint8_t> out;
        out.reserve(m_size);
        for (size_t i = 0; i < m_size; ++i) {
            out.push_back(m_buf[(m_head + i) % m_cap]);
        }
        return out;
    }

private:
    std::vector<uint8_t> m_buf;
    size_t m_cap;
    size_t m_head = 0;
    size_t m_size = 0;
    mutable std::mutex m_mutex;
};

}
