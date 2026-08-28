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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <sys/un.h>
#  include <unistd.h>
#  include <poll.h>
#  include <fcntl.h>
#  include <errno.h>
#endif

namespace eq {

namespace {

// ── Minimal JSON helpers ─────────────────────────────────────────────────────

/// Escape a string for embedding in a JSON string literal. Error responses
/// echo back the client's own `cmd` value; splicing that in raw let a command
/// name containing a quote or backslash produce malformed JSON (or inject
/// extra fields) into the reply, breaking the client's parser.
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

/// Parse a float starting at `pos`, without copying the entire remainder of
/// the line. std::stof takes a std::string, so the old
/// `std::stof(json.substr(pos))` allocated and copied every trailing byte on
/// each of the (up to kMaxFirTaps) numbers in a set_fir array, making parsing
/// quadratic in the request size.
bool ParseFloatAt(const std::string& json, size_t pos, float& out, size_t& consumed) {
    // A numeric literal can't be longer than this; bounding the copy keeps
    // the parse linear overall.
    static constexpr size_t kMaxNumberChars = 64;
    const size_t len = std::min(kMaxNumberChars, json.size() - pos);
    try {
        size_t used = 0;
        out = std::stof(json.substr(pos, len), &used);
        if (used == 0) return false;
        consumed = used;
        return true;
    } catch (...) {
        return false;
    }
}

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

/// Extract numeric value for key. Returns `def` on failure.
float JsonGetFloat(const std::string& json, const std::string& key, float def = 0.f) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    float value = def;
    size_t consumed = 0;
    if (!ParseFloatAt(json, pos, value, consumed)) return def;
    return value;
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
        size_t consumed = 0;
        if (!ParseFloatAt(json, pos, out[i], consumed)) return false;
        pos += consumed;
    }
    return true;
}

/// Extract a variable-length array of floats for `key`: [...]. Unlike
/// JsonGetGains (fixed at kBandCount), this reads however many comma-
/// separated numbers are present between the brackets -- used for
/// set_fir's "taps" array, whose length varies per impulse response.
/// Returns false on a missing key, missing '[', or an unterminated/
/// malformed array; an empty array ("[]") is valid and yields out.empty().
/// Stops and fails past `maxCount` entries so a hostile array can't force
/// unbounded growth of `out` before the caller gets a chance to reject it.
bool JsonGetFloatArray(const std::string& json, const std::string& key,
                       std::vector<float>& out, size_t maxCount) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    out.clear();
    while (true) {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ',' || json[pos] == '\t')) ++pos;
        if (pos >= json.size()) return false;  // unterminated array
        if (json[pos] == ']') { ++pos; break; }
        if (out.size() >= maxCount) {
            // Report what we have; the caller's length check rejects it.
            out.push_back(0.0f);
            return true;
        }
        float value = 0.0f;
        size_t consumed = 0;
        if (!ParseFloatAt(json, pos, value, consumed)) return false;
        out.push_back(value);
        pos += consumed;
    }
    return true;
}

/// True if every element is finite. A NaN or Inf arriving over IPC otherwise
/// reaches Biquad/OverlapAdd and permanently poisons the filter state -- see
/// DSP/Biquad.h's note on SetCoefficients. Those layers reject non-finite
/// values themselves as a backstop; rejecting here means the client gets a
/// real error instead of a silently-ignored command.
template <typename Container>
bool AllFinite(const Container& values) {
    for (float v : values) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}

/// Build a simple JSON-line response.
std::string OkResponse() { return "{\"ok\":true}\n"; }
std::string ErrResponse(const std::string& msg) {
    return "{\"ok\":false,\"error\":\"" + JsonEscape(msg) + "\"}\n";
}

#ifndef _WIN32
/// Write the whole buffer, looping over partial writes. Returns false if the
/// peer went away or stopped reading. ::write() on a socket is permitted to
/// accept fewer bytes than requested; the previous code only checked for < 0
/// and so could silently truncate a response (get_state's is the longest).
///
/// EAGAIN here means the SO_SNDTIMEO set in HandleClient() expired, i.e. the
/// peer isn't draining its receive buffer. That is treated as a dead
/// connection rather than retried: blocking here would hold up Stop(), which
/// now joins client threads instead of detaching them.
bool WriteAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, data + sent, len - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}
#endif

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

    // Restrict the socket to the owning user. A Unix socket created under the
    // process umask is commonly world-writable, which would let any local
    // account drive this daemon's audio processing.
    if (::chmod(kSocketPath, S_IRUSR | S_IWUSR) < 0)
        std::perror("[IPC] chmod (continuing)");

    if (::listen(m_listenFd, 8) < 0) {
        std::perror("[IPC] listen");
        ::close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    // Make listen socket non-blocking so accept() never stalls the loop.
    int flags = ::fcntl(m_listenFd, F_GETFL, 0);
    ::fcntl(m_listenFd, F_SETFL, flags | O_NONBLOCK);

    // Self-pipe so Stop() can wake the accept loop immediately instead of
    // waiting out a poll timeout -- and, more importantly, without closing
    // the listening descriptor while the accept thread is still using it.
    if (::pipe(m_wakePipe) < 0) {
        std::perror("[IPC] pipe");
        ::close(m_listenFd);
        m_listenFd = -1;
        return false;
    }
    ::fcntl(m_wakePipe[0], F_SETFL, O_NONBLOCK);

    m_running = true;
    m_thread  = std::thread(&IpcServer::AcceptLoop, this);
    std::cout << "[IPC] Listening on " << kSocketPath << "\n";
    return true;
#endif
}

void IpcServer::Stop() {
    if (!m_running.exchange(false) && !m_thread.joinable())
        return;  // never started, or already stopped

#ifndef _WIN32
    // 1. Wake the accept loop. Note we do NOT close m_listenFd yet: the
    //    accept thread is still polling it, and closing a descriptor another
    //    thread is using is a use-after-free in disguise (the number can be
    //    handed straight back out by a concurrent open()).
    if (m_wakePipe[1] >= 0) {
        const char byte = 1;
        ssize_t ignored = ::write(m_wakePipe[1], &byte, 1);
        (void)ignored;
    }
#endif

    // 2. Join the accept thread, so nothing new is spawned from here on.
    if (m_thread.joinable())
        m_thread.join();

    // 3. Join every live client thread. These used to be detached, which let
    //    them keep using `this` and `m_state` after the server (and, via
    //    ~IpcServer, often the whole daemon) was gone.
    std::vector<std::unique_ptr<ClientThread>> clients;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        clients.swap(m_clients);
    }
    for (auto& c : clients) {
        if (c->thread.joinable())
            c->thread.join();
    }

#ifndef _WIN32
    // 4. Only now is it safe to release the descriptors.
    if (m_listenFd >= 0) {
        ::close(m_listenFd);
        m_listenFd = -1;
        ::unlink(kSocketPath);
    }
    for (int& fd : m_wakePipe) {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
#endif
}

void IpcServer::ReapFinishedClients() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto it = m_clients.begin(); it != m_clients.end();) {
        if ((*it)->done.load(std::memory_order_acquire)) {
            if ((*it)->thread.joinable())
                (*it)->thread.join();
            it = m_clients.erase(it);
        } else {
            ++it;
        }
    }
}

void IpcServer::AcceptLoop() {
#ifndef _WIN32
    while (m_running.load()) {
        pollfd pfds[2];
        pfds[0] = pollfd{ m_listenFd,   POLLIN, 0 };
        pfds[1] = pollfd{ m_wakePipe[0], POLLIN, 0 };

        int r = ::poll(pfds, 2, 200 /*ms*/);
        if (r < 0 && errno == EINTR) continue;

        // Stop() wrote to the wake pipe.
        if (r > 0 && (pfds[1].revents & POLLIN))
            break;

        ReapFinishedClients();

        if (r <= 0 || !(pfds[0].revents & POLLIN)) continue;

        int clientFd = ::accept(m_listenFd, nullptr, nullptr);
        if (clientFd < 0) continue;

        if (!m_running.load()) { ::close(clientFd); break; }

        auto entry = std::make_unique<ClientThread>();
        ClientThread* raw = entry.get();
        raw->thread = std::thread([this, clientFd, raw]() {
            HandleClient(clientFd);
            raw->done.store(true, std::memory_order_release);
        });
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_clients.push_back(std::move(entry));
        }
    }
#endif
}

void IpcServer::HandleClient(int fd) {
#ifndef _WIN32
    // Bounded blocking reads so this thread notices Stop() and can be joined
    // instead of sitting in read() forever on an idle connection. The send
    // timeout matters for the same reason: a peer that connects and then never
    // reads would otherwise wedge this thread inside write() and, with it,
    // Stop()'s join.
    timeval tv{};
    tv.tv_sec  = 0;
    tv.tv_usec = 200 * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string buf;
    buf.reserve(256);
    char tmp[256];

    while (m_running.load()) {
        ssize_t n = ::read(fd, tmp, sizeof(tmp) - 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;  // read timeout -- re-check m_running
            break;
        }
        if (n == 0) break;  // peer closed
        buf.append(tmp, static_cast<size_t>(n));

        // Process complete newline-delimited lines.
        size_t pos;
        bool disconnect = false;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            std::string resp = ProcessCommand(line);
            if (!WriteAll(fd, resp.c_str(), resp.size())) {
                disconnect = true;  // client likely gone
                break;
            }
        }
        if (disconnect) break;

        // A client that never sends a newline would otherwise grow `buf`
        // without limit -- an unauthenticated local memory-exhaustion DoS.
        if (buf.size() > kMaxLineBytes) {
            std::string resp = ErrResponse("command line too long");
            WriteAll(fd, resp.c_str(), resp.size());
            break;
        }
    }
    ::close(fd);
#else
    (void)fd;
#endif
}

std::string IpcServer::ProcessCommand(const std::string& line) {
    if (line.empty()) return ErrResponse("empty command");

    std::string cmd = JsonGetString(line, "cmd");

    if (cmd == "set_bands") {
        std::array<float, kBandCount> gains{};
        if (!JsonGetGains(line, gains))
            return ErrResponse("invalid gains_db array");
        if (!AllFinite(gains))
            return ErrResponse("gains_db contains a non-finite value");
        m_state->SetGains(gains);
        m_state->current_gains = gains;
        std::cout << "[IPC] set_bands applied\n";
        return OkResponse();
    }

    if (cmd == "set_preamp") {
        float db = JsonGetFloat(line, "gain_db");
        if (!std::isfinite(db))
            return ErrResponse("gain_db must be a finite number");
        // Bound the range before it becomes a linear multiplier: the audio
        // path computes pow(10, db/20), and a large enough db yields +Inf,
        // which turns any silent sample into Inf*0 == NaN.
        if (db < kMinPreampDb || db > kMaxPreampDb) {
            return ErrResponse("gain_db out of range ["
                               + std::to_string(static_cast<int>(kMinPreampDb)) + ", "
                               + std::to_string(static_cast<int>(kMaxPreampDb)) + "] dB");
        }
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

    if (cmd == "set_fir") {
        std::vector<float> taps;
        if (!JsonGetFloatArray(line, "taps", taps, EqState::kMaxFirTaps + 1))
            return ErrResponse("invalid taps array");
        if (taps.empty())
            return ErrResponse("taps array must not be empty (use clear_fir to disable FIR)");
        if (taps.size() > EqState::kMaxFirTaps)
            return ErrResponse("taps array exceeds max length (" + std::to_string(EqState::kMaxFirTaps) + ")");
        if (!AllFinite(taps))
            return ErrResponse("taps contains a non-finite value");
        m_state->SetImpulseResponse(taps.data(), static_cast<uint32_t>(taps.size()));
        m_state->current_fir_length = static_cast<uint32_t>(taps.size());
        std::cout << "[IPC] set_fir applied (" << taps.size() << " taps)\n";
        return OkResponse();
    }

    if (cmd == "clear_fir") {
        m_state->ClearImpulseResponse();
        m_state->current_fir_length = 0;
        std::cout << "[IPC] clear_fir applied\n";
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
           << ",\"sample_rate\":" << m_state->sample_rate.load()
           << ",\"channels\":"  << m_state->channels.load()
           << ",\"fir_length\":" << m_state->current_fir_length
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
