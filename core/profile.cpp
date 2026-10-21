/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "profile.hpp"

#include <algorithm>
#include <system_error>

namespace ta {

namespace {

SourceProfile::Transfer parseTransfer(const std::string& s) {
  if (s == "memory") return SourceProfile::Transfer::Memory;
  if (s == "file") return SourceProfile::Transfer::File;
  if (s == "native") return SourceProfile::Transfer::Native;
  return SourceProfile::Transfer::Auto;
}

const char* byteOrderName(NormalizeOptions::ByteOrder b) {
  switch (b) {
    case NormalizeOptions::ByteOrder::Little: return "little";
    case NormalizeOptions::ByteOrder::Big: return "big";
    default: return "auto";
  }
}

const char* polarityName(NormalizeOptions::Polarity p) {
  switch (p) {
    case NormalizeOptions::Polarity::MinIsBlack: return "min-is-black";
    case NormalizeOptions::Polarity::MinIsWhite: return "min-is-white";
    default: return "auto";
  }
}

// File names are derived from the source name; keep only portable characters.
std::string safeName(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.';
    out += ok ? char(c) : '_';
  }
  return out.empty() ? "default" : out;
}

}  // namespace

const char* transferName(SourceProfile::Transfer t) {
  switch (t) {
    case SourceProfile::Transfer::Memory: return "memory";
    case SourceProfile::Transfer::File: return "file";
    case SourceProfile::Transfer::Native: return "native";
    default: return "auto";
  }
}

Kv SourceProfile::toKv() const {
  Kv kv;
  kv.set("source", source);
  kv.setInt("helper.bits", helperBits);
  kv.set("transfer", transferName(transfer));
  kv.setInt("bit-depth", bitDepth);
  kv.setBool("show-ui", showUI);
  kv.setBool("modal-ui", modalUI);
  kv.setInt("transfer-count", transferCount);
  kv.set("after-batch", afterBatch == AfterBatch::Disarm ? "disarm" : afterBatch == AfterBatch::Stay ? "stay" : "rearm");
  kv.set("byte-order", byteOrderName(normalize.byteOrder));
  kv.set("polarity", polarityName(normalize.polarity));
  kv.setInt("bits-stored", normalize.bitsStored);
  kv.setBool("invert", normalize.invert);
  kv.setReal("dpi-override", normalize.dpiOverride);
  kv.setInt("timeout.call-ms", callTimeoutMs);
  kv.setInt("timeout.arm-ms", armTimeoutMs);
  kv.setInt("orientation.quarter-turns", defaultOrientation.quarterTurns);
  kv.setBool("orientation.flip", defaultOrientation.flipH);
  kv.setBool("auto-advance", autoAdvance);
  return kv;
}

SourceProfile SourceProfile::fromKv(const Kv& kv) {
  SourceProfile p;
  p.source = kv.str("source");
  p.helperBits = kv.integer("helper.bits", 32) == 64 ? 64 : 32;
  p.transfer = parseTransfer(kv.str("transfer", "auto"));
  const int64_t bd = kv.integer("bit-depth", 0);
  p.bitDepth = (bd == 8 || bd == 16) ? int(bd) : 0;
  p.showUI = kv.boolean("show-ui", true);
  p.modalUI = kv.boolean("modal-ui", false);
  p.transferCount = int(kv.integer("transfer-count", -1));
  const std::string ab = kv.str("after-batch", kv.boolean("disarm-after-each-image", false) ? "disarm" : "rearm");
  p.afterBatch = ab == "disarm" ? AfterBatch::Disarm : ab == "stay" ? AfterBatch::Stay : AfterBatch::Rearm;
  const std::string bo = kv.str("byte-order", "auto");
  p.normalize.byteOrder = bo == "little" ? NormalizeOptions::ByteOrder::Little
                          : bo == "big"  ? NormalizeOptions::ByteOrder::Big
                                         : NormalizeOptions::ByteOrder::Auto;
  const std::string pol = kv.str("polarity", "auto");
  p.normalize.polarity = pol == "min-is-black"   ? NormalizeOptions::Polarity::MinIsBlack
                         : pol == "min-is-white" ? NormalizeOptions::Polarity::MinIsWhite
                                                 : NormalizeOptions::Polarity::Auto;
  const int64_t bs = kv.integer("bits-stored", 0);
  p.normalize.bitsStored = (bs >= 8 && bs <= 16) ? int(bs) : 0;
  p.normalize.invert = kv.boolean("invert", false);
  p.normalize.dpiOverride = std::max(0.0, kv.real("dpi-override", 0));
  p.callTimeoutMs = int(std::max<int64_t>(0, kv.integer("timeout.call-ms", 20000)));
  p.armTimeoutMs = int(std::max<int64_t>(0, kv.integer("timeout.arm-ms", 0)));
  p.defaultOrientation.quarterTurns = int(((kv.integer("orientation.quarter-turns", 0) % 4) + 4) % 4);
  p.defaultOrientation.flipH = kv.boolean("orientation.flip", false);
  p.autoAdvance = kv.boolean("auto-advance", true);
  return p;
}

fs::Path SourceProfile::pathFor(const fs::Path& dir, const std::string& source) {
  return dir / fs::fromUtf8(safeName(source) + ".profile");
}

SourceProfile SourceProfile::load(const fs::Path& dir, const std::string& source) {
  Kv kv;
  SourceProfile p;
  if (Kv::load(pathFor(dir, source), kv)) p = fromKv(kv);
  p.source = source;
  return p;
}

bool SourceProfile::save(const fs::Path& dir, std::string* err) const {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return toKv().save(pathFor(dir, source), err);
}

}  // namespace ta
