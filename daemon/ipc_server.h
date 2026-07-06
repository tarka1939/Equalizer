#pragma once
#include "eq_state.h"
#include <functional>
#include <string>
#include <thread>

namespace eq {

/// JSON-line IPC server.
///
/// - Linux/Mac: Unix domain socket at /tmp/eq-daemon.sock
/// - Windows:   Named pipe \\.\pipe\eq-daemon
///
/// Each connection is handled in a separate thread.  Commands are
/// processed synchronously; the response is written before the next
/// command is read.
class IpcServer {
public:
    explicit IpcServer(EqState* state);
    ~IpcServer();

    /// Start listening.  Spawns an accept loop in a background thread.
    bool Start();

    /// Signal the accept loop to stop and join.
    void Stop();

    static constexpr const char* kSocketPath  = "/tmp/eq-daemon.sock";
    static constexpr const char* kPipeName    = R"(\\.\pipe\eq-daemon)";

private:
    void AcceptLoop();
    void HandleClient(int fd);
    std::string ProcessCommand(const std::string& line);

    EqState*    m_state;
    int         m_listenFd = -1;
    bool        m_running  = false;
    std::thread m_thread;
};

} // namespace eq
