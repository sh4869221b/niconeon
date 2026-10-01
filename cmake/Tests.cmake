set(niconeon_test_environment
  "NICONEON_SYNTHETIC_COMMENTS=unset:"
  "NICONEON_AUTO_VIDEO_PATH=unset:"
  "NICONEON_AUTO_PERF_LOG=unset:"
  "NICONEON_AUTO_EXIT_MS=unset:"
  "NICONEON_NICONICO_COOKIE=unset:"
  "NICONICO_COOKIE=unset:")

function(niconeon_test name source)
  qt_add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE ${ARGN} Qt6::Test niconeon_options)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 60 LABELS unit
    ENVIRONMENT_MODIFICATION "${niconeon_test_environment}"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
  get_target_property(qt_qml_type Qt6::Qml TYPE)
  if(qt_qml_type STREQUAL "SHARED_LIBRARY")
    set_target_properties(${name} PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
  endif()
endfunction()

niconeon_test(spatial_grid_incremental_test tests/unit/spatial_grid_incremental_test.cpp niconeon_render)
niconeon_test(danmaku_text_width_test tests/unit/danmaku_text_width_test.cpp niconeon_render)
niconeon_test(danmaku_ng_drop_test tests/unit/danmaku_ng_drop_test.cpp niconeon_render)
niconeon_test(danmaku_sprite_cache_test tests/unit/danmaku_sprite_cache_test.cpp niconeon_render)
niconeon_test(danmaku_raster_pipeline_test tests/unit/danmaku_raster_pipeline_test.cpp niconeon_render)
niconeon_test(license_resource_test tests/unit/license_resource_test.cpp Qt6::Core)
niconeon_license_resources(license_resource_test)

# Source lists are explicit so adding a test always requires an intentional build change.
niconeon_test(graphics_environment_test tests/unit/graphics_environment_test.cpp niconeon_domain)
niconeon_test(domain_test tests/unit/domain_test.cpp niconeon_domain)
niconeon_test(filter_manager_test tests/unit/filter_manager_test.cpp niconeon_domain)
niconeon_test(comment_timeline_test tests/unit/comment_timeline_test.cpp niconeon_domain)
niconeon_test(store_test tests/unit/store_test.cpp niconeon_domain)
if(NICONEON_BUILD_APP)
  niconeon_test(comment_service_test tests/unit/comment_service_test.cpp niconeon_services)
  niconeon_test(service_regression_test tests/unit/service_regression_test.cpp niconeon_services)
  niconeon_test(niconico_fetcher_test tests/unit/niconico_fetcher_test.cpp niconeon_services)
  niconeon_test(application_controller_test tests/unit/application_controller_test.cpp niconeon_services)

  qt_add_executable(qml_views_test tests/qml/quicktest.cpp src/ui/qml/niconeon_qml.qrc src/ui/LicenseProvider.cpp)
  target_link_libraries(qml_views_test PRIVATE niconeon_services niconeon_playback Qt6::QuickTest Qt6::QuickControls2)
  target_compile_definitions(qml_views_test PRIVATE QUICK_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}/tests/qml")
  get_target_property(qt_qml_type Qt6::Qml TYPE)
  if(qt_qml_type STREQUAL "SHARED_LIBRARY")
    set_target_properties(qml_views_test PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
  endif()
  niconeon_license_resources(qml_views_test)
  add_test(NAME qml_views_test COMMAND qml_views_test -input "${PROJECT_SOURCE_DIR}/tests/qml")
  set_tests_properties(qml_views_test PROPERTIES TIMEOUT 60 LABELS qml
    ENVIRONMENT_MODIFICATION "${niconeon_test_environment}"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

endif()

if(NICONEON_BUILD_UI_E2E)
  qt_add_executable(rendernode_alignment_e2e tests/e2e/rendernode_alignment_e2e.cpp)
  target_link_libraries(rendernode_alignment_e2e PRIVATE niconeon_render Qt6::Test)
  get_target_property(qt_qml_type Qt6::Qml TYPE)
  if(qt_qml_type STREQUAL "SHARED_LIBRARY")
    set_target_properties(rendernode_alignment_e2e PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
  endif()
  add_test(NAME rendernode_alignment_e2e COMMAND rendernode_alignment_e2e)
  set_tests_properties(rendernode_alignment_e2e PROPERTIES TIMEOUT 60 LABELS "e2e;opengl"
    ENVIRONMENT_MODIFICATION "${niconeon_test_environment}"
    ENVIRONMENT "QSG_RHI_BACKEND=opengl;LIBGL_ALWAYS_SOFTWARE=1;NICONEON_REQUIRE_OPENGL=1")
  if(NICONEON_BUILD_APP)
    qt_add_executable(video_overlay_e2e tests/e2e/video_overlay_e2e.cpp)
    target_link_libraries(video_overlay_e2e PRIVATE niconeon_playback niconeon_render Qt6::Test)
    if(qt_qml_type STREQUAL "SHARED_LIBRARY")
      set_target_properties(video_overlay_e2e PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
    endif()
    add_test(NAME video_overlay_e2e COMMAND video_overlay_e2e)
    set_tests_properties(video_overlay_e2e PROPERTIES TIMEOUT 90 LABELS "e2e;opengl"
      ENVIRONMENT_MODIFICATION "${niconeon_test_environment}"
      ENVIRONMENT "QSG_RHI_BACKEND=opengl;LIBGL_ALWAYS_SOFTWARE=1;NICONEON_REQUIRE_OPENGL=1")
  endif()
endif()

if(UNIX AND NOT APPLE)
  find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)
  add_test(NAME packaging_smoke_test COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/packaging/package_smoke_test.py")
  set_tests_properties(packaging_smoke_test PROPERTIES TIMEOUT 30 LABELS packaging)
  if(NICONEON_BUILD_APP)
    add_test(NAME application_exit_test COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/packaging/application_exit_test.py" "$<TARGET_FILE:niconeon>")
    set_tests_properties(application_exit_test PROPERTIES TIMEOUT 20 LABELS "qml;integration")
  endif()
endif()
