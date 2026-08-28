/*
 * test_ipc_server.cpp — Integration test for eq::IpcServer's JSON-line
 * protocol, exercised over a real Unix domain socket (the exact code path
 * production clients use), rather than reaching into private methods.
 *
 * No external test framework (see DSP/tests/test_biquad.cpp).
 *
 * POSIX-only (uses sys/socket.h directly), matching ipc_server.cpp's own
 * POSIX-only implementation.
 *
 * Build:
 *   g++ -std=c++17 -Wall -Wextra -O2 -I.. -c ../ipc_server.cpp -o ipc_server.o
 *   g++ -std=c++17 -Wall -Wextra -O2 -I.. -c test_ipc_server.cpp -o test_ipc_server.o
 *   g++ ipc_server.o test_ipc_server.o -lpthread -o test_ipc_server
 *   ./test_ipc_server
 */
#include "../ipc_server.h"
#include "../eq_state.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_currentTest = "";

void Check(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s: %s (%s:%d)\n", g_currentTest, expr, file, line);
    }
}

#define CHECK(cond) Check((cond), #cond, __FILE__, __LINE__)
#define RUN_TEST(fn) do { g_currentTest = #fn; fn(); } while (0)

// ── Minimal blocking Unix-socket client for the JSON-line protocol ──────────

class TestClient {
public:
    bool Connect(const char* path) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

        // Retry briefly: the server's accept loop polls every 200ms, and the
        // socket file may not exist yet the instant Start() returns on a
        // loaded machine.
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    ~TestClient() { if (fd_ >= 0) ::close(fd_); }

    int RawFd() const { return fd_; }

    // Raw write with no newline appended -- used to exercise the server's
    // maximum-line-length guard.
    bool WriteRaw(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            ssize_t n = ::write(fd_, data.data() + sent, data.size() - sent);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // Send one JSON command line and block for one JSON response line.
    // Returns "" on timeout/error.
    std::string SendCommand(const std::string& json, int timeoutMs = 2000) {
        std::string line = json + "\n";
        if (::write(fd_, line.c_str(), line.size()) < 0) return "";

        timeval tv{};
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::string buf;
        char tmp[512];
        while (buf.find('\n') == std::string::npos) {
            ssize_t n = ::read(fd_, tmp, sizeof(tmp));
            if (n <= 0) break;
            buf.append(tmp, static_cast<size_t>(n));
        }
        auto pos = buf.find('\n');
        return pos == std::string::npos ? buf : buf.substr(0, pos);
    }

private:
    int fd_ = -1;
};

// ── Tests ─────────────────────────────────────────────────────────────────────

void IpcServer_SetBandsValidUpdatesStateAndAcks() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    std::string resp = client.SendCommand(
        R"({"cmd":"set_bands","gains_db":[1,2,3,4,5,6,7,8,9,10]})");
    CHECK(resp.find("\"ok\":true") != std::string::npos);

    std::string state_resp = client.SendCommand(R"({"cmd":"get_state"})");
    CHECK(state_resp.find("[1,2,3,4,5,6,7,8,9,10]") != std::string::npos);

    server.Stop();
}

void IpcServer_SetBandsWrongLengthIsRejected() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    std::string resp = client.SendCommand(
        R"({"cmd":"set_bands","gains_db":[1,2,3]})");  // only 3, need 10
    CHECK(resp.find("\"ok\":false") != std::string::npos);
    CHECK(resp.find("invalid gains_db array") != std::string::npos);

    server.Stop();
}

void IpcServer_SetPreampUpdatesState() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    std::string resp = client.SendCommand(R"({"cmd":"set_preamp","gain_db":-4.5})");
    CHECK(resp.find("\"ok\":true") != std::string::npos);

    std::string state_resp = client.SendCommand(R"({"cmd":"get_state"})");
    CHECK(state_resp.find("-4.5") != std::string::npos);

    server.Stop();
}

void IpcServer_SetEnabledUpdatesState() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    std::string resp = client.SendCommand(R"({"cmd":"set_enabled","enabled":false})");
    CHECK(resp.find("\"ok\":true") != std::string::npos);

    std::string state_resp = client.SendCommand(R"({"cmd":"get_state"})");
    CHECK(state_resp.find("\"enabled\":false") != std::string::npos);

    server.Stop();
}

void IpcServer_GetStateReportsDefaults() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    std::string resp = client.SendCommand(R"({"cmd":"get_state"})");
    CHECK(resp.find("\"sample_rate\":48000") != std::string::npos);
    CHECK(resp.find("\"channels\":2") != std::string::npos);
    CHECK(resp.find("\"enabled\":true") != std::string::npos);

    server.Stop();
}

void IpcServer_GetStateReportsFirLengthDefault() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    std::string resp = client.SendCommand(R"({"cmd":"get_state"})");
    CHECK(resp.find("\"fir_length\":0") != std::string::npos);

    server.Stop();
}

void IpcServer_SetFirValidUpdatesStateAndAcks() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    std::string resp = client.SendCommand(
        R"({"cmd":"set_fir","taps":[0.5,0.25,-0.1,0.05]})");
    CHECK(resp.find("\"ok\":true") != std::string::npos);

    std::string state_resp = client.SendCommand(R"({"cmd":"get_state"})");
    CHECK(state_resp.find("\"fir_length\":4") != std::string::npos);

    // The RT side should see it queued via ConsumePendingIr, matching
    // set_bands's ConsumePending contract (daemon/tests/test_eq_state.cpp
    // covers ConsumePendingIr's own once-then-false semantics directly;
    // this test only checks the IPC command actually reaches EqState).
    CHECK(state.pending_ir_dirty.load() == true);
    CHECK(state.pending_ir_length == 4u);

    server.Stop();
}

void IpcServer_SetFirEmptyArrayIsRejected() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    std::string resp = client.SendCommand(R"({"cmd":"set_fir","taps":[]})");
    CHECK(resp.find("\"ok\":false") != std::string::npos);
    CHECK(resp.find("must not be empty") != std::string::npos);

    server.Stop();
}

void IpcServer_SetFirOversizedArrayIsRejected() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    std::string taps_json = "[";
    for (uint32_t i = 0; i < eq::EqState::kMaxFirTaps + 1; ++i) {
        if (i) taps_json += ",";
        taps_json += "0.01";
    }
    taps_json += "]";

    std::string resp = client.SendCommand(
        R"({"cmd":"set_fir","taps":)" + taps_json + "}");
    CHECK(resp.find("\"ok\":false") != std::string::npos);
    CHECK(resp.find("exceeds max length") != std::string::npos);

    // Rejected requests must not partially apply.
    CHECK(state.pending_ir_dirty.load() == false);

    server.Stop();
}

void IpcServer_SetFirInvalidArrayIsRejected() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    // Missing "taps" key entirely.
    std::string resp = client.SendCommand(R"({"cmd":"set_fir"})");
    CHECK(resp.find("\"ok\":false") != std::string::npos);
    CHECK(resp.find("invalid taps array") != std::string::npos);

    server.Stop();
}

void IpcServer_ClearFirResetsLengthAndAcks() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    CHECK(client.SendCommand(
        R"({"cmd":"set_fir","taps":[1,2,3]})").find("\"ok\":true") != std::string::npos);

    std::string resp = client.SendCommand(R"({"cmd":"clear_fir"})");
    CHECK(resp.find("\"ok\":true") != std::string::npos);

    std::string state_resp = client.SendCommand(R"({"cmd":"get_state"})");
    CHECK(state_resp.find("\"fir_length\":0") != std::string::npos);

    CHECK(state.pending_ir_dirty.load() == true);
    CHECK(state.pending_ir_length == 0u);

    server.Stop();
}

void IpcServer_LoadPresetIsNotYetImplemented() {
    // Documents current behaviour: load_preset is a recognised command but
    // always errors. If this test starts failing because someone implements
    // it, that's good news -- update the assertion accordingly.
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    std::string resp = client.SendCommand(R"({"cmd":"load_preset","path":"/tmp/x.json"})");
    CHECK(resp.find("\"ok\":false") != std::string::npos);
    CHECK(resp.find("not yet implemented") != std::string::npos);

    server.Stop();
}

void IpcServer_UnknownCommandIsRejected() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    std::string resp = client.SendCommand(R"({"cmd":"reticulate_splines"})");
    CHECK(resp.find("\"ok\":false") != std::string::npos);
    CHECK(resp.find("unknown command") != std::string::npos);

    server.Stop();
}

void IpcServer_EmptyLineIsRejected() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    std::string resp = client.SendCommand("");  // just sends "\n"
    CHECK(resp.find("\"ok\":false") != std::string::npos);
    CHECK(resp.find("empty command") != std::string::npos);

    server.Stop();
}

// ── Input validation ─────────────────────────────────────────────────────────

void IpcServer_SetBandsRejectsNonFiniteGain() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    // std::stof happily parses "nan"/"inf". A NaN gain reaches Biquad, lands
    // in the filter history, and every subsequent output stays NaN forever --
    // no later valid curve can undo it. Must be refused at the door.
    std::string resp = client.SendCommand(
        R"({"cmd":"set_bands","gains_db":[1,2,nan,4,5,6,7,8,9,10]})");
    CHECK(resp.find("\"ok\":false") != std::string::npos);
    CHECK(resp.find("non-finite") != std::string::npos);

    std::string resp2 = client.SendCommand(
        R"({"cmd":"set_bands","gains_db":[1,2,3,4,5,6,7,8,9,inf]})");
    CHECK(resp2.find("\"ok\":false") != std::string::npos);

    // Rejected requests must not partially apply.
    CHECK(state.pending_dirty.load() == false);

    server.Stop();
}

void IpcServer_SetFirRejectsNonFiniteTap() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    std::string resp = client.SendCommand(
        R"({"cmd":"set_fir","taps":[0.5,nan,0.25]})");
    CHECK(resp.find("\"ok\":false") != std::string::npos);
    CHECK(resp.find("non-finite") != std::string::npos);
    CHECK(state.pending_ir_dirty.load() == false);

    server.Stop();
}

void IpcServer_SetPreampRejectsNonFiniteAndOutOfRange() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    CHECK(client.SendCommand(R"({"cmd":"set_preamp","gain_db":nan})")
              .find("\"ok\":false") != std::string::npos);

    // Out of range: pow(10, db/20) overflows to +Inf well before this, and
    // Inf * 0 (a silent sample) is NaN.
    CHECK(client.SendCommand(R"({"cmd":"set_preamp","gain_db":1000})")
              .find("out of range") != std::string::npos);
    CHECK(client.SendCommand(R"({"cmd":"set_preamp","gain_db":-1000})")
              .find("out of range") != std::string::npos);

    // Preamp must be unchanged after all those rejections.
    CHECK(state.preamp_db.load() == 0.0f);

    // A value inside the range still works.
    CHECK(client.SendCommand(R"({"cmd":"set_preamp","gain_db":-6})")
              .find("\"ok\":true") != std::string::npos);
    CHECK(state.preamp_db.load() == -6.0f);

    server.Stop();
}

// Walk a JSON string literal starting at the opening quote, honouring
// backslash escapes. Returns the index of the terminating quote, or npos if
// the literal is unterminated (which is exactly the corruption we're testing
// for). Deliberately hand-rolled: the server has no JSON library either, and
// the point is to confirm its output is well formed by an independent reader.
size_t ScanJsonString(const std::string& s, size_t openQuote) {
    if (openQuote >= s.size() || s[openQuote] != '"') return std::string::npos;
    for (size_t i = openQuote + 1; i < s.size(); ++i) {
        if (s[i] == '\\') { ++i; continue; }   // skip the escaped char
        if (s[i] == '"') return i;
    }
    return std::string::npos;
}

void IpcServer_ErrorResponseEscapesEchoedCommand() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    // The unknown-command error echoes the client's own `cmd` value back. A
    // value ending in a backslash used to be spliced in raw, producing
    //     {"ok":false,"error":"unknown command: weird\"}
    // where the backslash escapes what should be the closing quote -- the
    // reply is no longer parseable and the client's parser breaks. The value
    // must come back escaped.
    std::string resp = client.SendCommand(R"({"cmd":"weird\\"})");
    CHECK(resp.find("\"ok\":false") != std::string::npos);

    const size_t errKey = resp.find("\"error\":");
    CHECK(errKey != std::string::npos);
    if (errKey != std::string::npos) {
        const size_t open  = resp.find('"', errKey + 8);
        const size_t close = ScanJsonString(resp, open);
        // The error string must terminate, and the object must close right
        // after it. Both fail on the unescaped form.
        CHECK(close != std::string::npos);
        if (close != std::string::npos)
            CHECK(close + 1 < resp.size() && resp[close + 1] == '}');
    }

    // Embedded quotes and control characters must survive the same way.
    std::string resp2 = client.SendCommand("{\"cmd\":\"a\tb\"}");
    CHECK(resp2.find("\"ok\":false") != std::string::npos);
    const size_t errKey2 = resp2.find("\"error\":");
    if (errKey2 != std::string::npos) {
        const size_t open2 = resp2.find('"', errKey2 + 8);
        CHECK(ScanJsonString(resp2, open2) != std::string::npos);
        // A literal tab inside a JSON string is invalid; it must be escaped.
        CHECK(resp2.find('\t') == std::string::npos);
    }

    server.Stop();
}

void IpcServer_OverlongLineDropsConnection() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient client;
    CHECK(client.Connect(eq::IpcServer::kSocketPath));

    // A client that never sends a newline used to grow the per-connection
    // buffer without bound -- an unauthenticated local memory-exhaustion DoS.
    // Send more than kMaxLineBytes with no terminator; the server must give up
    // on the connection rather than keep accumulating.
    const std::string junk(eq::IpcServer::kMaxLineBytes + 4096, 'A');
    client.WriteRaw(junk);   // may fail partway once the server closes -- fine

    // The connection is now dead: a well-formed command gets no state back.
    std::string resp = client.SendCommand(R"({"cmd":"get_state"})", 1000);
    CHECK(resp.find("\"sample_rate\"") == std::string::npos);

    // The server itself must still be healthy for new clients.
    TestClient fresh;
    CHECK(fresh.Connect(eq::IpcServer::kSocketPath));
    CHECK(fresh.SendCommand(R"({"cmd":"get_state"})").find("\"sample_rate\"") != std::string::npos);

    server.Stop();
}

// Stop() must be safe to call twice, and must not hang waiting on a client
// thread that is sitting idle on an open connection. Client threads used to be
// detached and could outlive the server, using `this` and `m_state` after both
// were gone; they are joined now, which only works because the read loop times
// out and re-checks the running flag.
void IpcServer_StopJoinsIdleClientsAndIsIdempotent() {
    eq::EqState state;
    eq::IpcServer server(&state);
    CHECK(server.Start());

    TestClient idle;
    CHECK(idle.Connect(eq::IpcServer::kSocketPath));
    CHECK(idle.SendCommand(R"({"cmd":"get_state"})").find("\"channels\"") != std::string::npos);

    // `idle` now just sits there with the connection open.
    server.Stop();
    server.Stop();  // second call must be a no-op, not a double-join/close

    CHECK(true);  // reaching here at all is the assertion: no hang, no crash
}

}  // namespace

int main() {
    RUN_TEST(IpcServer_SetBandsValidUpdatesStateAndAcks);
    RUN_TEST(IpcServer_SetBandsWrongLengthIsRejected);
    RUN_TEST(IpcServer_SetPreampUpdatesState);
    RUN_TEST(IpcServer_SetEnabledUpdatesState);
    RUN_TEST(IpcServer_GetStateReportsDefaults);
    RUN_TEST(IpcServer_GetStateReportsFirLengthDefault);
    RUN_TEST(IpcServer_SetFirValidUpdatesStateAndAcks);
    RUN_TEST(IpcServer_SetFirEmptyArrayIsRejected);
    RUN_TEST(IpcServer_SetFirOversizedArrayIsRejected);
    RUN_TEST(IpcServer_SetFirInvalidArrayIsRejected);
    RUN_TEST(IpcServer_ClearFirResetsLengthAndAcks);
    RUN_TEST(IpcServer_LoadPresetIsNotYetImplemented);
    RUN_TEST(IpcServer_UnknownCommandIsRejected);
    RUN_TEST(IpcServer_EmptyLineIsRejected);
    RUN_TEST(IpcServer_SetBandsRejectsNonFiniteGain);
    RUN_TEST(IpcServer_SetFirRejectsNonFiniteTap);
    RUN_TEST(IpcServer_SetPreampRejectsNonFiniteAndOutOfRange);
    RUN_TEST(IpcServer_ErrorResponseEscapesEchoedCommand);
    RUN_TEST(IpcServer_OverlongLineDropsConnection);
    RUN_TEST(IpcServer_StopJoinsIdleClientsAndIsIdempotent);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
