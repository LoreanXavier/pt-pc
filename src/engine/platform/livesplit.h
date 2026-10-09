#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace pt {

// A client of LiveSplit's Server component (LiveSplit: Control > Start TCP Server, port 16834 by default): one text command per
// line ("starttimer", "split", "reset", "setgametime 0:01:02.345"). Opt-in (pt.ini [extras] livesplit). Never blocks: the
// connect is non-blocking, retried every few seconds, and commands wait in a short queue until the connection is up.
class LiveSplitClient {
public:
    LiveSplitClient() = default;
    ~LiveSplitClient();
    LiveSplitClient(const LiveSplitClient&) = delete;
    LiveSplitClient& operator=(const LiveSplitClient&) = delete;

    void Configure(bool enabled, std::string host, int port);
    bool Enabled() const { return enabled_; }
    bool Connected() const { return state_ == State::Connected; }
    // queue a command (without its line end)
    void Send(std::string_view command);
    // connect, flush the queue; called once per frame
    void Poll();

private:
    enum class State { Off, Idle, Connecting, Connected };
    void Close();
    void StartConnect();

    bool enabled_ = false;
    std::string host_ = "127.0.0.1";
    int port_ = 16834;
    State state_ = State::Off;
    uintptr_t socket_ = ~uintptr_t(0);
    uint64_t retry_at_ms_ = 0;
    uint64_t connect_started_ms_ = 0;
    std::deque<std::string> queue_;
    bool logged_failure_ = false;
};

}
