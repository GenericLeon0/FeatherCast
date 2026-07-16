cmake_minimum_required(VERSION 3.22)

if(NOT WIN32)
  message(FATAL_ERROR "FeatherCast Native is a Windows-only Win32 application.")
endif()

project(FeatherCastNative VERSION 0.5.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FEATHERCAST_WARNINGS_AS_ERRORS "Treat FeatherCast project warnings as errors" OFF)
set(FEATHERCAST_EXPECTED_PUBLISHER "" CACHE STRING "Expected Authenticode publisher subject")
set(FEATHERCAST_ALLOWED_SIGNER_THUMBPRINTS "" CACHE STRING
    "Semicolon-separated SHA-256 Authenticode signer certificate thumbprints")

function(feathercast_apply_project_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    if(FEATHERCAST_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  endif()
endfunction()

set(FEATHERCAST_ICON "${CMAKE_CURRENT_SOURCE_DIR}/build/icon.ico")
set(FEATHERCAST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/native/app.manifest")
configure_file(native/FeatherCast.rc.in native/FeatherCast.rc @ONLY)
configure_file(native/src/version.hpp.in generated/version.hpp @ONLY)

add_library(FeatherCastSQLite STATIC
  third_party/sqlite/sqlite3.c
)

target_include_directories(FeatherCastSQLite PUBLIC third_party/sqlite)

target_compile_definitions(FeatherCastSQLite PRIVATE
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_THREADSAFE=1
)

add_executable(FeatherCast WIN32
  native/src/main.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/native/FeatherCast.rc
)

add_executable(FeatherCastPluginHost
  native/src/plugin_host.cpp
)

target_compile_definitions(FeatherCast PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

target_include_directories(FeatherCast PRIVATE native/src "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_compile_definitions(FeatherCastPluginHost PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

target_include_directories(FeatherCastPluginHost PRIVATE native/src)

target_link_libraries(FeatherCast PRIVATE
  FeatherCastSQLite
  d2d1
  d3d11
  dxgi
  dcomp
  dwrite
  windowscodecs
  shell32
  shlwapi
  ole32
  oleaut32
  oleacc
  uuid
  propsys
  dwmapi
  comctl32
  advapi32
  imm32
  normaliz
  powrprof
  winhttp
  wintrust
  crypt32
)

if(MSVC)
  target_compile_options(FeatherCast PRIVATE /bigobj)
  target_link_options(FeatherCast PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${FEATHERCAST_MANIFEST}")
endif()

add_executable(FeatherCastCoreTests
  native/tests/core_tests.cpp
)

add_library(FeatherCastTestExtension SHARED
  native/tests/test_extension.cpp
)

add_library(FeatherCastV1TestExtension SHARED
  native/tests/test_extension.cpp
)

add_library(FeatherCastBadApiExtension SHARED
  native/tests/test_extension.cpp
)

add_executable(FeatherCastPluginHostTests
  native/tests/plugin_host_tests.cpp
)

add_executable(FeatherCastSearchBenchmarks
  native/tests/search_benchmarks.cpp
)

target_compile_features(FeatherCastCoreTests PRIVATE cxx_std_23)
target_include_directories(FeatherCastCoreTests PRIVATE native/src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(FeatherCastCoreTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(FeatherCastCoreTests PRIVATE FeatherCastSQLite advapi32 normaliz wintrust crypt32)

target_compile_features(FeatherCastPluginHost PRIVATE cxx_std_23)
target_compile_features(FeatherCastTestExtension PRIVATE cxx_std_23)
target_compile_features(FeatherCastV1TestExtension PRIVATE cxx_std_23)
target_compile_features(FeatherCastBadApiExtension PRIVATE cxx_std_23)
target_compile_features(FeatherCastPluginHostTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastSearchBenchmarks PRIVATE cxx_std_23)

target_include_directories(FeatherCastTestExtension PRIVATE native/src)
target_include_directories(FeatherCastV1TestExtension PRIVATE native/src)
target_include_directories(FeatherCastBadApiExtension PRIVATE native/src)
target_include_directories(FeatherCastPluginHostTests PRIVATE native/src)
target_include_directories(FeatherCastSearchBenchmarks PRIVATE native/src)

target_compile_definitions(FeatherCastV1TestExtension PRIVATE FEATHERCAST_TEST_API_VERSION=1)
target_compile_definitions(FeatherCastBadApiExtension PRIVATE FEATHERCAST_TEST_BAD_API)
target_compile_definitions(FeatherCastPluginHostTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastSearchBenchmarks PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(FeatherCastSearchBenchmarks PRIVATE normaliz)

if(MSVC)
  target_compile_options(FeatherCastCoreTests PRIVATE /bigobj)
endif()

feathercast_apply_project_warnings(FeatherCast)
feathercast_apply_project_warnings(FeatherCastPluginHost)
feathercast_apply_project_warnings(FeatherCastCoreTests)
feathercast_apply_project_warnings(FeatherCastPluginHostTests)
feathercast_apply_project_warnings(FeatherCastTestExtension)
feathercast_apply_project_warnings(FeatherCastV1TestExtension)
feathercast_apply_project_warnings(FeatherCastBadApiExtension)
feathercast_apply_project_warnings(FeatherCastSearchBenchmarks)

enable_testing()
add_test(NAME FeatherCastCoreTests COMMAND FeatherCastCoreTests)
add_test(NAME FeatherCastPluginHostTests
  COMMAND FeatherCastPluginHostTests
    $<TARGET_FILE:FeatherCastPluginHost>
    $<TARGET_FILE:FeatherCastTestExtension>
    $<TARGET_FILE:FeatherCastV1TestExtension>
    $<TARGET_FILE:FeatherCastBadApiExtension>
)

install(TARGETS FeatherCast FeatherCastPluginHost RUNTIME DESTINATION bin)

set(CPACK_PACKAGE_NAME "FeatherCast")
set(CPACK_PACKAGE_VENDOR "Leon")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "FeatherCast-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "FeatherCast")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "FeatherCast")
set(CPACK_GENERATOR "ZIP;NSIS")
set(CPACK_NSIS_DISPLAY_NAME "FeatherCast")
set(CPACK_NSIS_PACKAGE_NAME "FeatherCast")
set(CPACK_PACKAGE_EXECUTABLES "FeatherCast" "FeatherCast")
set(CPACK_NSIS_MUI_ICON "${FEATHERCAST_ICON}")
set(CPACK_NSIS_MUI_UNIICON "${FEATHERCAST_ICON}")
set(CPACK_NSIS_MUI_FINISHPAGE_RUN "FeatherCast.exe")
set(CPACK_NSIS_EXTRA_DEFINES "!define MUI_FINISHPAGE_RUN_PARAMETERS \\\"--show\\\"")
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
    "DeleteRegValue HKCU \\\"Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Run\\\" \\\"FeatherCast\\\"")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
