cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c)

if(OS_WINDOWS)
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE src/dlss5-nr-filter.cpp src/bridge/nr_bridge.cpp src/bridge/nvof_flow.cpp
  )
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE d3d11 d3d12 dxgi version)

  # Caller shim: the DLSS NR runtime validates the calling module, so the
  # snippet calls must be forwarded through this separate DLL.
  add_library(nr-caller-shim SHARED src/bridge/caller_shim.cpp)
  target_link_libraries(nr-caller-shim PRIVATE d3d12)
  set_target_properties(
    nr-caller-shim
    PROPERTIES OUTPUT_NAME "nvngx.dll_obs" PREFIX "" SUFFIX ".dll"
  )
  add_custom_command(
    TARGET nr-caller-shim
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:nr-caller-shim> ${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>
    COMMENT "Copy nr-caller-shim to rundir"
  )
  install(TARGETS nr-caller-shim RUNTIME DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit")
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

option(ENABLE_PERFORMANCE_TESTS "Build frame policy tests and the manual OBS GPU smoke test" OFF)
option(ENABLE_GPU_SMOKE_TEST "Build the manual smoke test (requires an installed OBS and DLSS runtime)" OFF)
if(ENABLE_PERFORMANCE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
