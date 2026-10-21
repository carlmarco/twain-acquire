/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <string>

#include "frame.hpp"
#include "fs.hpp"
#include "kv.hpp"
#include "normalize.hpp"

namespace ta {

// Everything that can be set per TWAIN source. Defaults follow what the
// source reports; each field exists because some source needs it changed.
struct SourceProfile {
  std::string source;    // TWAIN product name
  int helperBits = 32;   // which helper loads this source: 32 or 64

  // Negotiation
  enum class Transfer { Auto, Memory, File, Native } transfer = Transfer::Auto;
  int bitDepth = 0;      // 0 = keep the source's default; 8 or 16 = request it
  bool showUI = true;    // let the source show its own window; some refuse to arm without it
  bool modalUI = false;
  int transferCount = -1;  // CAP_XFERCOUNT; -1 = as many as the source offers
  // When the source has delivered a batch and waits, still enabled:
  // re-arm for the next empty slot (closes and reopens the source), disarm,
  // or stay armed for more exposures.
  enum class AfterBatch { Rearm, Disarm, Stay } afterBatch = AfterBatch::Rearm;

  // Normalisation
  NormalizeOptions normalize;

  // Timeouts (milliseconds; 0 = none)
  int callTimeoutMs = 20000;   // any single request to the helper
  int armTimeoutMs = 0;        // disarm when no exposure arrives within this time

  // Placement
  Orientation defaultOrientation;
  bool autoAdvance = true;     // move to the next empty slot after each image

  Kv toKv() const;
  static SourceProfile fromKv(const Kv& kv);

  static fs::Path pathFor(const fs::Path& dir, const std::string& source);
  static SourceProfile load(const fs::Path& dir, const std::string& source);
  bool save(const fs::Path& dir, std::string* err = nullptr) const;
};

const char* transferName(SourceProfile::Transfer t);

}  // namespace ta
