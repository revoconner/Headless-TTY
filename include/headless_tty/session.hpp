#pragma once

#include "pty.hpp"
#include "protocol.hpp"
#include "ring.hpp"

#include <deque>
#include <vector>

namespace headless_tty {

// Must stay under MAX_FRAME_PAYLOAD, the whole ring travels in one Replay frame
constexpr size_t REPLAY_CAPACITY = 512 * 1024;
constexpr size_t MAX_QUEUED_BYTES = 8 * 1024 * 1024;
constexpr DWORD CLIENT_WRITE_TIMEOUT_MS = 5000;

// Serves one PTY over a named pipe so a client can attach, detach and attach again. One client at a time.
class Session {
public:
    Session(HeadlessTTY& tty, const std::wstring& name, TerminalSize size, bool wait_for_client);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Creates the pipe. Fails when the name is already taken, so call it before spawning anything.
    bool open();

    // Starts accepting clients. Call after the PTY has started.
    void serve();

    // PTY output callback. Never blocks on the client.
    void on_output(const uint8_t* data, size_t length);

    // The child has exited. Returns once the exit code is delivered, or at once when nobody is attached and wait_for_client is off.
    void finish(int exit_code);

    void stop();
    const std::string& get_last_error() const { return m_last_error; }

private:
    void serve_loop();
    bool wait_for_client();
    void serve_client();
    bool handle_frames(std::vector<uint8_t>& buf);
    bool handle_frame(FrameType type, const uint8_t* payload, size_t len);
    void attach(TerminalSize size);
    void apply_size(TerminalSize size);
    void enqueue(FrameType type, const void* payload, size_t len);
    bool flush_queue();
    bool write_frame(const std::vector<uint8_t>& frame);
    bool stopping() const;

    HeadlessTTY& m_tty;
    std::wstring m_name;
    TerminalSize m_size;
    bool m_wait;

    HANDLE m_hPipe = INVALID_HANDLE_VALUE;
    HANDLE m_hStop = NULL;
    HANDLE m_hReadEvent = NULL;
    HANDLE m_hWriteEvent = NULL;
    HANDLE m_hQueueEvent = NULL;
    std::thread m_thread;
    std::string m_last_error;

    // Guards everything below. Output, attach and finish all go through it, so a replay and the live stream never overlap or leave a gap.
    std::mutex m_mutex;
    RingBuffer m_ring{ REPLAY_CAPACITY };
    std::vector<uint8_t> m_held;
    std::deque<std::vector<uint8_t>> m_queue;
    size_t m_queued_bytes = 0;
    std::atomic<bool> m_attached{ false };
    bool m_dropped = false;
    bool m_finished = false;
    bool m_exit_delivered = false;
    int32_t m_exit_code = 0;
};

}
