cmake_minimum_required(VERSION 3.16)
project(midi_ble_rt C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED glib-2.0 gio-2.0 alsa)

add_executable(midi-ble-rtd
    src/midi-ble-rtd.c
)

add_executable(midi-ble-rtctl
    src/midi-ble-rtctl.c
)

target_include_directories(midi-ble-rtd PRIVATE ${DEPS_INCLUDE_DIRS})
target_link_libraries(midi-ble-rtd PRIVATE ${DEPS_LIBRARIES})
target_compile_options(midi-ble-rtd PRIVATE ${DEPS_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)

target_include_directories(midi-ble-rtctl PRIVATE ${DEPS_INCLUDE_DIRS})
target_link_libraries(midi-ble-rtctl PRIVATE ${DEPS_LIBRARIES})
target_compile_options(midi-ble-rtctl PRIVATE ${DEPS_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)

install(TARGETS midi-ble-rtd midi-ble-rtctl DESTINATION bin)
install(FILES config/roland-gokeys.ini.example DESTINATION share/midi-ble-rt)
install(FILES systemd/midi-ble-rtd.service DESTINATION lib/systemd/system)
