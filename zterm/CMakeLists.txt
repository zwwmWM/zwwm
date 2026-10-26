cmake_minimum_required(VERSION 3.25)
project(zterm LANGUAGES CXX)
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "zterm requires Clang")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET zwayland-client libzterm zw-lang zwwm-vlk vulkan pangocairo harfbuzz gbm libdrm xkbcommon)
pkg_check_modules(PROTOCOLS REQUIRED zwwm-protocols)
pkg_get_variable(protocol_dir zwwm-protocols pkgdatadir)
find_program(scanner zwayland-scanner REQUIRED)
find_program(glslang_validator glslangValidator REQUIRED)
foreach(stage vert frag)
  set(spirv_header "${CMAKE_CURRENT_BINARY_DIR}/terminal-${stage}.spv.h")
  set(spirv_name "terminal_vertex_spirv")
  if(stage STREQUAL "frag")
    set(spirv_name "terminal_fragment_spirv")
  endif()
  add_custom_command(OUTPUT "${spirv_header}"
    COMMAND "${glslang_validator}" -V --target-env vulkan1.3 --vn "${spirv_name}"
            -o "${spirv_header}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/terminal.${stage}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/terminal.${stage}" VERBATIM)
  list(APPEND headers "${spirv_header}")
endforeach()
set(specs
  "xdg-shell|upstream/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
  "linux-dmabuf|upstream/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml"
  "viewporter|upstream/wayland-protocols/stable/viewporter/viewporter.xml"
  "fractional-scale|upstream/wayland-protocols/staging/fractional-scale/fractional-scale-v1.xml"
  "cursor-shape|upstream/wayland-protocols/staging/cursor-shape/cursor-shape-v1.xml"
  "ext-zwwm-manager-v1|ext-zwwm-manager-v1.xml"
  "zwwm-client-connection-v1|zwwm-client-connection-v1.xml")
foreach(spec IN LISTS specs)
  string(REPLACE "|" ";" parts "${spec}")
  list(GET parts 0 name)
  list(GET parts 1 xml)
  set(header "${CMAKE_CURRENT_BINARY_DIR}/${name}-zwayland-client.h")
  add_custom_command(OUTPUT "${header}"
    COMMAND "${scanner}" zwayland-client-header "${protocol_dir}/${xml}" "${header}"
    DEPENDS "${protocol_dir}/${xml}" VERBATIM)
  list(APPEND headers "${header}")
endforeach()
add_executable(zterm src/main.cpp src/gpu.cpp src/text.cpp src/clipboard.cpp src/config.cpp src/config_watch.cpp src/theme.cpp src/connection.cpp src/daemon.cpp ${headers})
target_include_directories(zterm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
# Vulkan create-info structs are filled with partial designated initializers.
target_compile_options(zterm PRIVATE -Wall -Wextra -Wpedantic -Werror -Wno-missing-designated-field-initializers)
target_compile_definitions(zterm PRIVATE
  ZTERM_THEME_DIR="${CMAKE_INSTALL_FULL_DATADIR}/zterm/themes"
  ZTERM_SOURCE_THEME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/themes")
target_link_libraries(zterm PRIVATE PkgConfig::DEPS util)
install(TARGETS zterm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES config.zw DESTINATION "${CMAKE_INSTALL_DATADIR}/zterm")
install(DIRECTORY themes/ DESTINATION "${CMAKE_INSTALL_DATADIR}/zterm/themes")
install(FILES README.md DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/zterm")
