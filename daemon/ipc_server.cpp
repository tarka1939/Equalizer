/*
 * ipc_server.cpp
 *
 * JSON-line IPC server for the eq-daemon.
 *
 * On POSIX systems (Linux/Mac) we use a Unix-domain socket.
 * On Windows the implementation would use a Named Pipe instead
 * (compile with -DPLATFORM_WINDOWS) — a stub is provided here.
 *
 * JSON parsing is kept minimal (no external library dependency):
 * we handle the small fixed command set ourselves.
 */
#include "ipc_server.h"
#include "eq_state.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#ifndef _WIN32
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#  include <poll.h>
#  include <fcntl.h>
#endif

namespace eq {

namespace {

// ── Minimal JSON helpers ─────────────────────────────────────────────────────

/// Extract a string value for key in a flat JSON object.
/// Returns "" if not found.
std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    ++pos;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

/// Extract numeric value for key (returns NaN on failure).
float JsonGetFloat(const std::string& json, const std::string& key, float def = 0.f) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    try { return std::stof(json.substr(pos)); } catch (...) { return def; }
}

/// Extract bool value for key.
bool JsonGetBool(const std::string& json, const std::string& key, bool def = false) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos < json.size()) {
        if (json[pos] == 't') return true;
        if (json[pos] == 'f') return false;
    }
    return def;
}

/// Extract an array of 10 floats from "gains_db": [...].
bool JsonGetGains(const std::string& json, std::array<float, kBandCount>& out) {
    auto pos = json.find("\"gains_db\"");
    if (pos == std::string::npos) return false;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    for (int i = 0; i < kBandCount; ++i) {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ',')) ++pos;
        if (pos >= json.size() || json[pos] == ']') return false;
        try {
            size_t consumed;
            out[i] = std::stof(json.substr(pos), &consumed);
            pos += consumed;
        } catch (...) { return false; }
    }
    return true;
}

/// Build a simple JSON-line response.
std::string OkResponse() { return "{\"ok\":true}\n"; }
std::string ErrResponse(const std::string& msg) {
    return "{\"ok\":false,\"error\":\"" + msg + "\"}\n";
}

} // anonymous namespace

// ── IpcServer ────────────────────────────────────────────────────────────────

IpcServer::IpcServer(EqState* state) : m_state(state) {}

IpcServer::~IpcServer() { Stop(); }

bool IpcServer::Start() {
#ifdef _WIN32
    std::cerr << "[IPC] Windows named pipe not yet implemented.\n";
    return false;
#else
    // Remove stale socket file.
    ::unlink(kSocketPath);

    m_listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_listenFd < 0) {
        std::perror("[IPC] socket");
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

    if (::bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("[IPC] bind");
        ::close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    if (::listen(m_listenFd, 8) < 0) {
        std::perror("[IPC] listen");
        ::close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    // Make listen socket non-blocking so Stop() can unblock accept().
    int flags = ::fcntl(m_listenFd, F_GETFL, 0);
    ::fcntl(m_listenFd, F_SETFL, flags | O_NONBLOCK);

    m_running = true;
    m_thread  = std::thread(&IpcServer::AcceptLoop, this);
    std::cout << "[IPC] Listening on " << kSocketPath << "\n";
    return true;
#endif
}

void IpcServer::Stop() {
    m_running = false;
#ifndef _WIN32
    if (m_listenFd >= 0) {
        ::close(m_listenFd);
        m_listenFd = -1;
        ::unlink(kSocketPath);
    }
#endif
    if (m_thread.joinable())
        m_thread.join();
}

void IpcServer::AcceptLoop() {
#ifndef _WIN32
    while (m_running) {
        // Use poll() so we can check m_running periodically.
        pollfd pfd{ m_listenFd, POLLIN, 0 };
        int r = ::poll(&pfd, 1, 200 /*ms*/);
        if (r <= 0) continue;

        int clientFd = ::accept(m_listenFd, nullptr, nullptr);
        if (clientFd < 0) continue;

        // Handle client in a detached thread.
        std::thread([this, clientFd]() { HandleClient(clientFd); }).detach();
    }
#endif
}

void IpcServer::HandleClient(int fd) {
#ifndef _WIN32
    std::string buf;
    buf.reserve(256);
    char tmp[256];

    while (true) {
        ssize_t n = ::read(fd, tmp, sizeof(tmp) - 1);
        if (n <= 0) break;
        tmp[n] = '\0';
        buf += tmp;

        // Process complete newline-delimited lines.
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            std::string resp = ProcessCommand(line);
            ::write(fd, resp.c_str(), resp.size());
        }
    }
    ::close(fd);
#endif
}

std::string IpcServer::ProcessCommand(const std::string& line) {
    if (line.empty()) return ErrResponse("empty command");

    std::string cmd = JsonGetString(line, "cmd");

    if (cmd == "set_bands") {
        std::array<float, kBandCount> gains{};
        if (!JsonGetGains(line, gains))
            return ErrResponse("invalid gains_db array");
        m_state->SetGains(gains);
        m_state->current_gains = gains;
        std::cout << "[IPC] set_bands applied\n";
        return OkResponse();
    }

    if (cmd == "set_preamp") {
        float db = JsonGetFloat(line, "gain_db");
        m_state->preamp_db.store(db, std::memory_order_relaxed);
        std::cout << "[IPC] set_preamp " << db << " dB\n";
        return OkResponse();
    }

    if (cmd == "set_enabled") {
        bool en = JsonGetBool(line, "enabled", true);
        m_state->enabled.store(en, std::memory_order_relaxed);
        std::cout << "[IPC] set_enabled " << (en ? "true" : "false") << "\n";
        return OkResponse();
    }

    if (cmd == "get_state") {
        std::ostringstream ss;
        ss << "{\"gains_db\":[";
        for (int i = 0; i < kBandCount; ++i) {
            ss << m_state->current_gains[i];
            if (i + 1 < kBandCount) ss << ",";
        }
        ss << "],\"preamp_db\":" << m_state->preamp_db.load()
           << ",\"enabled\":"   << (m_state->enabled.load() ? "true" : "false")
           << ",\"sample_rate\":" << m_state->sample_rate
           << ",\"channels\":"  << m_state->channels
           << "}\n";
        return ss.str();
    }

    if (cmd == "load_preset") {
        // The path is readable; actual file parsing happens here.
        std::string path = JsonGetString(line, "path");
        if (path.empty())
            return ErrResponse("missing path");
        // TODO: read file, parse JSON preset, call SetGains().
        std::cout << "[IPC] load_preset: " << path << " (not yet implemented)\n";
        return ErrResponse("load_preset not yet implemented");
    }

    return ErrResponse("unknown command: " + cmd);
}

} // namespace eq
