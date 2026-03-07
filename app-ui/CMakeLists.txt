cmake_minimum_required(VERSION 3.21)
project(niconeon-ui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Quick Qml)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED mpv)

qt_standard_project_setup(REQUIRES 6.4)
include(CTest)
option(NICONEON_BUILD_UI_E2E "Build Niconeon UI end-to-end tests" OFF)

set_source_files_properties(qml/theme/AppTheme.qml PROPERTIES
  QT_QML_SINGLETON_TYPE TRUE
)

# Opt in to newer QML CMake policies when supported by the installed Qt.
if(DEFINED QT_KNOWN_POLICY_QTP0001)
  qt_policy(SET QTP0001 NEW)
endif()
if(DEFINED QT_KNOWN_POLICY_QTP0004)
  qt_policy(SET QTP0004 NEW)
endif()

qt_add_executable(niconeon-ui
  src/main.cpp
  src/LicenseProvider.cpp
  src/mpv/MpvItem.cpp
  src/ipc/CoreClient.cpp
  src/danmaku/DanmakuController.cpp
  src/danmaku/DanmakuAtlasPacker.cpp
  src/danmaku/DanmakuSimdUpdater.cpp
  src/danmaku/DanmakuTextSpriteCache.cpp
  src/danmaku/DanmakuUpdateWorker.cpp
  src/danmaku/DanmakuSpatialGrid.cpp
  src/danmaku/DanmakuRenderNodeItem.cpp
)

target_include_directories(niconeon-ui PRIVATE
  src
  ${MPV_INCLUDE_DIRS}
)

target_link_libraries(niconeon-ui PRIVATE
  Qt6::Quick
  Qt6::Qml
  ${MPV_LIBRARIES}
)

qt_add_qml_module(niconeon-ui
  URI Niconeon
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/theme/AppTheme.qml
    qml/components/AppButton.qml
    qml/components/AppDialog.qml
    qml/components/AppTabButton.qml
    qml/components/AppTextArea.qml
    qml/components/AppTextField.qml
    qml/components/DanmakuOverlay.qml
    qml/components/NgDropZone.qml
    qml/components/Toast.qml
    qml/components/FilterDialog.qml
    qml/components/PlaybackSpeedDialog.qml
    qml/components/AboutDialog.qml
    qml/components/SettingsDialog.qml
    qml/components/FontSizeDialog.qml
)

set(app_license_file "${CMAKE_SOURCE_DIR}/../LICENSE")
set(distribution_license_file "${CMAKE_SOURCE_DIR}/../COPYING")
set(third_party_notices_file "${CMAKE_SOURCE_DIR}/../THIRD_PARTY_NOTICES.txt")
set_source_files_properties("${app_license_file}" PROPERTIES
  QT_RESOURCE_ALIAS "LICENSE"
)
set_source_files_properties("${distribution_license_file}" PROPERTIES
  QT_RESOURCE_ALIAS "COPYING"
)
set_source_files_properties("${third_party_notices_file}" PROPERTIES
  QT_RESOURCE_ALIAS "THIRD_PARTY_NOTICES.txt"
)

qt_add_resources(niconeon-ui "license_resources"
  PREFIX "/licenses"
  FILES
    "${app_license_file}"
    "${distribution_license_file}"
    "${third_party_notices_file}"
)

if(BUILD_TESTING)
  find_package(Qt6 6.4 REQUIRED COMPONENTS Test)

  qt_add_executable(niconeon-ui-fake-core
    tests/support/fake_core.cpp
  )

  target_link_libraries(niconeon-ui-fake-core PRIVATE
    Qt6::Core
  )

  set_target_properties(niconeon-ui-fake-core PROPERTIES
    OUTPUT_NAME niconeon-fake-core
  )

  qt_add_executable(niconeon-ui-unit
    tests/unit/spatial_grid_incremental_test.cpp
    src/danmaku/DanmakuSpatialGrid.cpp
  )

  target_include_directories(niconeon-ui-unit PRIVATE
    src
  )

  target_link_libraries(niconeon-ui-unit PRIVATE
    Qt6::Core
    Qt6::Test
  )

  add_test(NAME spatial_grid_incremental_test COMMAND niconeon-ui-unit)

  qt_add_executable(niconeon-ui-unit-core-client
    tests/unit/core_client_test.cpp
    src/ipc/CoreClient.cpp
  )

  target_include_directories(niconeon-ui-unit-core-client PRIVATE
    src
  )

  target_link_libraries(niconeon-ui-unit-core-client PRIVATE
    Qt6::Core
    Qt6::Test
  )

  add_dependencies(niconeon-ui-unit-core-client niconeon-ui-fake-core)
  add_test(NAME core_client_test COMMAND niconeon-ui-unit-core-client)
  set_tests_properties(core_client_test PROPERTIES
    ENVIRONMENT "NICONEON_FAKE_CORE_BIN=$<TARGET_FILE:niconeon-ui-fake-core>"
  )

  qt_add_executable(niconeon-ui-unit-danmaku-text-width
    tests/unit/danmaku_text_width_test.cpp
    src/danmaku/DanmakuAtlasPacker.cpp
    src/danmaku/DanmakuController.cpp
    src/danmaku/DanmakuSimdUpdater.cpp
    src/danmaku/DanmakuUpdateWorker.cpp
    src/danmaku/DanmakuSpatialGrid.cpp
    src/danmaku/DanmakuTextSpriteCache.cpp
  )

  target_include_directories(niconeon-ui-unit-danmaku-text-width PRIVATE
    src
  )

  target_link_libraries(niconeon-ui-unit-danmaku-text-width PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
  )

  add_test(NAME danmaku_text_width_test COMMAND niconeon-ui-unit-danmaku-text-width)
  set_tests_properties(danmaku_text_width_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )

  qt_add_executable(niconeon-ui-unit-danmaku-ng-drop
    tests/unit/danmaku_ng_drop_test.cpp
    src/danmaku/DanmakuAtlasPacker.cpp
    src/danmaku/DanmakuController.cpp
    src/danmaku/DanmakuSimdUpdater.cpp
    src/danmaku/DanmakuUpdateWorker.cpp
    src/danmaku/DanmakuSpatialGrid.cpp
    src/danmaku/DanmakuTextSpriteCache.cpp
  )

  target_include_directories(niconeon-ui-unit-danmaku-ng-drop PRIVATE
    src
  )

  target_link_libraries(niconeon-ui-unit-danmaku-ng-drop PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
  )

  add_test(NAME danmaku_ng_drop_test COMMAND niconeon-ui-unit-danmaku-ng-drop)
  set_tests_properties(danmaku_ng_drop_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )

  qt_add_executable(niconeon-ui-unit-danmaku-sprite-cache
    tests/unit/danmaku_sprite_cache_test.cpp
    src/danmaku/DanmakuAtlasPacker.cpp
    src/danmaku/DanmakuTextSpriteCache.cpp
  )

  target_include_directories(niconeon-ui-unit-danmaku-sprite-cache PRIVATE
    src
  )

  target_link_libraries(niconeon-ui-unit-danmaku-sprite-cache PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
  )

  add_test(NAME danmaku_sprite_cache_test COMMAND niconeon-ui-unit-danmaku-sprite-cache)
  set_tests_properties(danmaku_sprite_cache_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )
endif()

if(BUILD_TESTING AND NICONEON_BUILD_UI_E2E)

  qt_add_executable(niconeon-ui-e2e
    tests/e2e/rendernode_alignment_e2e.cpp
    src/danmaku/DanmakuAtlasPacker.cpp
    src/danmaku/DanmakuController.cpp
    src/danmaku/DanmakuSimdUpdater.cpp
    src/danmaku/DanmakuTextSpriteCache.cpp
    src/danmaku/DanmakuUpdateWorker.cpp
    src/danmaku/DanmakuSpatialGrid.cpp
    src/danmaku/DanmakuRenderNodeItem.cpp
  )

  target_include_directories(niconeon-ui-e2e PRIVATE
    src
  )

  target_link_libraries(niconeon-ui-e2e PRIVATE
    Qt6::Quick
    Qt6::Qml
    Qt6::Test
  )

  add_test(NAME rendernode_alignment_e2e COMMAND niconeon-ui-e2e)
endif()
