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

qt_add_executable(niconeon-ui
  src/main.cpp
  src/mpv/MpvItem.cpp
  src/ipc/CoreClient.cpp
  src/danmaku/DanmakuController.cpp
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
    qml/components/DanmakuOverlay.qml
    qml/components/DanmakuItem.qml
    qml/components/NgDropZone.qml
    qml/components/Toast.qml
    qml/components/FilterDialog.qml
    qml/components/PlaybackSpeedDialog.qml
)
