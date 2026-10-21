# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

add_library(ta_core STATIC
  crc32.cpp dicom.cpp export.cpp frame.cpp fs.cpp handoff.cpp json.cpp kv.cpp layout.cpp manifest.cpp
  normalize.cpp profile.cpp render.cpp staging.cpp tiff.cpp)
target_include_directories(ta_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ta_core PUBLIC ta_wire)
find_package(Threads REQUIRED)
target_link_libraries(ta_core PUBLIC Threads::Threads)
