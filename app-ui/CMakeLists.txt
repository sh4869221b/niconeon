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
  src/danmaku/DanmakuListModel.cpp
  src/danmaku/DanmakuController.cpp
  src/danmaku/DanmakuSpatialGrid.cpp
  src/danmaku/DanmakuSceneItem.cpp
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
    qml/components/VideoPlayer.qml
    qml/components/AppButton.qml
    qml/components/DanmakuOverlay.qml
    qml/components/DanmakuItem.qml
    qml/components/NgDropZone.qml
    qml/components/Toast.qml
    qml/components/FilterDialog.qml
    qml/components/PlaybackSpeedDialog.qml
    qml/components/AboutDialog.qml
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
