#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace pt {

class LiveSplitClient {
public:
    LiveSplitClient() = default;
    ~LiveSplitClient();
    LiveSplitClient(const LiveSplitClient&) = delete;
    LiveSplitClient& operator=(const LiveSplitClient&) = delete;

    void Configure(bool enabled, std::string host, int port);
    bool Enabled() const { return enabled_; }
    bool Connected() const { return state_ == State::Connected; }
    void Send(std::string_view command);
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
