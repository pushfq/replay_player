cmake_minimum_required(VERSION 3.25.0)

project(replay_player)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(lib/osu-parsers)
add_subdirectory(lib/ctfp)

add_executable(
  ${PROJECT_NAME}
  src/main.cpp
)

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE
  ntdll
  ctfp::ctfp
  osu-parsers::osu-parsers
)

target_compile_features(
  ${PROJECT_NAME}
  PRIVATE
  cxx_std_23
)

target_compile_options(
  ${PROJECT_NAME}
  PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -fno-exceptions
  -fno-rtti
)
