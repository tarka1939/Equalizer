/*
 * main.cpp  —  eq-daemon entry point
 *
 * Usage:
 *   eq-daemon [--no-audio]
 *
 *   --no-audio   Start IPC server only (for testing without audio hardware).
 */
#include "audio_backend.h"
#include "eq_state.h"
#include "ipc_server.h"

// audio_backend.h only *declares* CreateAudioBackend(); each platform supplies
// the definition. On Linux that comes from pipewire_backend.cpp, a real
// translation unit in the target, so nothing extra is needed here. The WASAPI
// stub instead defines it `inline` in its header, and an inline function is
// only emitted in a translation unit that odr-uses it -- so the TU that calls
// it (this one) has to see the definition or the link fails with "unresolved
// external symbol eq::CreateAudioBackend". Listing wasapi_backend.h in
// DAEMON_SOURCES does not do that; CMake does not compile headers.
#ifdef BACKEND_WASAPI
#  include "wasapi_backend.h"
#endif

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>

// ── Signal handling ───────────────────────────────────────────────────────────
static volatile sig_atomic_t g_quit = 0;
static void OnSignal(int) { g_quit = 1; }

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    bool no_audio = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--no-audio") == 0) no_audio = true;

    std::signal(SIGINT,  OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::cout << "eq-daemon v1.0 starting\n";

    // Shared state (owned here, borrowed by IPC + audio backend).
    eq::EqState state;

    // ── IPC server ─────────────────────────────────────────────────────────
    eq::IpcServer ipc(&state);
    if (!ipc.Start()) {
        std::cerr << "Failed to start IPC server — aborting.\n";
        return 1;
    }

    // ── Audio backend ──────────────────────────────────────────────────────
    if (!no_audio) {
        auto backend = eq::CreateAudioBackend(&state);
        std::cout << "Audio backend: " << backend->Name() << "\n";

        // Open() blocks (PipeWire main loop) so run it on a thread.
        std::thread audio_thread([&]() {
            if (!backend->Open()) {
                std::cerr << "[Audio] Backend failed to open.\n";
                g_quit = 1;
            }
        });

        // Wait for quit signal.
        while (!g_quit)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        backend->Close();
        if (audio_thread.joinable()) audio_thread.join();
    } else {
        std::cout << "[main] Running in --no-audio mode (IPC only).\n";
        while (!g_quit)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ipc.Stop();
    std::cout << "eq-daemon stopped.\n";
    return 0;
}
