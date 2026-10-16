add_executable(ta_core_tests core_tests.cpp)
target_link_libraries(ta_core_tests PRIVATE ta_core)
add_test(NAME core COMMAND ta_core_tests)

if(TA_BUILD_APP)
  add_executable(ta_helper_tests helper_tests.cpp)
  target_link_libraries(ta_helper_tests PRIVATE ta_appcore)
  add_dependencies(ta_helper_tests ta_helper)
  target_compile_definitions(ta_helper_tests PRIVATE
    TA_HELPER_PATH="$<TARGET_FILE:ta_helper>")
  add_test(NAME helper COMMAND ta_helper_tests)
endif()
