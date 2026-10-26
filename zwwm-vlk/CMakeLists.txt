cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(zwwm-vlk LANGUAGES CXX)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "zwwm-vlk requires Clang")
  endif()
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(ZWAYLAND_CLIENT REQUIRED IMPORTED_TARGET zwayland-client)
  set(ZWWM_VLK_ZWAYLAND PkgConfig::ZWAYLAND_CLIENT)
else()
  set(ZWWM_VLK_ZWAYLAND zwayland-client)
endif()

find_package(PkgConfig REQUIRED)
find_package(Vulkan 1.3 REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_library(zwwm-vlk
  src/device.cpp
  src/dmabuf.cpp
  src/image.cpp
  src/window.cpp
)
add_library(zwwm::vlk ALIAS zwwm-vlk)

target_include_directories(zwwm-vlk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(zwwm-vlk
  PUBLIC ${ZWWM_VLK_ZWAYLAND} Vulkan::Vulkan
  PRIVATE PkgConfig::LIBDRM)
target_compile_features(zwwm-vlk PUBLIC cxx_std_23)
# Vulkan create-info structs are filled with partial designated initializers.
target_compile_options(zwwm-vlk PRIVATE -Wall -Wextra -Wpedantic -Werror -Wno-missing-designated-field-initializers)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(GNUInstallDirs)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../.version" ZWWM_VLK_VERSION LIMIT_COUNT 1)
  set_target_properties(zwwm-vlk PROPERTIES VERSION "${ZWWM_VLK_VERSION}")
  configure_file(zwwm-vlk.pc.in zwwm-vlk.pc @ONLY)
  install(TARGETS zwwm-vlk
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/zwwm-vlk.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
  install(FILES ../LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/zwwm-vlk")
endif()
