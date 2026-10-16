add_library(ta_wire STATIC ta_wire.c ta_kv.c)
target_include_directories(ta_wire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
