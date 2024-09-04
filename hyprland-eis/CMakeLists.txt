cmake_minimum_required(VERSION 3.19)
find_package(PkgConfig REQUIRED)

include(GNUInstallDirs)
set(LIBEXECDIR ${CMAKE_INSTALL_FULL_LIBEXECDIR})

project(hyprland-eis LANGUAGES CXX VERSION 0.1)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 11)  # Ensure C standard is set if needed

add_compile_options(
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Wno-unused-value
        -Wno-missing-field-initializers
        -Wno-narrowing
        -Wno-pointer-arith
        $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-fpermissive>
        -Wno-address-of-temporary)

pkg_check_modules(
        deps
        REQUIRED
        IMPORTED_TARGET
        libei-1.0
        xkbcommon
        wayland-client
        wayland-protocols
        wayland-scanner
)

set(PROJECT_SOURCES
        src/config.h
        src/main.cpp
        ../protocols/virtual-keyboard-unstable-v1-protocol.c
        ../protocols/wlr-virtual-pointer-unstable-v1-protocol.c
)
# Gather all .c files from the util directory
file(GLOB UTIL_SOURCES src/util/*.cpp)

# Add the executable, including the C and C++ sources
add_executable(hyprland-eis
        ${UTIL_SOURCES}
        ${PROJECT_SOURCES}
)

target_link_libraries(
        hyprland-eis PRIVATE PkgConfig::deps eis)

install(TARGETS hyprland-eis
        BUNDLE DESTINATION .
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
