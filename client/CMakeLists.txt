cmake_minimum_required(VERSION 3.16)
project(rmi_client LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DEFAULT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui")
if (NOT DEFINED IMGUI_DIR)
  set(IMGUI_DIR "" CACHE PATH "Path to Dear ImGui")
endif()

if (IMGUI_DIR)
  if (NOT EXISTS "${IMGUI_DIR}/imgui.h")
    message(FATAL_ERROR "Dear ImGui not found at IMGUI_DIR. Point it at the imgui root (containing imgui.h).")
  endif()
else()
  if (EXISTS "${IMGUI_DEFAULT_DIR}/imgui.h")
    set(IMGUI_DIR "${IMGUI_DEFAULT_DIR}")
  else()
    include(FetchContent)
    set(IMGUI_GIT_REPO "https://github.com/ocornut/imgui.git" CACHE STRING "Dear ImGui git repository")
    set(IMGUI_GIT_TAG "v1.90.5" CACHE STRING "Dear ImGui git tag or commit")
    message(STATUS "IMGUI_DIR not set; fetching Dear ImGui from ${IMGUI_GIT_REPO} (${IMGUI_GIT_TAG}).")
    FetchContent_Declare(
      imgui
      GIT_REPOSITORY ${IMGUI_GIT_REPO}
      GIT_TAG ${IMGUI_GIT_TAG}
    )
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
  endif()
endif()

find_package(SDL2 REQUIRED)

if (EXISTS "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp")
  set(IMGUI_SDL_RENDERER_BACKEND "sdlrenderer2")
  set(IMGUI_SDL_RENDERER_SRC "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp")
elseif (EXISTS "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer.cpp")
  set(IMGUI_SDL_RENDERER_BACKEND "sdlrenderer")
  set(IMGUI_SDL_RENDERER_SRC "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer.cpp")
else()
  message(FATAL_ERROR "No SDL renderer backend found in Dear ImGui. Expected imgui_impl_sdlrenderer2.cpp or imgui_impl_sdlrenderer.cpp.")
endif()

add_executable(rmi_client
  src/main.cpp
  src/rmi_client.cpp
  src/net.cpp
  src/stb_image.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_SDL_RENDERER_SRC}
  ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)

add_library(rmi_protocol STATIC
  ../protocol/rmi_protocol.c
)
target_include_directories(rmi_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_include_directories(rmi_client PRIVATE
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
  ${IMGUI_DIR}/misc/cpp
  src
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

if (IMGUI_SDL_RENDERER_BACKEND STREQUAL "sdlrenderer2")
  target_compile_definitions(rmi_client PRIVATE RMI_IMGUI_SDLRENDERER2)
else()
  target_compile_definitions(rmi_client PRIVATE RMI_IMGUI_SDLRENDERER)
endif()

if (TARGET SDL2::SDL2)
  target_link_libraries(rmi_client PRIVATE SDL2::SDL2)
  if (TARGET SDL2::SDL2main)
    target_link_libraries(rmi_client PRIVATE SDL2::SDL2main)
  endif()
else()
  target_include_directories(rmi_client PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(rmi_client PRIVATE ${SDL2_LIBRARIES})
endif()

target_link_libraries(rmi_client PRIVATE rmi_protocol)
