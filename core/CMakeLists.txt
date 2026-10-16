add_library(ta_core STATIC
  crc32.cpp dicom.cpp export.cpp frame.cpp fs.cpp kv.cpp layout.cpp
  normalize.cpp profile.cpp render.cpp staging.cpp tiff.cpp)
target_include_directories(ta_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ta_core PUBLIC ta_wire)
find_package(Threads REQUIRED)
target_link_libraries(ta_core PUBLIC Threads::Threads)
