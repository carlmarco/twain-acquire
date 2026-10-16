cmake_minimum_required(VERSION 3.21)
project(twain-acquire VERSION 0.1.2 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(TA_BUILD_APP "Build the Qt application" ON)
option(TA_BUILD_TESTS "Build the tests" ON)
option(TA_HELPER_ONLY "Build only the acquisition helper (use for the 32-bit helper)" OFF)
set(TWAIN_INCLUDE_DIR "" CACHE PATH "Directory containing twain.h (Windows TWAIN helper)")

# Pass -DTA_VERSION=... to override (the release build passes the tag).
if(NOT TA_VERSION)
  set(TA_VERSION "${PROJECT_VERSION}")
endif()

# A helper-only build (the 32-bit helper) carries its C runtime inside, so it
# needs no separate runtime installed for its bitness.
if(TA_HELPER_ONLY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# The application and its helpers share one output folder.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  add_compile_options(/W4 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_subdirectory(wire)
add_subdirectory(helper)

if(NOT TA_HELPER_ONLY)
  add_subdirectory(core)
  if(TA_BUILD_APP)
    add_subdirectory(app)
  endif()
  if(TA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
  endif()
endif()
