cmake_minimum_required(VERSION 3.16)
project(midi_ble_rt C)

include(GNUInstallDirs)
include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED glib-2.0 gio-2.0 alsa)

add_library(midi-ble-rt-core
    src/mb-session.c
)

target_include_directories(midi-ble-rt-core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${DEPS_INCLUDE_DIRS}
)
target_link_libraries(midi-ble-rt-core PUBLIC ${DEPS_LIBRARIES})
target_compile_options(midi-ble-rt-core PRIVATE ${DEPS_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)

add_executable(midi-ble-rtd
    src/midi-ble-rtd.c
)

add_executable(midi-ble-rtctl
    src/midi-ble-rtctl.c
)

target_include_directories(midi-ble-rtd PRIVATE ${DEPS_INCLUDE_DIRS})
target_link_libraries(midi-ble-rtd PRIVATE midi-ble-rt-core ${DEPS_LIBRARIES})
target_compile_options(midi-ble-rtd PRIVATE ${DEPS_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)

target_include_directories(midi-ble-rtctl PRIVATE ${DEPS_INCLUDE_DIRS})
target_link_libraries(midi-ble-rtctl PRIVATE ${DEPS_LIBRARIES})
target_compile_options(midi-ble-rtctl PRIVATE ${DEPS_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(test-mb-session
        tests/test-mb-session.c
    )
    target_link_libraries(test-mb-session PRIVATE midi-ble-rt-core)
    target_compile_options(test-mb-session PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mb-session COMMAND test-mb-session)
endif()

install(TARGETS midi-ble-rtd midi-ble-rtctl DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY config/ DESTINATION ${CMAKE_INSTALL_DATADIR}/midi-ble-rt/config FILES_MATCHING PATTERN "*.ini.example")
