# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(misbklv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only core for now (ADR 0005/0010/0011); .cpp split comes later.
add_library(misbklv INTERFACE)
target_include_directories(misbklv INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Regenerate the committed registry tables from TOML source (ADR 0012).
# Not part of the default build; CI runs it and checks for drift.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_custom_target(regenerate-registry
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_registry.py
            ${CMAKE_CURRENT_SOURCE_DIR}/registry/uas0601.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/src/registry/uas0601_tables.generated.hpp
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_registry.py
            ${CMAKE_CURRENT_SOURCE_DIR}/registry/vmti0903.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/src/registry/vmti0903_tables.generated.hpp
    COMMENT "Regenerating registry tables from registry/*.toml")
endif()

enable_testing()

add_executable(roundtrip_test test/roundtrip_test.cpp)
target_link_libraries(roundtrip_test PRIVATE misbklv)
add_test(NAME roundtrip
  COMMAND roundtrip_test
          ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/dayflight_first_packet.klv)

add_executable(stream_roundtrip_test test/stream_roundtrip_test.cpp)
target_link_libraries(stream_roundtrip_test PRIVATE misbklv)
add_test(NAME stream_dayflight
  COMMAND stream_roundtrip_test
          ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/dayflight.klv)
add_test(NAME stream_nightflight
  COMMAND stream_roundtrip_test
          ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/nightflight_ir.klv)

add_executable(nested_roundtrip_test test/nested_roundtrip_test.cpp)
target_link_libraries(nested_roundtrip_test PRIVATE misbklv)
add_test(NAME nested_vmti
  COMMAND nested_roundtrip_test
          ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/vmti_nested.klv)

add_executable(imapb_test test/imapb_test.cpp)
target_link_libraries(imapb_test PRIVATE misbklv)
add_test(NAME imapb COMMAND imapb_test)

add_executable(standalone_roundtrip_test test/standalone_roundtrip_test.cpp)
target_link_libraries(standalone_roundtrip_test PRIVATE misbklv)
add_test(NAME standalone_vmti
  COMMAND standalone_roundtrip_test
          ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/vmti_standalone.klv)
