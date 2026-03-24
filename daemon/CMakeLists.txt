cmake_minimum_required(VERSION 3.20)

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
