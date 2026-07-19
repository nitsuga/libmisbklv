# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(misbklv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)  # defines CMAKE_INSTALL_INCLUDEDIR/LIBDIR (used below)
find_package(Threads REQUIRED)  # gst_stream_test's loopback receiver thread (B4)

# Address+UB sanitizers for the hardening pass (off by default; CI/dev opt-in).
# Applied to all targets so instrumentation is consistent across the lib + tests.
option(MISBKLV_SANITIZE "Build with -fsanitize=address,undefined" OFF)
if(MISBKLV_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
  message(STATUS "sanitizers enabled (address,undefined)")
endif()

# Compiled core library (ADR 0005/0010/0011). All public headers — including the
# generated constexpr registry tables (include/misbklv/registry/) — under include/.
add_library(misbklv STATIC
  src/ber.cpp
  src/codec.cpp
  src/packet.cpp
  src/builder.cpp
  src/series.cpp
  src/registries.cpp
  src/message.cpp
  src/ts.cpp)
add_library(misbklv::misbklv ALIAS misbklv)
target_include_directories(misbklv PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(misbklv PUBLIC cxx_std_20)

# --- Optional gstreamer media backend: misbklv-gst (ADR 0013; fork 14) -------
# A separate target so the core library stays gstreamer-free. Built only when
# gstreamer is found (unless MISBKLV_GSTREAMER=OFF).
option(MISBKLV_GSTREAMER "Build the gstreamer media backend" ON)
if(MISBKLV_GSTREAMER)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(GST IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0)
  endif()
  if(GST_FOUND)
    add_library(misbklv-gst STATIC src/gst/gst_backend.cpp src/gst/stream.cpp)
    add_library(misbklv::gst ALIAS misbklv-gst)
    # Export as misbklv::gst (not misbklv::misbklv-gst); the ALIAS is build-tree only.
    set_target_properties(misbklv-gst PROPERTIES EXPORT_NAME gst)
    target_link_libraries(misbklv-gst PUBLIC misbklv PkgConfig::GST Threads::Threads)
    message(STATUS "misbklv-gst: gstreamer media backend enabled")
  else()
    message(STATUS "misbklv-gst: gstreamer not found -- media backend disabled")
  endif()
endif()

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

add_executable(hardening_test test/hardening_test.cpp)  # real-world hardening
target_link_libraries(hardening_test PRIVATE misbklv)
add_test(NAME hardening COMMAND hardening_test
  ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/dayflight_first_packet.klv)

add_executable(message_test test/message_test.cpp)  # high-level Message (ADR 0018)
target_link_libraries(message_test PRIVATE misbklv)
add_test(NAME message COMMAND message_test
  ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/dayflight_first_packet.klv)

add_executable(mock_backend_test test/mock_backend_test.cpp)
target_link_libraries(mock_backend_test PRIVATE misbklv)
add_test(NAME mock_backend COMMAND mock_backend_test)

if(TARGET misbklv-gst)  # gstreamer extraction round-trip (ADR 0013 / B1)
  add_executable(gst_backend_test test/gst_backend_test.cpp)
  target_link_libraries(gst_backend_test PRIVATE misbklv-gst)
  add_test(NAME gst_extract COMMAND gst_backend_test
    "${CMAKE_CURRENT_SOURCE_DIR}/data/Day Flight.mpg"
    "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/dayflight.klv")

  add_executable(gst_insert_test test/gst_insert_test.cpp)  # B2 insertion round-trip
  target_link_libraries(gst_insert_test PRIVATE misbklv-gst)
  add_test(NAME gst_insert COMMAND gst_insert_test
    "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/dayflight.klv"
    "${CMAKE_CURRENT_BINARY_DIR}/insert_roundtrip.ts")

  add_executable(gst_stream_test test/gst_stream_test.cpp)  # B4 real-time udp round-trip
  target_link_libraries(gst_stream_test PRIVATE misbklv-gst Threads::Threads)
  add_test(NAME gst_stream COMMAND gst_stream_test
    "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/dayflight.klv")

  add_executable(api_test test/api_test.cpp)  # high-level facade end-to-end (ADR 0018)
  target_link_libraries(api_test PRIVATE misbklv-gst)
  add_test(NAME api_stream COMMAND api_test
    "${CMAKE_CURRENT_SOURCE_DIR}/data/Day Flight.mpg"
    "${CMAKE_CURRENT_BINARY_DIR}/api_edit.ts")

  add_executable(klv_edit examples/klv_edit.cpp)  # user-facing example (not a test)
  target_link_libraries(klv_edit PRIVATE misbklv-gst)

  add_executable(stop_test test/stop_test.cpp)  # cooperative cancellation (ADR 0019)
  target_link_libraries(stop_test PRIVATE misbklv-gst Threads::Threads)
  add_test(NAME stream_stop COMMAND stop_test
    "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/dayflight_first_packet.klv")
endif()

# --- Regression over real MPEG-TS captures ----------------------------------
# The KLV elementary streams were extracted once and **committed** under
# test/fixtures/ (cheyenne/falls/falls_ext/sync.klv), rather than re-extracted at
# build time with ffmpeg. A demuxer's exact byte output is NOT stable across
# ffmpeg versions — ffmpeg 6 framed falls.ts stream 0 differently than 4.4, which
# made CI non-deterministic — so committing the fixtures makes this regression
# hermetic and drops the ffmpeg build/test dependency entirely.
set(_fix ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures)
add_test(NAME stream_cheyenne  COMMAND stream_roundtrip_test ${_fix}/cheyenne.klv)
add_test(NAME stream_falls     COMMAND stream_roundtrip_test ${_fix}/falls.klv)
add_test(NAME stream_falls_ext COMMAND stream_roundtrip_test ${_fix}/falls_ext.klv)  # extended 0601 (tags 75-91)
add_test(NAME stream_sync      COMMAND stream_roundtrip_test ${_fix}/sync.klv)

# --- gst-free MPEG-TS KLV extractor (fork 12 / B3): 0x06 + 0x15 --------------
# Cross-checks our extractor's output against the committed reference KLV; the
# .ts/.mpg inputs come from git-lfs (`git lfs pull`).
add_executable(ts_extract_test test/ts_extract_test.cpp)
target_link_libraries(ts_extract_test PRIVATE misbklv)
add_test(NAME ts_extract_dayflight COMMAND ts_extract_test  # 0x06
  "${CMAKE_CURRENT_SOURCE_DIR}/data/Day Flight.mpg" ${_fix}/dayflight.klv)
add_test(NAME ts_extract_cheyenne COMMAND ts_extract_test  # 0x15
  "${CMAKE_CURRENT_SOURCE_DIR}/data/Cheyenne.ts" ${_fix}/cheyenne.klv)
add_test(NAME ts_extract_sync COMMAND ts_extract_test  # 0x15
  "${CMAKE_CURRENT_SOURCE_DIR}/data/klv_metadata_test_sync.ts" ${_fix}/sync.klv)

# --- Install / export --------------------------------------------------------
# Core:  find_package(misbklv)                 -> misbklv::misbklv
# Facade: find_package(misbklv COMPONENTS gst) -> misbklv::gst (needs gstreamer)
# The gst backend is a find_package COMPONENT so core-only consumers don't inherit
# the gstreamer dependency; its Config re-discovers gstreamer (ADR 0014).
include(CMakePackageConfigHelpers)

install(TARGETS misbklv EXPORT misbklvCoreTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})  # headers incl. generated tables
install(EXPORT misbklvCoreTargets
  FILE misbklvCoreTargets.cmake
  NAMESPACE misbklv::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/misbklv)

set(MISBKLV_HAS_GST OFF)
if(TARGET misbklv-gst)
  set(MISBKLV_HAS_GST ON)
  install(TARGETS misbklv-gst EXPORT misbklvGstTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(EXPORT misbklvGstTargets
    FILE misbklvGstTargets.cmake
    NAMESPACE misbklv::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/misbklv)
endif()

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
