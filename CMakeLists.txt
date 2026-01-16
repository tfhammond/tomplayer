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
  src/demo/wasapi_demo.cpp
  src/engine/player_engine.cpp
  src/audio/wasapi_output.cpp
  src/buffer/audio_ring_buffer.cpp
  src/decode/wav_decoder.cpp
  src/decode/flac_decoder.cpp
)

add_executable(player ${PLAYER_SOURCES})
target_include_directories(player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(player PRIVATE cxx_std_20)

find_package(FLAC CONFIG REQUIRED)
target_link_libraries(player PRIVATE FLAC::FLAC ole32 mmdevapi avrt uuid)

include(CTest)
if (BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)

  add_executable(wasapi_output_tests
    tests/wasapi_output_tests.cpp
    src/audio/wasapi_output.cpp
  )
  target_include_directories(wasapi_output_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_features(wasapi_output_tests PRIVATE cxx_std_20)
  target_compile_definitions(wasapi_output_tests PRIVATE TOMPLAYER_TESTING)
  target_link_libraries(wasapi_output_tests PRIVATE Catch2::Catch2WithMain ole32 avrt)

  add_test(NAME wasapi_output_tests COMMAND wasapi_output_tests)

  add_executable(ring_buffer_tests
    tests/ring_buffer_tests.cpp
    src/buffer/audio_ring_buffer.cpp
  )
  target_include_directories(ring_buffer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_features(ring_buffer_tests PRIVATE cxx_std_20)
  target_link_libraries(ring_buffer_tests PRIVATE Catch2::Catch2WithMain)

  add_test(NAME ring_buffer_tests COMMAND ring_buffer_tests)
endif()

if (MSVC)
  target_compile_options(player PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
else()
  target_compile_options(player PRIVATE -Wall -Wextra -Wpedantic)
endif()
