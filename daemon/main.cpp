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
