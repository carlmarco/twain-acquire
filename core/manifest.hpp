/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "frame.hpp"
#include "fs.hpp"
#include "layout.hpp"
#include "patient.hpp"

namespace ta {

// A mount file (<name>.mount.json) written next to exported images. It
// records which file sits in which slot of which layout, so an exported
// session can be opened again and shown as it was taken.
struct ManifestImage {
  std::string file;          // name in the same folder
  uint32_t seq = 0;          // order taken in the session
  int slot = -1;             // 0-based; -1 = not placed
  Orientation orientation;   // as set by the operator
  bool orientationApplied = false;  // already turned in the file's pixels
  std::string acquired;      // local time, "YYYY-MM-DDTHH:MM:SS"; may be empty
};

struct MountManifest {
  static constexpr int kVersion = 1;

  int version = kVersion;
  std::string software;
  std::string session;
  std::string source;
  std::string exported;      // local time, "YYYY-MM-DDTHH:MM:SS"
  PatientInfo patient;
  Layout layout;
  std::vector<ManifestImage> images;
};

constexpr const char* kManifestSuffix = ".mount.json";

std::string encodeManifest(const MountManifest& m);
// Refuses a file of another format or a newer version, and any image whose
// file name is not a plain name in the same folder.
bool decodeManifest(const std::string& text, MountManifest& out, std::string* err = nullptr);
bool readManifest(const fs::Path& file, MountManifest& out, std::string* err = nullptr);

// Mount files in `folder`, newest first.
std::vector<fs::Path> listManifests(const fs::Path& folder);
// Image files in `folder` this application can read (.dcm, .tif, .tiff), by name.
std::vector<fs::Path> listImageFiles(const fs::Path& folder);
// Reads a DICOM or TIFF file written by this application.
bool readImageFile(const fs::Path& file, Frame& out, std::string* err = nullptr);

std::string localIsoTime(int64_t unixMs);

}  // namespace ta
