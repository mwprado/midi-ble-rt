cmake_minimum_required(VERSION 3.16)

project(midi_ble_rt_gui LANGUAGES C)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(UI_DEPS REQUIRED glib-2.0 gio-2.0)
pkg_check_modules(GNOME_DEPS REQUIRED gtk4 libadwaita-1 gio-2.0 glib-2.0)

add_library(midi-ble-rt-ui-facade STATIC
    facade/mb-ui-facade.c
    facade/mb-ui-model.c
    facade/mb-daemon-observer.c
    facade/mb-bluez-observer.c
)

target_include_directories(midi-ble-rt-ui-facade PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/facade
    ${UI_DEPS_INCLUDE_DIRS}
)

target_link_libraries(midi-ble-rt-ui-facade PUBLIC
    ${UI_DEPS_LIBRARIES}
)

target_compile_options(midi-ble-rt-ui-facade PRIVATE
    ${UI_DEPS_CFLAGS_OTHER}
    -Wall -Wextra
)

add_executable(midi-ble-rt-gui
    gnome/mb-gnome-main.c
    gnome/mb-gnome-window.c
)

target_include_directories(midi-ble-rt-gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/gnome
    ${CMAKE_CURRENT_SOURCE_DIR}/facade
    ${CMAKE_BINARY_DIR}/generated
    ${GNOME_DEPS_INCLUDE_DIRS}
)

target_link_libraries(midi-ble-rt-gui PRIVATE
    midi-ble-rt-ui-facade
    ${GNOME_DEPS_LIBRARIES}
)

target_compile_options(midi-ble-rt-gui PRIVATE
    ${GNOME_DEPS_CFLAGS_OTHER}
    -Wall -Wextra
)

install(TARGETS midi-ble-rt-gui
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
