# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

add_library(ta_wire STATIC ta_wire.c ta_kv.c)
target_include_directories(ta_wire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
