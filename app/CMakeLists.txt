# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
set(CMAKE_AUTOMOC ON)

# The parts with no widgets, shared with the tests.
add_library(ta_appcore STATIC HelperClient.cpp Stager.cpp HandoffChannel.cpp Thumbnail.cpp
  HelperClient.hpp Stager.hpp HandoffChannel.hpp Thumbnail.hpp AppTypes.hpp)
target_include_directories(ta_appcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ta_appcore PUBLIC ta_core Qt6::Core Qt6::Gui Qt6::Network)

add_executable(twain_acquire WIN32
  main.cpp MainWindow.cpp ImageView.cpp MountView.cpp Dialogs.cpp ViewerWindow.cpp
  MainWindow.hpp ImageView.hpp MountView.hpp Dialogs.hpp ViewerWindow.hpp)
set_target_properties(twain_acquire PROPERTIES OUTPUT_NAME twain-acquire)
target_link_libraries(twain_acquire PRIVATE ta_appcore Qt6::Widgets)
target_compile_definitions(twain_acquire PRIVATE TA_VERSION="${TA_VERSION}")
# The application finds its helpers next to itself.
add_dependencies(twain_acquire ta_helper)
