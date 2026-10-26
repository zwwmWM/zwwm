find_package(PkgConfig REQUIRED)
include(GNUInstallDirs)
find_package(glslang CONFIG REQUIRED)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

set(ZWWM_SHADER_FILES zwwm.glsl glass_physics.glsl surface.vert surface.frag glass.frag kawase.frag)
foreach(shader IN LISTS ZWWM_SHADER_FILES)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}")
endforeach()

# #version must be the first line, so sources are embedded unpadded.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/zwwm.glsl" ZWWM_LIBRARY_SHADER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/glass_physics.glsl" ZWWM_GLASS_PHYSICS_SHADER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/surface.vert" ZWWM_SURFACE_VERTEX_SHADER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/surface.frag" ZWWM_SURFACE_FRAGMENT_SHADER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/glass.frag" ZWWM_GLASS_FRAGMENT_SHADER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/kawase.frag" ZWWM_KAWASE_FRAGMENT_SHADER)
configure_file(src/shaders.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/zwwm/renderer/shaders.hpp" @ONLY)

add_library(zwwm_renderer STATIC
  src/scene.cpp
  src/shader_compiler.cpp
  src/vulkan.cpp
)

target_include_directories(zwwm_renderer PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
)
target_include_directories(zwwm_renderer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/zwwm/renderer")
target_link_libraries(zwwm_renderer
  PUBLIC zwwm::vlk
  PRIVATE PkgConfig::PNG glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
target_compile_definitions(zwwm_renderer PRIVATE
  ZWWM_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/zwwm"
  ZWWM_SOURCE_DATA_DIR="${CMAKE_SOURCE_DIR}/zwwm/data"
)
target_compile_options(zwwm_renderer PRIVATE -Wall -Wextra -Wpedantic -Werror -Wno-missing-designated-field-initializers)
add_library(zwwm::renderer ALIAS zwwm_renderer)
