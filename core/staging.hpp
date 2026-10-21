/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "frame.hpp"
#include "fs.hpp"
#include "patient.hpp"

namespace ta {

struct StagedImage {
  uint32_t seq = 0;
  std::string file;  // name inside the session directory
  int slot = -1;     // -1 = not placed
  Orientation orientation;
  bool discarded = false;
  uint32_t crc = 0;  // CRC-32 of the file bytes
  uint32_t width = 0, height = 0;
  int bitsStored = 16;
  double dpiX = 0, dpiY = 0;
  int64_t acquiredUnixMs = 0;
};

struct SessionState {
  std::string id;
  std::string layout;
  std::string source;
  int64_t startedUnixMs = 0;
  PatientInfo patient;
  std::vector<StagedImage> images;
  bool closed = false;
  std::string closeReason;

  const StagedImage* image(uint32_t seq) const;
};

// A crash-safe staging session. Each image is written to its own file (temp,
// flush, rename) before one journal line records it, and every later change is
// one more flushed journal line. After a crash or power loss, replaying the
// journal gives back every image that had been recorded.
class StagingSession {
 public:
  static std::unique_ptr<StagingSession> create(const fs::Path& root, const std::string& layout,
                                                const std::string& source, const PatientInfo& patient,
                                                std::string* err = nullptr);
  static std::unique_ptr<StagingSession> resume(const fs::Path& dir, std::string* err = nullptr);

  // Session directories under `root` that were never closed, newest first.
  static std::vector<fs::Path> listOpen(const fs::Path& root);

  // Read-only access for other threads (the export worker).
  static bool readState(const fs::Path& dir, SessionState& out, std::string* err = nullptr);
  static bool readImage(const fs::Path& dir, const StagedImage& img, Frame& out,
                        std::string* err = nullptr);

  bool addImage(const Frame& f, int slot, uint32_t* seqOut = nullptr, std::string* err = nullptr);
  bool place(uint32_t seq, int slot, std::string* err = nullptr);
  bool setOrientation(uint32_t seq, const Orientation& o, std::string* err = nullptr);
  bool discard(uint32_t seq, std::string* err = nullptr);
  bool setPatient(const PatientInfo& p, std::string* err = nullptr);
  bool close(const std::string& reason, std::string* err = nullptr);

  bool loadImage(uint32_t seq, Frame& out, std::string* err = nullptr) const;

  const SessionState& state() const { return state_; }
  const fs::Path& dir() const { return dir_; }

 private:
  StagingSession() = default;
  bool record(const std::string& op, const std::vector<std::pair<std::string, std::string>>& fields,
              std::string* err);

  fs::Path dir_;
  SessionState state_;
};

}  // namespace ta
