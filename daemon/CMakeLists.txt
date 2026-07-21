cmake_minimum_required(VERSION 3.20)

# ──────────────────────────────────────────────────────────────────────────────
# IPC / state unit tests — deliberately independent of the platform backend
# branch below. ipc_server.cpp and eq_state.h have zero audio-backend
# dependencies (no PipeWire/WASAPI/CoreAudio), so these build and run even on
# a machine that can't build eq-daemon itself (e.g. no libpipewire-0.3-dev).
# ──────────────────────────────────────────────────────────────────────────────
if(EQUALIZER_BUILD_TESTS)
    find_package(Threads REQUIRED)

    add_executable(eq_state_tests tests/test_eq_state.cpp eq_state.h)
    target_include_directories(eq_state_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME eq_state_tests COMMAND eq_state_tests)

    add_executable(ipc_server_tests
        tests/test_ipc_server.cpp
        ipc_server.cpp
        ipc_server.h
        eq_state.h
    )
    target_include_directories(ipc_server_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ipc_server_tests PRIVATE Threads::Threads)
    add_test(NAME ipc_server_tests COMMAND ipc_server_tests)

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
    list(APPEND DAEMON_SOURCES wasapi_backend.cpp wasapi_backend.h)

    add_executable(eq-daemon ${DAEMON_SOURCES})
    target_compile_definitions(eq-daemon PRIVATE BACKEND_WASAPI UNICODE _UNICODE)
    target_include_directories(eq-daemon PRIVATE ${CMAKE_SOURCE_DIR}/DSP)
    target_link_libraries(eq-daemon PRIVATE dsp ole32 ksuser avrt mmdevapi)

elseif(PLATFORM_MAC)
    list(APPEND DAEMON_SOURCES coreaudio_backend.cpp coreaudio_backend.h)

    add_executable(eq-daemon ${DAEMON_SOURCES})
    target_compile_definitions(eq-daemon PRIVATE BACKEND_COREAUDIO)
    target_include_directories(eq-daemon PRIVATE ${CMAKE_SOURCE_DIR}/DSP)
    target_link_libraries(eq-daemon PRIVATE dsp
        "-framework CoreAudio" "-framework AudioToolbox" "-framework Foundation")

else()
    message(WARNING "eq-daemon: unsupported platform — building stub only")
    list(APPEND DAEMON_SOURCES stub_backend.cpp stub_backend.h)

    add_executable(eq-daemon ${DAEMON_SOURCES})
    target_include_directories(eq-daemon PRIVATE ${CMAKE_SOURCE_DIR}/DSP)
    target_link_libraries(eq-daemon PRIVATE dsp)
endif()
