#pragma once

#include <string>
#include <vector>

#include "dicom.hpp"
#include "frame.hpp"
#include "fs.hpp"
#include "kv.hpp"

namespace ta {

struct LayoutSlot {
  std::string label;
  int column = 0;
  int row = 0;
  Orientation orientation;              // turn applied to images placed here
  std::vector<DicomCode> region;        // optional, written to Anatomic Region Sequence
  std::vector<DicomCode> structures;    // optional, written to Primary Anatomic Structure Sequence
};

// A mount: a grid of labelled slots that images are placed into.
struct Layout {
  std::string id;
  std::string name;
  int columns = 1;
  int rows = 1;
  std::vector<LayoutSlot> slotList;
  bool mirrored = false;  // show columns right-to-left (the other viewing convention)

  // Index of the first slot after `from` (wrapping) not in `filled`, or -1.
  int nextEmpty(int from, const std::vector<bool>& filled) const;
  // Column a slot is drawn in, after mirroring.
  int displayColumn(const LayoutSlot& s) const { return mirrored ? columns - 1 - s.column : s.column; }

  Kv toKv() const;
  static bool fromKv(const Kv& kv, Layout& out, std::string* err = nullptr);
};

// Codes as "value^scheme^meaning;value^scheme^meaning".
std::vector<DicomCode> parseCodes(const std::string& s);
std::string formatCodes(const std::vector<DicomCode>& codes);

std::vector<Layout> builtinLayouts();

// Built-in layouts followed by every *.layout file in `dir`. A file whose id
// matches a built-in replaces it.
std::vector<Layout> loadLayouts(const fs::Path& dir, std::vector<std::string>* problems = nullptr);

}  // namespace ta
