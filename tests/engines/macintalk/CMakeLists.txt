# The MacinTalk engine on its own: a console program that lists the voices,
# says what is installed, checks the phoneme table, and with Panthera and
# Apple's data sings test phrases to WAV files and measures them.
#
#   cmake -G Ninja -S tests/engines/macintalk -B build-tests/macintalk
#   cmake --build build-tests/macintalk
#   build-tests/macintalk/macintalk_sing_test [--out DIR] [--gen leopard] [--voice Fred]
#   build-tests/macintalk/macintalk_sing_test --calibrate      (the tuning table)
cmake_minimum_required(VERSION 3.20)
project(macintalk_sing_test C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(SVS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
include(${SVS_ROOT}/cmake/engines/macintalk.cmake)

add_executable(macintalk_sing_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sing_test.cpp
  ${SVS_ROOT}/src/voices/pitch.cpp
  ${SVS_ROOT}/src/audio/wav.cpp
  ${SVS_ROOT}/src/core/paths.cpp
  ${SVS_ROOT}/src/core/phonology.cpp
)
target_compile_definitions(macintalk_sing_test PRIVATE SVS_SOURCE_DIR="${SVS_ROOT}")
target_compile_options(macintalk_sing_test PRIVATE -Wall -Wextra)
target_compile_options(svs_macintalk PRIVATE -Wall -Wextra)
target_link_libraries(macintalk_sing_test PRIVATE svs_macintalk)
if(WIN32)
  target_link_options(macintalk_sing_test PRIVATE -static -static-libgcc -static-libstdc++)
endif()
