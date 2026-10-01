cmake_minimum_required(VERSION 3.25)
project(niconeon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
include(CTest)
include(GNUInstallDirs)
include(cmake/ProjectOptions.cmake)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui OpenGL Quick Qml QuickControls2 Network Sql Concurrent)
# Qt 6.8 distributions export GuiPrivate from Gui; newer Qt installs may
# provide it as a separate package. Both require matching private headers.
if(NOT TARGET Qt6::GuiPrivate)
  find_package(Qt6 6.8 REQUIRED COMPONENTS GuiPrivate)
endif()
if(WIN32 AND MINGW)
  # MSYS2's mpv.pc exposes POSIX /ucrt64 paths, which native CMake cannot
  # consume as imported include directories. Resolve the same UCRT64 SDK
  # next to the selected compiler, using native paths throughout.
  get_filename_component(niconeon_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(niconeon_mingw_prefix "${niconeon_compiler_bin}" DIRECTORY)
  find_path(MPV_INCLUDE_DIR NAMES mpv/client.h HINTS "${niconeon_mingw_prefix}/include" REQUIRED)
  find_library(MPV_LIBRARY NAMES mpv libmpv HINTS "${niconeon_mingw_prefix}/lib" REQUIRED)
  add_library(niconeon_mpv UNKNOWN IMPORTED)
  set_target_properties(niconeon_mpv PROPERTIES IMPORTED_LOCATION "${MPV_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
  add_library(niconeon_mpv ALIAS PkgConfig::MPV)
endif()
qt_standard_project_setup(REQUIRES 6.8)

add_library(niconeon_domain STATIC
  src/domain/Domain.cpp
  src/filters/FilterEngine.cpp
  src/filters/FilterManager.cpp
  src/storage/Store.cpp
  src/comments/CommentTimeline.cpp
  src/comments/SyntheticComments.cpp
)
target_include_directories(niconeon_domain PUBLIC src)
target_link_libraries(niconeon_domain PUBLIC Qt6::Core Qt6::Sql niconeon_options)
if(WIN32)
  target_link_libraries(niconeon_domain PRIVATE shell32 ole32 uuid)
endif()

add_library(niconeon_render STATIC
  src/danmaku/DanmakuAtlasPacker.cpp
  src/danmaku/DanmakuController.cpp
  src/danmaku/DanmakuSimdUpdater.cpp
  src/danmaku/DanmakuTextSpriteCache.cpp
  src/danmaku/DanmakuUpdateWorker.cpp
  src/danmaku/DanmakuSpatialGrid.cpp
  src/danmaku/DanmakuRenderNodeItem.cpp
)
target_include_directories(niconeon_render PUBLIC src)
target_link_libraries(niconeon_render PUBLIC Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::Quick Qt6::Qml niconeon_options)
target_link_libraries(niconeon_render PRIVATE Qt6::GuiPrivate)

function(niconeon_license_resources target)
  foreach(name LICENSE COPYING THIRD_PARTY_NOTICES.txt)
    set_source_files_properties("${PROJECT_SOURCE_DIR}/${name}" PROPERTIES QT_RESOURCE_ALIAS "${name}")
  endforeach()
  qt_add_resources(${target} license_resources PREFIX /licenses FILES
    "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/COPYING" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt")
endfunction()

option(NICONEON_BUILD_APP "Build the desktop application and application integration tests" ON)
if(NICONEON_BUILD_APP)
  add_library(niconeon_services STATIC
    src/app/ApplicationController.cpp
    src/app/CommentService.cpp
    src/comments/NiconicoFetcher.cpp
  )
  target_include_directories(niconeon_services PUBLIC src)
  target_link_libraries(niconeon_services PUBLIC niconeon_domain niconeon_render niconeon_playback Qt6::Network Qt6::Concurrent niconeon_options)

  add_library(niconeon_playback STATIC src/playback/MpvItem.cpp)
  target_include_directories(niconeon_playback PUBLIC src)
  target_link_libraries(niconeon_playback PUBLIC Qt6::Quick Qt6::OpenGL niconeon_mpv niconeon_options)

  qt_add_executable(niconeon src/app/main.cpp src/ui/LicenseProvider.cpp)
  target_link_libraries(niconeon PRIVATE niconeon_services niconeon_playback Qt6::QuickControls2)

  set(niconeon_qml_files
    src/ui/qml/Main.qml
    src/ui/qml/theme/AppTheme.qml
    src/ui/qml/components/AppButton.qml
    src/ui/qml/components/AppDialog.qml
    src/ui/qml/components/AppTabButton.qml
    src/ui/qml/components/AppTextArea.qml
    src/ui/qml/components/AppTextField.qml
    src/ui/qml/components/DanmakuOverlay.qml
    src/ui/qml/components/NgDropZone.qml
    src/ui/qml/components/Toast.qml
    src/ui/qml/components/FilterDialog.qml
    src/ui/qml/components/PlaybackSpeedDialog.qml
    src/ui/qml/components/AboutDialog.qml
    src/ui/qml/components/SettingsDialog.qml
    src/ui/qml/components/FontSizeDialog.qml
  )
  foreach(qml_file IN LISTS niconeon_qml_files)
    file(RELATIVE_PATH alias "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/qml" "${CMAKE_CURRENT_SOURCE_DIR}/${qml_file}")
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${alias}")
  endforeach()
  set_source_files_properties(src/ui/qml/theme/AppTheme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
  qt_policy(SET QTP0001 NEW)
  qt_policy(SET QTP0004 NEW)
  qt_add_qml_module(niconeon URI Niconeon VERSION 1.0 RESOURCE_PREFIX /qt/qml QML_FILES ${niconeon_qml_files})
  # Distribution Qt builds load QML plugins dynamically. Static Qt builds still
  # need the import scanner to link their plugin targets into the executable.
  get_target_property(niconeon_qt_qml_type Qt6::Qml TYPE)
  if(niconeon_qt_qml_type STREQUAL "SHARED_LIBRARY")
    set_target_properties(niconeon PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
  endif()

  niconeon_license_resources(niconeon)
endif()

option(NICONEON_BUILD_UI_E2E "Build actual OpenGL renderer integration tests (requires a working display)" OFF)
if(BUILD_TESTING)
  find_package(Qt6 6.8 REQUIRED COMPONENTS Test QuickTest)
  include(cmake/Tests.cmake)
endif()

if(NICONEON_BUILD_APP)
  install(TARGETS niconeon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
  install(FILES LICENSE COPYING SOURCE_CODE.md THIRD_PARTY_NOTICES.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/niconeon)
  if(UNIX AND NOT APPLE)
    install(FILES packaging/appimage/niconeon.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/appimage/niconeon.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
  endif()

endif()

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format-19 clang-format)
if(CLANG_FORMAT_EXECUTABLE)
  add_custom_target(format-check COMMAND "${CMAKE_COMMAND}" -DCLANG_FORMAT=${CLANG_FORMAT_EXECUTABLE}
    -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/FormatCheck.cmake VERBATIM)
endif()

option(NICONEON_BUILD_PERF_TOOLS "Build deterministic GUI raster scheduling profiler" OFF)
if(NICONEON_BUILD_PERF_TOOLS)
  qt_add_executable(raster_profile tests/perf/raster_profile.cpp)
  target_link_libraries(raster_profile PRIVATE niconeon_render)
  set_target_properties(raster_profile PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
endif()
