/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <QImage>

#include "frame.hpp"

constexpr int kThumbSize = 384;

// A small 8-bit rendering of a frame, turned as given, with the automatic
// display window. Safe to call from any thread.
QImage renderThumbnail(const ta::Frame& f, const ta::Orientation& o, int size = kThumbSize);
