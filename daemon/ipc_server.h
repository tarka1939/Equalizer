#pragma once
#include "eq_state.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eq {

/// JSON-line IPC server.
///
/// - Linux/Mac: Unix domain socket at /tmp/eq-daemon.sock
/// - Windows:   Named pipe \\.\pipe\eq-daemon
///
/// Each connection is handled in a separate thread.  Commands are
/// processed synchronously; the response is written before the next
/// command is read.
///
/// Shutdown ordering matters here and is not obvious: Stop() signals the
/// accept loop, shuts down (but does not close) the listening socket to
/// break it out of poll(), joins the accept thread AND every live client
/// thread, and only then closes the descriptor. Closing first -- as this
/// used to -- races the accept thread still using that fd, and can hand it
/// a descriptor number that has since been recycled by another thread.
/// Client threads are joined rather than detached because they hold raw
/// `this`/`m_state` pointers and would otherwise outlive both.
class IpcServer {
public:
    explicit IpcServer(EqState* state);
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    /// Start listening.  Spawns an accept loop in a background thread.
    bool Start();

    /// Signal the accept loop to stop, join all worker threads, and release
    /// the socket. Safe to call more than once.
    void Stop();

    static constexpr const char* kSocketPath  = "/tmp/eq-daemon.sock";
    static constexpr const char* kPipeName    = R"(\\.\pipe\eq-daemon)";

    /// Longest single command line accepted from a client. A client that
    /// never sends a newline would otherwise grow the per-connection buffer
    /// without bound. Sized to comfortably hold the largest legitimate
    /// command -- a set_fir with kMaxFirTaps float literals -- plus slack.
    static constexpr size_t kMaxLineBytes = 1u << 20;  // 1 MiB

    /// Accepted range for set_preamp, in dB. Bounded because the audio path
    /// turns this into pow(10, db/20): a large enough value overflows to
    /// +Inf, and Inf * 0 (a silent sample) is NaN, which then propagates.
    static constexpr float kMinPreampDb = -60.f;
    static constexpr float kMaxPreampDb =  20.f;

private:
    void AcceptLoop();
    void HandleClient(int fd);
    std::string ProcessCommand(const std::string& line);

    /// Join any client threads that have finished, so a long-lived server
    /// doesn't accumulate thread objects for connections that already ended.
    void ReapFinishedClients();

    EqState*    m_state;
    int         m_listenFd = -1;
    /// Self-pipe used to wake AcceptLoop()'s poll() from Stop() without
    /// closing m_listenFd out from under it. [0] = read end, [1] = write end.
    int         m_wakePipe[2] = { -1, -1 };
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    struct ClientThread {
        std::thread       thread;
        std::atomic<bool> done{false};
    };
    std::mutex m_clientsMutex;
    std::vector<std::unique_ptr<ClientThread>> m_clients;
};

} // namespace eq
