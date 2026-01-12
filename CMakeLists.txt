cmake_minimum_required(VERSION 3.23)

project(tomplayer
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PLAYER_SOURCES
  src/main.cpp
  src/cli/interactive_cli.cpp
  src/engine/player_engine.cpp
  src/audio/wasapi_output.cpp
  src/decode/wav_decoder.cpp
  src/decode/flac_decoder.cpp
)

add_executable(player ${PLAYER_SOURCES})
target_include_directories(player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(player PRIVATE cxx_std_20)

find_package(FLAC CONFIG REQUIRED)
target_link_libraries(player PRIVATE FLAC::FLAC)

if (MSVC)
  target_compile_options(player PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
else()
  target_compile_options(player PRIVATE -Wall -Wextra -Wpedantic)
endif()
