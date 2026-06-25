cmake_minimum_required(VERSION 3.16)
project(midi_ble_rt C)

include(GNUInstallDirs)
include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED glib-2.0 gio-2.0 alsa)

add_library(midi-ble-rt-core
    src/mb-alsa.c
    src/mb-ble-midi.c
    src/mb-buffer.c
    src/mb-config.c
    src/mb-duplex-runtime.c
    src/mb-frame-model.c
    src/mb-log.c
    src/mb-runtime.c
    src/mb-session.c
    src/mb-slice-ring.c
    src/mb-stats.c
)

target_include_directories(midi-ble-rt-core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${DEPS_INCLUDE_DIRS}
)
target_link_libraries(midi-ble-rt-core PUBLIC ${DEPS_LIBRARIES})
target_compile_options(midi-ble-rt-core PRIVATE ${DEPS_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)

add_executable(midi-ble-rtd
    src/mb-daemon.c
    src/mb-orchestrator.c
)

add_executable(midi-ble-rtctl
    src/mb-stats-ctl.c
    src/midi-ble-rtctl.c
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
    add_test(NAME mb-session COMMAND test-mb-session)

    add_executable(test-mb-alsa
        tests/test-mb-alsa.c
    )
    target_link_libraries(test-mb-alsa PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-alsa PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-alsa COMMAND test-mb-alsa)

    add_executable(test-mb-ble-midi
        tests/test-mb-ble-midi.c
    )
    target_link_libraries(test-mb-ble-midi PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-ble-midi PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-ble-midi COMMAND test-mb-ble-midi)

    add_executable(test-mb-config
        tests/test-mb-config.c
    )
    target_link_libraries(test-mb-config PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-config PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-config COMMAND test-mb-config)

    add_executable(test-mb-buffer
        tests/test-mb-buffer.c
    )
    target_link_libraries(test-mb-buffer PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-buffer PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-buffer COMMAND test-mb-buffer)

    add_executable(test-mb-frame-model
        tests/test-mb-frame-model.c
    )
    target_link_libraries(test-mb-frame-model PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-frame-model PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-frame-model COMMAND test-mb-frame-model)

    add_executable(test-mb-slice-ring
        tests/test-mb-slice-ring.c
    )
    target_link_libraries(test-mb-slice-ring PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-slice-ring PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-slice-ring COMMAND test-mb-slice-ring)

    add_executable(test-mb-runtime
        tests/test-mb-runtime.c
    )
    target_link_libraries(test-mb-runtime PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-runtime PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-runtime COMMAND test-mb-runtime)

    add_executable(test-mb-duplex-runtime
        tests/test-mb-duplex-runtime.c
    )
    target_link_libraries(test-mb-duplex-runtime PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-duplex-runtime PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-duplex-runtime COMMAND test-mb-duplex-runtime)
endif()

install(TARGETS midi-ble-rtd midi-ble-rtctl DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY config/ DESTINATION ${CMAKE_INSTALL_DATADIR}/midi-ble-rt/config FILES_MATCHING PATTERN "*.ini.example")
install(FILES examples/midi/test-note.mid DESTINATION ${CMAKE_INSTALL_DATADIR}/midi-ble-rt/examples/midi)
install(PROGRAMS scripts/test-alsa-loopback.sh scripts/test-fluidsynth-smoke.sh DESTINATION ${CMAKE_INSTALL_DATADIR}/midi-ble-rt/scripts)
install(FILES packaging/fedora/midi-ble-rtd.service DESTINATION lib/systemd/user)
