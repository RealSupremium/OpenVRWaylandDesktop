cmake_minimum_required(VERSION 3.20)
project(openvr_desktop_overlay C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(GIO REQUIRED gio-2.0 gio-unix-2.0)
pkg_check_modules(VULKAN REQUIRED vulkan)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner REQUIRED)

pkg_get_variable(WAYLAND_PROTOCOLS_BASE wayland-protocols pkgdatadir)
set(XDG_OUTPUT_XML "${WAYLAND_PROTOCOLS_BASE}/unstable/xdg-output/xdg-output-unstable-v1.xml")

set(XDG_OUTPUT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/wayland-xdg-output-unstable-v1-client-protocol.h")
set(XDG_OUTPUT_CODE "${CMAKE_CURRENT_BINARY_DIR}/wayland-xdg-output-unstable-v1-protocol.c")

add_custom_command(
    OUTPUT "${XDG_OUTPUT_HEADER}"
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header "${XDG_OUTPUT_XML}" "${XDG_OUTPUT_HEADER}"
    DEPENDS "${XDG_OUTPUT_XML}"
    COMMENT "Generating Wayland xdg-output client header..."
)

add_custom_command(
    OUTPUT "${XDG_OUTPUT_CODE}"
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code "${XDG_OUTPUT_XML}" "${XDG_OUTPUT_CODE}"
    DEPENDS "${XDG_OUTPUT_XML}"
    COMMENT "Generating Wayland xdg-output protocol C code..."
)

add_custom_target(generate_wayland_protocols
    DEPENDS "${XDG_OUTPUT_HEADER}" "${XDG_OUTPUT_CODE}"
)

find_path(OPENVR_INCLUDE_DIR openvr.h PATH_SUFFIXES openvr)
find_library(OPENVR_LIBRARY NAMES openvr_api)

if(NOT OPENVR_INCLUDE_DIR OR NOT OPENVR_LIBRARY)
    message(FATAL_ERROR "OpenVR library or headers not found.")
endif()

set(SOURCES
    src/App.cpp
    src/CaptureInstance.cpp
    src/ConfigManager.cpp
    src/ManifestManager.cpp
    src/TrayManager.cpp
    src/InputManager.cpp
    src/VRVulkanOverlay.cpp
    src/PortalCapture.cpp
    src/WaylandDisplayTracker.cpp
    src/EvdevTracker.cpp
    ${XDG_OUTPUT_CODE}
)

add_executable(openvr_desktop_overlay ${SOURCES})

add_dependencies(openvr_desktop_overlay generate_wayland_protocols)

target_include_directories(openvr_desktop_overlay PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${PIPEWIRE_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
    ${VULKAN_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${OPENVR_INCLUDE_DIR}
)

target_link_libraries(openvr_desktop_overlay PRIVATE
    ${WAYLAND_CLIENT_LIBRARIES}
    ${PIPEWIRE_LIBRARIES}
    ${GIO_LIBRARIES}
    ${VULKAN_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${OPENVR_LIBRARY}
    nlohmann_json::nlohmann_json
    Threads::Threads
)

set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)

set_target_properties(openvr_desktop_overlay PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

add_custom_command(TARGET openvr_desktop_overlay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OPENVR_LIBRARY}"
    "$<TARGET_FILE_DIR:openvr_desktop_overlay>"
    COMMENT "Copying ${OPENVR_LIBRARY} next to executable"
)

include(GNUInstallDirs)

install(TARGETS openvr_desktop_overlay
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES "${OPENVR_LIBRARY}"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES resources/openvr_desktop_overlay.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

install(FILES resources/openvr_desktop_overlay.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
)
