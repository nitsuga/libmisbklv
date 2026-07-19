# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(misbklv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)  # defines CMAKE_INSTALL_INCLUDEDIR/LIBDIR (used below)

# Compiled core library (ADR 0005/0010/0011). All public headers — including the
# generated constexpr registry tables (include/misbklv/registry/) — under include/.
add_library(misbklv STATIC
  src/ber.cpp
  src/codec.cpp
  src/packet.cpp
  src/builder.cpp
  src/series.cpp
  src/registries.cpp)
add_library(misbklv::misbklv ALIAS misbklv)
target_include_directories(misbklv PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(misbklv PUBLIC cxx_std_20)

# Regenerate the committed registry tables from TOML source (ADR 0012).
# Not part of the default build; CI runs it and checks for drift.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  set(_gen_dir ${CMAKE_CURRENT_SOURCE_DIR}/include/misbklv/registry)
  add_custom_target(regenerate-registry
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_registry.py
            ${CMAKE_CURRENT_SOURCE_DIR}/registry/uas0601.toml
            ${_gen_dir}/uas0601_tables.generated.hpp
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_registry.py
            ${CMAKE_CURRENT_SOURCE_DIR}/registry/vmti0903.toml
            ${_gen_dir}/vmti0903_tables.generated.hpp
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_registry.py
            ${CMAKE_CURRENT_SOURCE_DIR}/registry/vtarget0903.toml
            ${_gen_dir}/vtarget0903_tables.generated.hpp
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

add_executable(vtarget_roundtrip_test test/vtarget_roundtrip_test.cpp)
target_link_libraries(vtarget_roundtrip_test PRIVATE misbklv)
add_test(NAME vtarget_series
  COMMAND vtarget_roundtrip_test
          ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/vmti_vtarget.klv)

add_executable(jmisb_crosscheck_test test/jmisb_crosscheck_test.cpp)
target_link_libraries(jmisb_crosscheck_test PRIVATE misbklv)
add_test(NAME jmisb_crosscheck COMMAND jmisb_crosscheck_test)

# --- Regression over real MPEG-TS captures (data/*.ts, git-lfs) --------------
# The KLV elementary stream is extracted at build time with ffmpeg (a test-only
# dependency) rather than committing large .klv fixtures. Requires the LFS .ts to
# be materialized (`git lfs pull`). Skipped entirely if ffmpeg is not found.
find_program(FFMPEG_EXECUTABLE ffmpeg)
if(FFMPEG_EXECUTABLE)
  set(_klv_outputs "")
  function(add_ts_stream_test name ts_file stream_idx)
    set(_klv ${CMAKE_CURRENT_BINARY_DIR}/${name}.klv)
    add_custom_command(OUTPUT ${_klv}
      COMMAND ${FFMPEG_EXECUTABLE} -y -loglevel error
              -i ${CMAKE_CURRENT_SOURCE_DIR}/data/${ts_file}
              -map 0:d:${stream_idx} -c copy -f data ${_klv}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/${ts_file}
      COMMENT "Extracting KLV: ${ts_file} stream ${stream_idx} -> ${name}.klv"
      VERBATIM)
    set(_klv_outputs ${_klv_outputs} ${_klv} PARENT_SCOPE)
    add_test(NAME stream_${name} COMMAND stream_roundtrip_test ${_klv})
  endfunction()

  add_ts_stream_test(cheyenne   Cheyenne.ts               0)
  add_ts_stream_test(falls      falls.ts                  0)
  add_ts_stream_test(falls_ext  falls.ts                  1)  # extended 0601 (tags 75-91)
  add_ts_stream_test(sync       klv_metadata_test_sync.ts 0)

  # Extract as part of the default build so `ctest` finds the .klv fixtures.
  add_custom_target(extract_ts_klv ALL DEPENDS ${_klv_outputs})
else()
  message(STATUS "ffmpeg not found -- skipping data/*.ts regression tests")
endif()

# --- Install / export: consumers do find_package(misbklv) -> misbklv::misbklv -
include(CMakePackageConfigHelpers)

install(TARGETS misbklv EXPORT misbklvTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})  # headers incl. generated tables
install(EXPORT misbklvTargets
  FILE misbklvTargets.cmake
  NAMESPACE misbklv::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/misbklv)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/misbklvConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/misbklvConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/misbklv)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/misbklvConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/misbklvConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/misbklvConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/misbklv)
