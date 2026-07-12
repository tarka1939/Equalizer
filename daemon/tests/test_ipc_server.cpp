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

}  // namespace

int main() {
    RUN_TEST(IpcServer_SetBandsValidUpdatesStateAndAcks);
    RUN_TEST(IpcServer_SetBandsWrongLengthIsRejected);
    RUN_TEST(IpcServer_SetPreampUpdatesState);
    RUN_TEST(IpcServer_SetEnabledUpdatesState);
    RUN_TEST(IpcServer_GetStateReportsDefaults);
    RUN_TEST(IpcServer_LoadPresetIsNotYetImplemented);
    RUN_TEST(IpcServer_UnknownCommandIsRejected);
    RUN_TEST(IpcServer_EmptyLineIsRejected);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
