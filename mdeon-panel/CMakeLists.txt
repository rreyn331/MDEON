cmake_minimum_required(VERSION 3.22)
project(mdeon_panel VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO_XCB REQUIRED IMPORTED_TARGET cairo-xcb)
pkg_check_modules(CAIRO_XLIB REQUIRED IMPORTED_TARGET cairo-xlib)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(GDK_PIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)
pkg_check_modules(JSONC QUIET IMPORTED_TARGET json-c)
pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols)
pkg_check_modules(XKBCOMMON QUIET IMPORTED_TARGET xkbcommon)
find_program(WAYLAND_SCANNER wayland-scanner)

add_executable(mdeon-panel
    src/main.cpp
    src/icon_theme.cpp
    src/panel.cpp
    src/modules/battery.cpp
    src/modules/bluetooth.cpp
    src/modules/clock.cpp
    src/modules/custom_button.cpp
    src/modules/executer.cpp
    src/modules/launcher.cpp
    src/modules/spacer.cpp
    src/modules/application_menu.cpp
    src/modules/desktop_hub.cpp
    src/modules/network.cpp
    src/modules/notifications.cpp
    src/modules/show_desktop.cpp
    src/modules/task_list.cpp
    src/modules/tray.cpp
    src/modules/volume.cpp
    src/modules/virtual_desktop.cpp
    src/backends/x11_backend.cpp
    src/backends/wayland_task_backend.cpp
)
install(TARGETS mdeon-panel RUNTIME DESTINATION bin)
add_executable(mdeon-panel-config src/config_editor.cpp)
target_link_libraries(mdeon-panel-config PRIVATE
    PkgConfig::CAIRO_XLIB
    PkgConfig::PANGOCAIRO
    PkgConfig::X11
)
target_compile_features(mdeon-panel-config PRIVATE cxx_std_20)
target_include_directories(mdeon-panel-config PRIVATE include)
target_compile_definitions(mdeon-panel-config PRIVATE
    MDEON_PANEL_EDITOR_SOURCE_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/config-example.conf"
    MDEON_PANEL_EDITOR_FALLBACK_CONFIG="${CMAKE_INSTALL_FULL_DATADIR}/mdeon-panel/config-example.conf"
)
target_compile_options(mdeon-panel-config PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS mdeon-panel-config RUNTIME DESTINATION bin)
install(FILES config-example.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/mdeon-panel)
install(FILES mdeon-panel-config.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(DIRECTORY extensions/gnome/mdeon-panel-window-manager@roy
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions)
install(PROGRAMS extensions/gnome/install.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions
    RENAME install-mdeon-panel-gnome.sh)
install(PROGRAMS extensions/gnome/install-system.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions
    RENAME install-mdeon-panel-gnome-system.sh)
target_include_directories(mdeon-panel PRIVATE include)
target_link_libraries(mdeon-panel PRIVATE
    PkgConfig::CAIRO_XCB
    PkgConfig::PANGOCAIRO
    PkgConfig::XCB
    PkgConfig::GIO
    PkgConfig::GIO_UNIX
    PkgConfig::GDK_PIXBUF
)
target_compile_features(mdeon-panel PRIVATE cxx_std_20)
target_compile_definitions(mdeon-panel PRIVATE
    MDEON_PANEL_SOURCE_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/config-example.conf"
    MDEON_PANEL_FALLBACK_CONFIG_PATH="${CMAKE_INSTALL_FULL_DATADIR}/mdeon-panel/config-example.conf"
)
target_compile_options(mdeon-panel PRIVATE -Wall -Wextra -Wpedantic)

if(JSONC_FOUND)
    target_link_libraries(mdeon-panel PRIVATE PkgConfig::JSONC)
    target_compile_definitions(mdeon-panel PRIVATE SIMPANEL_HAVE_JSON_C=1)
endif()

if(WAYLAND_CLIENT_FOUND AND WAYLAND_PROTOCOLS_FOUND AND XKBCOMMON_FOUND AND WAYLAND_SCANNER)
    pkg_get_variable(WAYLAND_PROTOCOLS_DATA_DIR wayland-protocols pkgdatadir)
    set(LAYER_SHELL_XML
        ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml)
    set(LAYER_SHELL_HEADER
        ${CMAKE_CURRENT_BINARY_DIR}/generated/wlr-layer-shell-unstable-v1-client-protocol.h)
    set(LAYER_SHELL_CODE
        ${CMAKE_CURRENT_BINARY_DIR}/generated/wlr-layer-shell-unstable-v1-protocol.c)
    set(XDG_SHELL_XML
        ${WAYLAND_PROTOCOLS_DATA_DIR}/stable/xdg-shell/xdg-shell.xml)
    set(XDG_SHELL_HEADER
        ${CMAKE_CURRENT_BINARY_DIR}/generated/xdg-shell-client-protocol.h)
    set(XDG_SHELL_CODE
        ${CMAKE_CURRENT_BINARY_DIR}/generated/xdg-shell-protocol.c)
    set(FOREIGN_TOPLEVEL_XML
        ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml)
    set(FOREIGN_TOPLEVEL_HEADER
        ${CMAKE_CURRENT_BINARY_DIR}/generated/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h)
    set(FOREIGN_TOPLEVEL_CODE
        ${CMAKE_CURRENT_BINARY_DIR}/generated/wlr-foreign-toplevel-management-unstable-v1-protocol.c)
    set(PLASMA_SHELL_XML
        ${CMAKE_CURRENT_SOURCE_DIR}/protocols/plasma-shell.xml)
    set(PLASMA_SHELL_HEADER
        ${CMAKE_CURRENT_BINARY_DIR}/generated/plasma-shell-client-protocol.h)
    set(PLASMA_SHELL_CODE
        ${CMAKE_CURRENT_BINARY_DIR}/generated/plasma-shell-protocol.c)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
    add_custom_command(
        OUTPUT ${LAYER_SHELL_HEADER} ${LAYER_SHELL_CODE}
        COMMAND ${WAYLAND_SCANNER} client-header ${LAYER_SHELL_XML} ${LAYER_SHELL_HEADER}
        COMMAND ${WAYLAND_SCANNER} private-code ${LAYER_SHELL_XML} ${LAYER_SHELL_CODE}
        DEPENDS ${LAYER_SHELL_XML}
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${XDG_SHELL_HEADER} ${XDG_SHELL_CODE}
        COMMAND ${WAYLAND_SCANNER} client-header ${XDG_SHELL_XML} ${XDG_SHELL_HEADER}
        COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_XML} ${XDG_SHELL_CODE}
        DEPENDS ${XDG_SHELL_XML}
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${FOREIGN_TOPLEVEL_HEADER} ${FOREIGN_TOPLEVEL_CODE}
        COMMAND ${WAYLAND_SCANNER} client-header ${FOREIGN_TOPLEVEL_XML} ${FOREIGN_TOPLEVEL_HEADER}
        COMMAND ${WAYLAND_SCANNER} private-code ${FOREIGN_TOPLEVEL_XML} ${FOREIGN_TOPLEVEL_CODE}
        DEPENDS ${FOREIGN_TOPLEVEL_XML}
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${PLASMA_SHELL_HEADER} ${PLASMA_SHELL_CODE}
        COMMAND ${WAYLAND_SCANNER} client-header ${PLASMA_SHELL_XML} ${PLASMA_SHELL_HEADER}
        COMMAND ${WAYLAND_SCANNER} private-code ${PLASMA_SHELL_XML} ${PLASMA_SHELL_CODE}
        DEPENDS ${PLASMA_SHELL_XML}
        VERBATIM
    )
    target_sources(mdeon-panel PRIVATE
        src/backends/wayland_backend.cpp
        ${FOREIGN_TOPLEVEL_CODE}
        ${LAYER_SHELL_CODE}
        ${PLASMA_SHELL_CODE}
        ${XDG_SHELL_CODE}
    )
    target_include_directories(mdeon-panel PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(mdeon-panel PRIVATE
        PkgConfig::WAYLAND_CLIENT
        PkgConfig::XKBCOMMON
    )
    target_compile_definitions(mdeon-panel PRIVATE SIMPANEL_HAVE_WAYLAND=1)
    target_compile_definitions(mdeon-panel PRIVATE SIMPANEL_HAVE_FOREIGN_TOPLEVEL=1)
    message(STATUS "Building the direct Wayland backend")
else()
    message(STATUS "Wayland backend disabled: wayland-client, wayland-protocols, xkbcommon, and wayland-scanner are required")
endif()