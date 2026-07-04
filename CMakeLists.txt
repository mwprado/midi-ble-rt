cmake_minimum_required(VERSION 3.16)

set(MIDI_BLE_RT_VERSION "0.9.4" CACHE STRING "midi-ble-rt project version")
project(midi_ble_rt VERSION ${MIDI_BLE_RT_VERSION} LANGUAGES C)

include(GNUInstallDirs)
include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SYSTEMD_USER_UNIT_DIR "lib/systemd/user"
    CACHE PATH "systemd user unit installation directory")

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED glib-2.0 gio-2.0 alsa)

configure_file(
    src/mb-version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/mb-version.h"
    @ONLY
)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/systemd/user")
configure_file(
    systemd/user/midi-ble-rtd.service.in
    "${CMAKE_CURRENT_BINARY_DIR}/systemd/user/midi-ble-rtd.service"
    @ONLY
)

add_library(midi-ble-rt-core STATIC
    src/mb-alsa.c
    src/mb-alsa-port.c
    src/mb-bluez.c
    src/mb-gatt-midi.c
    src/mb-ble-midi.c
    src/mb-config.c
    src/mb-control-protocol.c
    src/mb-device-discovery.c
    src/mb-duplex-runtime.c
    src/mb-frame-model.c
    src/mb-latency-diagnostics.c
    src/mb-log.c
    src/mb-paths.c
    src/mb-rtkit.c
    src/mb-runtime.c
    src/mb-session.c
    src/mb-slice-ring.c
    src/mb-stats.c
)

target_include_directories(midi-ble-rt-core
    PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${DEPS_INCLUDE_DIRS}
)
target_link_libraries(midi-ble-rt-core PUBLIC ${DEPS_LIBRARIES})
target_compile_options(midi-ble-rt-core PRIVATE ${DEPS_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)

add_executable(midi-ble-rtd
    src/mb-daemon-main.c
    src/mb-daemon.c
)

add_executable(midi-ble-rtctl
    src/mb-ctl-main.c
    src/mb-stats-ctl.c
    src/midi-ble-rtctl.c
)

set_source_files_properties(src/mb-daemon.c
    PROPERTIES COMPILE_DEFINITIONS "main=midi_ble_rtd_runtime_main"
)

set_source_files_properties(src/mb-stats-ctl.c
    PROPERTIES COMPILE_DEFINITIONS "main=midi_ble_rtctl_stats_main"
)

set_source_files_properties(src/midi-ble-rtctl.c
    PROPERTIES COMPILE_DEFINITIONS "main=midi_ble_rtctl_bluez_main"
)

target_include_directories(midi-ble-rtd PRIVATE ${DEPS_INCLUDE_DIRS})
target_link_libraries(midi-ble-rtd PRIVATE midi-ble-rt-core ${DEPS_LIBRARIES})
target_compile_options(midi-ble-rtd PRIVATE ${DEPS_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)

target_include_directories(midi-ble-rtctl PRIVATE ${DEPS_INCLUDE_DIRS})
target_link_libraries(midi-ble-rtctl PRIVATE midi-ble-rt-core ${DEPS_LIBRARIES})
target_compile_options(midi-ble-rtctl PRIVATE ${DEPS_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(test-mb-session
        tests/test-mb-session.c
    )
    target_link_libraries(test-mb-session PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-session PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-session COMMAND $<TARGET_FILE:test-mb-session>)

    add_executable(test-mb-session-state
        tests/test-mb-session-state.c
    )
    target_link_libraries(test-mb-session-state PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-session-state PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-session-state COMMAND $<TARGET_FILE:test-mb-session-state>)

    add_executable(test-mb-alsa
        tests/test-mb-alsa.c
    )
    target_link_libraries(test-mb-alsa PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-alsa PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-alsa COMMAND $<TARGET_FILE:test-mb-alsa>)

    add_executable(test-mb-ble-midi
        tests/test-mb-ble-midi.c
    )
    target_link_libraries(test-mb-ble-midi PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-ble-midi PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-ble-midi COMMAND $<TARGET_FILE:test-mb-ble-midi>)

    add_executable(test-mb-config
        tests/test-mb-config.c
    )
    target_link_libraries(test-mb-config PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-config PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-config COMMAND $<TARGET_FILE:test-mb-config>)

    add_executable(test-mb-frame-model
        tests/test-mb-frame-model.c
    )
    target_link_libraries(test-mb-frame-model PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-frame-model PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-frame-model COMMAND $<TARGET_FILE:test-mb-frame-model>)

    add_executable(test-mb-slice-ring
        tests/test-mb-slice-ring.c
    )
    target_link_libraries(test-mb-slice-ring PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-slice-ring PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-slice-ring COMMAND $<TARGET_FILE:test-mb-slice-ring>)

    add_executable(test-mb-runtime
        tests/test-mb-runtime.c
    )
    target_link_libraries(test-mb-runtime PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-runtime PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-runtime COMMAND $<TARGET_FILE:test-mb-runtime>)

    add_executable(test-mb-duplex-runtime
        tests/test-mb-duplex-runtime.c
    )
    target_link_libraries(test-mb-duplex-runtime PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-duplex-runtime PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-duplex-runtime COMMAND $<TARGET_FILE:test-mb-duplex-runtime>)

    add_executable(test-mb-paths
        tests/test-mb-paths.c
    )
    target_link_libraries(test-mb-paths PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-paths PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-paths COMMAND $<TARGET_FILE:test-mb-paths>)

    add_executable(test-mb-control-protocol
        tests/test-mb-control-protocol.c
    )
    target_link_libraries(test-mb-control-protocol PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-control-protocol PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-control-protocol COMMAND $<TARGET_FILE:test-mb-control-protocol>)
endif()

set(MIDI_BLE_RT_MAN_PAGES
    docs/man/midi-ble-rtd.1
    docs/man/midi-ble-rtctl.1
)

find_program(MAKEINFO_EXECUTABLE makeinfo)
if(MAKEINFO_EXECUTABLE)
    set(MIDI_BLE_RT_INFO_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/docs/info/midi-ble-rt.texi")
    set(MIDI_BLE_RT_INFO_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/docs/info/midi-ble-rt.info")

    add_custom_command(
        OUTPUT "${MIDI_BLE_RT_INFO_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/docs/info"
        COMMAND "${MAKEINFO_EXECUTABLE}" --no-split -o "${MIDI_BLE_RT_INFO_OUTPUT}" "${MIDI_BLE_RT_INFO_SOURCE}"
        DEPENDS "${MIDI_BLE_RT_INFO_SOURCE}"
        VERBATIM
    )
    add_custom_target(midi-ble-rt-info ALL DEPENDS "${MIDI_BLE_RT_INFO_OUTPUT}")
else()
    message(STATUS "makeinfo not found; GNU Info manual will not be built")
endif()

install(TARGETS midi-ble-rtd midi-ble-rtctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY config/ DESTINATION ${CMAKE_INSTALL_DATADIR}/midi-ble-rt/config FILES_MATCHING PATTERN "*.example")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/systemd/user/midi-ble-rtd.service" DESTINATION ${SYSTEMD_USER_UNIT_DIR})
install(FILES docs/MULTI_DEVICE_CONFIG.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES ${MIDI_BLE_RT_MAN_PAGES} DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES docs/info/midi-ble-rt.texi DESTINATION ${CMAKE_INSTALL_DOCDIR})
if(MAKEINFO_EXECUTABLE)
    install(FILES "${MIDI_BLE_RT_INFO_OUTPUT}" DESTINATION ${CMAKE_INSTALL_INFODIR})
endif()
