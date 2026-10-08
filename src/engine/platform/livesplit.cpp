#include "engine/platform/livesplit.h"

#include <chrono>
#include <cstring>

#include "engine/core/log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pt {
namespace {

constexpr uintptr_t kNoSocket = ~uintptr_t(0);
constexpr uint64_t kRetryMs = 3000;
constexpr uint64_t kConnectTimeoutMs = 3000;
constexpr size_t kQueueLimit = 256;

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

#ifdef _WIN32
bool Startup() {
    static const bool ok = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
bool WouldBlock() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
}
void CloseSocket(uintptr_t s) { closesocket(static_cast<SOCKET>(s)); }
#else
bool Startup() { return true; }
bool WouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EALREADY; }
void CloseSocket(uintptr_t s) { close(static_cast<int>(s)); }
#endif

}

LiveSplitClient::~LiveSplitClient() {
    Close();
}

void LiveSplitClient::Configure(bool enabled, std::string host, int port) {
    if (host == "localhost" || host.empty()) host = "127.0.0.1";
    const bool changed = enabled != enabled_ || host != host_ || port != port_;
    enabled_ = enabled;
    host_ = std::move(host);
    port_ = port > 0 && port < 65536 ? port : 16834;
    if (!changed) return;
    Close();
    queue_.clear();
    state_ = enabled_ ? State::Idle : State::Off;
    retry_at_ms_ = 0;
    logged_failure_ = false;
    if (enabled_) LogInfo("livesplit: server client on, {}:{}", host_, port_);
}

void LiveSplitClient::Send(std::string_view command) {
    if (!enabled_) return;
    if (queue_.size() >= kQueueLimit) queue_.pop_front();
    queue_.emplace_back(std::string(command) + "\r\n");
}

void LiveSplitClient::Close() {
    if (socket_ != kNoSocket) {
        CloseSocket(socket_);
        socket_ = kNoSocket;
    }
    if (state_ != State::Off) state_ = State::Idle;
}

void LiveSplitClient::StartConnect() {
    retry_at_ms_ = NowMs() + kRetryMs;
    if (!Startup()) return;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port_));
    if (inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) {
        if (!logged_failure_) LogWarn("livesplit: '{}' is not an IPv4 address", host_);
        logged_failure_ = true;
        return;
    }
#ifdef _WIN32
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return;
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    socket_ = static_cast<uintptr_t>(s);
#else
    const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return;
#ifdef __APPLE__
    const int no_sigpipe = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    socket_ = static_cast<uintptr_t>(s);
#endif
    if (connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        state_ = State::Connected;
        LogInfo("livesplit: connected to {}:{}", host_, port_);
        logged_failure_ = false;
        return;
    }
    if (!WouldBlock()) {
        Close();
        return;
    }
    state_ = State::Connecting;
    connect_started_ms_ = NowMs();
}

void LiveSplitClient::Poll() {
    if (!enabled_) return;
    if (state_ == State::Idle && NowMs() >= retry_at_ms_) StartConnect();
    if (state_ == State::Connecting) {
        fd_set write_set;
        fd_set error_set;
        FD_ZERO(&write_set);
        FD_ZERO(&error_set);
#ifdef _WIN32
        FD_SET(static_cast<SOCKET>(socket_), &write_set);
        FD_SET(static_cast<SOCKET>(socket_), &error_set);
        const int nfds = 0;
#else
        FD_SET(static_cast<int>(socket_), &write_set);
        FD_SET(static_cast<int>(socket_), &error_set);
        const int nfds = static_cast<int>(socket_) + 1;
#endif
        timeval zero{};
        const int ready = select(nfds, nullptr, &write_set, &error_set, &zero);
        if (ready > 0) {
            int error = 0;
#ifdef _WIN32
            int length = sizeof(error);
            getsockopt(static_cast<SOCKET>(socket_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
            const bool failed = FD_ISSET(static_cast<SOCKET>(socket_), &error_set) || error != 0;
#else
            socklen_t length = sizeof(error);
            getsockopt(static_cast<int>(socket_), SOL_SOCKET, SO_ERROR, &error, &length);
            const bool failed = FD_ISSET(static_cast<int>(socket_), &error_set) || error != 0;
#endif
            if (failed) {
                if (!logged_failure_) LogInfo("livesplit: no server at {}:{} (Control > Start TCP Server in LiveSplit), retrying", host_, port_);
                logged_failure_ = true;
                Close();
            } else {
                state_ = State::Connected;
                logged_failure_ = false;
                LogInfo("livesplit: connected to {}:{}", host_, port_);
            }
        } else if (ready < 0 || NowMs() - connect_started_ms_ > kConnectTimeoutMs) {
            Close();
        }
    }
    while (state_ == State::Connected && !queue_.empty()) {
        std::string& front = queue_.front();
#ifdef _WIN32
        const int sent = send(static_cast<SOCKET>(socket_), front.data(), static_cast<int>(front.size()), 0);
#elif defined(__APPLE__)
        const int sent = static_cast<int>(send(static_cast<int>(socket_), front.data(), front.size(), 0));
#else
        const int sent = static_cast<int>(send(static_cast<int>(socket_), front.data(), front.size(), MSG_NOSIGNAL));
#endif
        if (sent < 0) {
            if (!WouldBlock()) {
                LogInfo("livesplit: connection lost; reconnecting");
                Close();
                retry_at_ms_ = NowMs() + kRetryMs;
            }
            break;
        }
        if (static_cast<size_t>(sent) < front.size()) {
            front.erase(0, static_cast<size_t>(sent));
            break;
        }
        queue_.pop_front();
    }
}

}
