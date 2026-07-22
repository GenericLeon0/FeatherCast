cmake_minimum_required(VERSION 3.22)

if(NOT WIN32)
  message(FATAL_ERROR "FeatherCast Native is a Windows-only Win32 application.")
endif()

project(FeatherCastNative VERSION 0.7.3 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# FeatherCast is distributed as a self-contained native launcher. Keep every
# project target on the static MSVC runtime so the installer and portable ZIP
# do not depend on a separately installed Visual C++ Redistributable.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY
      "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(FEATHERCAST_WARNINGS_AS_ERRORS "Treat FeatherCast project warnings as errors" OFF)
set(FEATHERCAST_EXPECTED_PUBLISHER "" CACHE STRING "Expected Authenticode publisher subject")
set(FEATHERCAST_ALLOWED_SIGNER_THUMBPRINTS "" CACHE STRING
    "Semicolon-separated SHA-256 Authenticode signer certificate thumbprints")

function(feathercast_apply_project_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /FS)
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
  SQLITE_ENABLE_FTS5
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_THREADSAFE=1
)

add_library(FeatherCastRuntime STATIC
  native/src/runtime.cpp
  native/src/persistence_service.cpp
  native/src/search_coordinator.cpp
  native/src/search_pipeline.cpp
  native/src/discovery_service.cpp
  native/src/command_catalog.cpp
  native/src/capability_catalog.cpp
  native/src/settings_catalog.cpp
  native/src/background_services.cpp
  native/src/network_client.cpp
  native/src/file_content.cpp
  native/src/file_index_service.cpp
  native/src/file_search_service.cpp
  native/src/game_discovery.cpp
  native/src/preview_service.cpp
  native/src/library.cpp
  native/src/snippets_io.cpp
)

target_include_directories(FeatherCastRuntime PUBLIC native/src)
target_compile_features(FeatherCastRuntime PUBLIC cxx_std_23)
target_compile_definitions(FeatherCastRuntime PUBLIC
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)
target_link_libraries(FeatherCastRuntime PUBLIC
  FeatherCastSQLite
  advapi32
  crypt32
  ole32
  windowscodecs
  winhttp
)

add_library(FeatherCastUi STATIC
  native/src/ui.cpp
  native/src/result_icons.cpp
  native/src/library_ui.cpp
)

target_include_directories(FeatherCastUi PUBLIC native/src)
target_compile_features(FeatherCastUi PUBLIC cxx_std_23)
target_compile_definitions(FeatherCastUi PUBLIC
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)
target_link_libraries(FeatherCastUi PUBLIC FeatherCastRuntime d2d1 oleacc uuid)

add_executable(FeatherCast WIN32
  native/src/main.cpp
  native/src/audio_volume.cpp
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
  FeatherCastRuntime
  FeatherCastUi
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
  wintrust
  crypt32
)

if(MSVC)
  target_compile_options(FeatherCast PRIVATE /bigobj "$<$<CONFIG:Release>:/Zi>")
  target_compile_options(FeatherCastPluginHost PRIVATE "$<$<CONFIG:Release>:/Zi>")
  target_link_options(FeatherCast PRIVATE
    "/MANIFEST:EMBED"
    "/MANIFESTINPUT:${FEATHERCAST_MANIFEST}"
    "$<$<CONFIG:Release>:/DEBUG:FULL>"
  )
  target_link_options(FeatherCastPluginHost PRIVATE "$<$<CONFIG:Release>:/DEBUG:FULL>")
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

add_executable(FeatherCastFileSearchBenchmarks
  native/tests/file_search_benchmarks.cpp
)

add_executable(FeatherCastSettingsExecutorTests
  native/tests/settings_executor_tests.cpp
)

add_executable(FeatherCastStorageTrustTests
  native/tests/storage_trust_tests.cpp
)

add_executable(FeatherCastInteractionStateTests
  native/tests/interaction_state_tests.cpp
)

add_executable(FeatherCastUiEventQueueTests
  native/tests/ui_event_queue_tests.cpp
)

add_executable(FeatherCastPersistenceServiceTests
  native/tests/persistence_service_tests.cpp
)

add_executable(FeatherCastSearchCoordinatorTests
  native/tests/search_coordinator_tests.cpp
)

add_executable(FeatherCastDiscoveryServiceTests
  native/tests/discovery_service_tests.cpp
)

add_executable(FeatherCastCatalogUiStateTests
  native/tests/catalog_ui_state_tests.cpp
)

add_executable(FeatherCastBackgroundServicesTests
  native/tests/background_services_tests.cpp
)

add_executable(FeatherCastRuntimeLifecycleTests
  native/tests/runtime_lifecycle_tests.cpp
)

add_executable(FeatherCastProductivityPackTests
  native/tests/productivity_pack_tests.cpp
)

add_executable(FeatherCastLibraryTests
  native/tests/library_tests.cpp
)

add_executable(FeatherCastSearchFilesTests
  native/tests/search_files_tests.cpp
)

add_executable(FeatherCastGameDiscoveryTests
  native/tests/game_discovery_tests.cpp
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
target_compile_features(FeatherCastFileSearchBenchmarks PRIVATE cxx_std_23)
target_compile_features(FeatherCastSettingsExecutorTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastStorageTrustTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastInteractionStateTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastUiEventQueueTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastPersistenceServiceTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastSearchCoordinatorTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastDiscoveryServiceTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastCatalogUiStateTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastBackgroundServicesTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastRuntimeLifecycleTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastProductivityPackTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastLibraryTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastSearchFilesTests PRIVATE cxx_std_23)
target_compile_features(FeatherCastGameDiscoveryTests PRIVATE cxx_std_23)

target_include_directories(FeatherCastTestExtension PRIVATE native/src)
target_include_directories(FeatherCastV1TestExtension PRIVATE native/src)
target_include_directories(FeatherCastBadApiExtension PRIVATE native/src)
target_include_directories(FeatherCastPluginHostTests PRIVATE native/src)
target_include_directories(FeatherCastSearchBenchmarks PRIVATE native/src)
target_include_directories(FeatherCastFileSearchBenchmarks PRIVATE native/src third_party/sqlite)
target_include_directories(FeatherCastSettingsExecutorTests PRIVATE native/src)
target_include_directories(FeatherCastStorageTrustTests PRIVATE native/src third_party/sqlite)
target_include_directories(FeatherCastInteractionStateTests PRIVATE native/src)
target_include_directories(FeatherCastUiEventQueueTests PRIVATE native/src)
target_include_directories(FeatherCastPersistenceServiceTests PRIVATE native/src third_party/sqlite)
target_include_directories(FeatherCastSearchCoordinatorTests PRIVATE native/src)
target_include_directories(FeatherCastDiscoveryServiceTests PRIVATE native/src)
target_include_directories(FeatherCastCatalogUiStateTests PRIVATE native/src)
target_include_directories(FeatherCastBackgroundServicesTests PRIVATE native/src)
target_include_directories(FeatherCastRuntimeLifecycleTests PRIVATE native/src third_party/sqlite)
target_include_directories(FeatherCastProductivityPackTests PRIVATE native/src)
target_include_directories(FeatherCastLibraryTests PRIVATE native/src)
target_include_directories(FeatherCastSearchFilesTests PRIVATE native/src third_party/sqlite)
target_include_directories(FeatherCastGameDiscoveryTests PRIVATE native/src)

target_compile_definitions(FeatherCastV1TestExtension PRIVATE FEATHERCAST_TEST_API_VERSION=1)
target_compile_definitions(FeatherCastBadApiExtension PRIVATE FEATHERCAST_TEST_BAD_API)
target_compile_definitions(FeatherCastPluginHostTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastSearchBenchmarks PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastFileSearchBenchmarks PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastSettingsExecutorTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastStorageTrustTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastInteractionStateTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastUiEventQueueTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastPersistenceServiceTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastSearchCoordinatorTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastDiscoveryServiceTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastCatalogUiStateTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastBackgroundServicesTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastRuntimeLifecycleTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastProductivityPackTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastLibraryTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastSearchFilesTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(FeatherCastGameDiscoveryTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(FeatherCastUiEventQueueTests PRIVATE FeatherCastRuntime)
target_link_libraries(FeatherCastPersistenceServiceTests PRIVATE FeatherCastRuntime)
target_link_libraries(FeatherCastSearchCoordinatorTests PRIVATE FeatherCastRuntime)
target_link_libraries(FeatherCastDiscoveryServiceTests PRIVATE FeatherCastRuntime)
target_link_libraries(FeatherCastCatalogUiStateTests PRIVATE FeatherCastRuntime FeatherCastUi)
target_link_libraries(FeatherCastBackgroundServicesTests PRIVATE FeatherCastRuntime)
target_link_libraries(FeatherCastRuntimeLifecycleTests PRIVATE FeatherCastRuntime)
target_link_libraries(FeatherCastProductivityPackTests PRIVATE FeatherCastRuntime)
target_link_libraries(FeatherCastLibraryTests PRIVATE FeatherCastRuntime)
target_link_libraries(FeatherCastSearchFilesTests PRIVATE FeatherCastRuntime)
target_link_libraries(FeatherCastGameDiscoveryTests PRIVATE FeatherCastRuntime)
target_link_libraries(FeatherCastSearchBenchmarks PRIVATE normaliz)
target_link_libraries(FeatherCastFileSearchBenchmarks PRIVATE FeatherCastRuntime)
target_link_libraries(FeatherCastStorageTrustTests PRIVATE FeatherCastSQLite advapi32 crypt32)

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
feathercast_apply_project_warnings(FeatherCastFileSearchBenchmarks)
feathercast_apply_project_warnings(FeatherCastSettingsExecutorTests)
feathercast_apply_project_warnings(FeatherCastStorageTrustTests)
feathercast_apply_project_warnings(FeatherCastInteractionStateTests)
feathercast_apply_project_warnings(FeatherCastRuntime)
feathercast_apply_project_warnings(FeatherCastUi)
feathercast_apply_project_warnings(FeatherCastUiEventQueueTests)
feathercast_apply_project_warnings(FeatherCastPersistenceServiceTests)
feathercast_apply_project_warnings(FeatherCastSearchCoordinatorTests)
feathercast_apply_project_warnings(FeatherCastDiscoveryServiceTests)
feathercast_apply_project_warnings(FeatherCastCatalogUiStateTests)
feathercast_apply_project_warnings(FeatherCastBackgroundServicesTests)
feathercast_apply_project_warnings(FeatherCastRuntimeLifecycleTests)
feathercast_apply_project_warnings(FeatherCastProductivityPackTests)
feathercast_apply_project_warnings(FeatherCastLibraryTests)
feathercast_apply_project_warnings(FeatherCastSearchFilesTests)
feathercast_apply_project_warnings(FeatherCastGameDiscoveryTests)

enable_testing()
add_test(NAME FeatherCastCoreTests COMMAND FeatherCastCoreTests)
add_test(NAME FeatherCastPluginHostTests
  COMMAND FeatherCastPluginHostTests
    $<TARGET_FILE:FeatherCastPluginHost>
    $<TARGET_FILE:FeatherCastTestExtension>
    $<TARGET_FILE:FeatherCastV1TestExtension>
    $<TARGET_FILE:FeatherCastBadApiExtension>
)
add_test(NAME FeatherCastSettingsExecutorTests COMMAND FeatherCastSettingsExecutorTests)
add_test(NAME FeatherCastStorageTrustTests COMMAND FeatherCastStorageTrustTests)
add_test(NAME FeatherCastInteractionStateTests COMMAND FeatherCastInteractionStateTests)
add_test(NAME FeatherCastUiEventQueueTests COMMAND FeatherCastUiEventQueueTests)
add_test(NAME FeatherCastPersistenceServiceTests COMMAND FeatherCastPersistenceServiceTests)
add_test(NAME FeatherCastSearchCoordinatorTests COMMAND FeatherCastSearchCoordinatorTests)
add_test(NAME FeatherCastDiscoveryServiceTests COMMAND FeatherCastDiscoveryServiceTests)
add_test(NAME FeatherCastCatalogUiStateTests COMMAND FeatherCastCatalogUiStateTests)
add_test(NAME FeatherCastBackgroundServicesTests COMMAND FeatherCastBackgroundServicesTests)
add_test(NAME FeatherCastRuntimeLifecycleTests COMMAND FeatherCastRuntimeLifecycleTests)
add_test(NAME FeatherCastProductivityPackTests COMMAND FeatherCastProductivityPackTests)
add_test(NAME FeatherCastLibraryTests COMMAND FeatherCastLibraryTests)
add_test(NAME FeatherCastSearchFilesTests COMMAND FeatherCastSearchFilesTests)
add_test(NAME FeatherCastGameDiscoveryTests COMMAND FeatherCastGameDiscoveryTests)
add_test(NAME FeatherCastSilentLifecycleTests COMMAND FeatherCast --self-test)

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
