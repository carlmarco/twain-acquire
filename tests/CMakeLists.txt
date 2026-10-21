# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
