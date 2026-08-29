cmake_minimum_required(VERSION 3.20)

# ──────────────────────────────────────────────────────────────────────────────
# IPC / state unit tests — deliberately independent of the platform backend
# branch below. ipc_server.cpp and eq_state.h have zero audio-backend
# dependencies (no PipeWire/WASAPI/CoreAudio), so these build and run even on
# a machine that can't build eq-daemon itself (e.g. no libpipewire-0.3-dev).
#
# "Any platform" holds for eq_state_tests and wasapi_backend_tests, but NOT
# for ipc_server_tests -- see the guard on it below.
# ──────────────────────────────────────────────────────────────────────────────
if(EQUALIZER_BUILD_TESTS)
    find_package(Threads REQUIRED)

    add_executable(eq_state_tests tests/test_eq_state.cpp eq_state.h)
    target_include_directories(eq_state_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME eq_state_tests COMMAND eq_state_tests)

    # POSIX-only, and guarded accordingly. tests/test_ipc_server.cpp drives a
    # real Unix domain socket (<sys/socket.h>, <sys/un.h>, <unistd.h>), which
    # matches ipc_server.cpp's own POSIX-only implementation -- ipc_server.cpp
    # #ifndef _WIN32's its whole body away, so there would be nothing to test
    # on Windows even if the test's includes resolved. This target used to be
    # registered unconditionally, which broke the entire Windows CMake build
    # at "fatal error C1083: Cannot open include file: 'sys/socket.h'" -- and
    # the comment above claimed it built anywhere. Drop this guard when a
    # Windows named-pipe implementation lands on both sides (ARCHITECTURE.md
    # section 7.2).
    if(NOT PLATFORM_WINDOWS)
        add_executable(ipc_server_tests
            tests/test_ipc_server.cpp
            ipc_server.cpp
            ipc_server.h
            eq_state.h
        )
        target_include_directories(ipc_server_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(ipc_server_tests PRIVATE Threads::Threads)
        add_test(NAME ipc_server_tests COMMAND ipc_server_tests)
    else()
        message(STATUS
            "ipc_server_tests: POSIX-only (Unix domain sockets) — skipping on Windows.")
    endif()

    # wasapi_backend.h is currently a stub (Open() just logs and returns
    # false -- no real Win32/WASAPI calls), so it's safe to build and run
    # this test on any platform by defining BACKEND_WASAPI locally, same as
    # eq-daemon does for real on PLATFORM_WINDOWS below. See the comment at
    # the top of tests/test_wasapi_backend.cpp: once a real implementation
    # lands, move this target behind if(PLATFORM_WINDOWS).
    add_executable(wasapi_backend_tests tests/test_wasapi_backend.cpp wasapi_backend.h eq_state.h)
    target_include_directories(wasapi_backend_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME wasapi_backend_tests COMMAND wasapi_backend_tests)
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Sources common across all platforms
# ──────────────────────────────────────────────────────────────────────────────
set(DAEMON_SOURCES
    main.cpp
    ipc_server.cpp
    ipc_server.h
    audio_backend.h
    eq_state.h
)

# ──────────────────────────────────────────────────────────────────────────────
# Platform-specific backend
# ──────────────────────────────────────────────────────────────────────────────
if(PLATFORM_LINUX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

    list(APPEND DAEMON_SOURCES pipewire_backend.cpp pipewire_backend.h)

    add_executable(eq-daemon ${DAEMON_SOURCES})
    target_compile_definitions(eq-daemon PRIVATE BACKEND_PIPEWIRE)
    target_include_directories(eq-daemon PRIVATE
        ${PIPEWIRE_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/DSP
    )
    target_link_libraries(eq-daemon PRIVATE dsp ${PIPEWIRE_LIBRARIES})
    target_compile_options(eq-daemon PRIVATE ${PIPEWIRE_CFLAGS_OTHER} -Wall -Wextra)

elseif(PLATFORM_WINDOWS)
    # wasapi_backend.h is header-only: the stub defines WasapiBackend and
    # CreateAudioBackend() inline. This branch used to list a
    # wasapi_backend.cpp that has never existed in the repo, so configuring
    # the daemon on Windows failed outright. Add the .cpp back to this list
    # at the same time you add the file.
    #
    # Listing the header here is for IDE/source-group visibility only -- CMake
    # does not compile headers, so this line alone provides no definition of
    # CreateAudioBackend(). main.cpp includes wasapi_backend.h directly under
    # #ifdef BACKEND_WASAPI to get the inline definition; without that the
    # target configured and compiled but failed at link with
    # "unresolved external symbol eq::CreateAudioBackend". Note an inline
    # function is only emitted in a TU that odr-uses it, so a stub .cpp that
    # merely #includes the header would not fix that -- the calling TU has to
    # see the definition.
    list(APPEND DAEMON_SOURCES wasapi_backend.h)

    add_executable(eq-daemon ${DAEMON_SOURCES})
    target_compile_definitions(eq-daemon PRIVATE BACKEND_WASAPI UNICODE _UNICODE)
    target_include_directories(eq-daemon PRIVATE ${CMAKE_SOURCE_DIR}/DSP)
    target_link_libraries(eq-daemon PRIVATE dsp ole32 ksuser avrt mmdevapi)

else()
    # macOS and everything else: there is no backend source in the tree.
    # coreaudio_backend.{cpp,h} and stub_backend.{cpp,h} were both referenced
    # here but neither has ever existed, so these branches produced a
    # confusing "cannot find source file" at configure time. Skip the target
    # and say so instead -- the unit tests above still build and run.
    if(PLATFORM_MAC)
        message(STATUS
            "eq-daemon: no CoreAudio backend in this repo yet — skipping the "
            "daemon target. DSP/IPC unit tests still build.")
    else()
        message(STATUS
            "eq-daemon: unsupported platform and no stub backend in this repo — "
            "skipping the daemon target. DSP/IPC unit tests still build.")
    endif()
endif()
