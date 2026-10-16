#include "layout.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <system_error>

namespace ta {

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  std::istringstream in(s);
  while (std::getline(in, cur, sep)) out.push_back(cur);
  if (!s.empty() && s.back() == sep) out.emplace_back();
  return out;
}

LayoutSlot slot(const char* label, int col, int row, int turns = 0) {
  LayoutSlot s;
  s.label = label;
  s.column = col;
  s.row = row;
  s.orientation.quarterTurns = turns;
  return s;
}

}  // namespace

std::vector<DicomCode> parseCodes(const std::string& s) {
  std::vector<DicomCode> out;
  if (s.empty()) return out;
  for (const auto& item : split(s, ';')) {
    const auto parts = split(item, '^');
    if (parts.size() == 3) out.push_back({parts[0], parts[1], parts[2]});
  }
  return out;
}

std::string formatCodes(const std::vector<DicomCode>& codes) {
  std::string out;
  for (const auto& c : codes) {
    if (!out.empty()) out += ';';
    out += c.value + "^" + c.scheme + "^" + c.meaning;
  }
  return out;
}

int Layout::nextEmpty(int from, const std::vector<bool>& filled) const {
  const int n = int(slotList.size());
  for (int i = 1; i <= n; ++i) {
    const int k = ((from + i) % n + n) % n;
    if (k >= int(filled.size()) || !filled[size_t(k)]) return k;
  }
  return -1;
}

Kv Layout::toKv() const {
  Kv kv;
  kv.set("id", id);
  kv.set("name", name);
  kv.setInt("columns", columns);
  kv.setInt("rows", rows);
  for (size_t i = 0; i < slotList.size(); ++i) {
    const LayoutSlot& s = slotList[i];
    const std::string key = "slot." + std::to_string(i + 1);
    kv.set(key, s.label + "|" + std::to_string(s.column) + "|" + std::to_string(s.row) + "|" +
                    std::to_string(s.orientation.quarterTurns) + "|" + (s.orientation.flipH ? "1" : "0"));
    if (!s.region.empty()) kv.set(key + ".region", formatCodes(s.region));
    if (!s.structures.empty()) kv.set(key + ".structures", formatCodes(s.structures));
  }
  return kv;
}

bool Layout::fromKv(const Kv& kv, Layout& out, std::string* err) {
  out = Layout{};
  out.id = kv.str("id");
  out.name = kv.str("name", out.id);
  out.columns = int(std::clamp<int64_t>(kv.integer("columns", 1), 1, 32));
  out.rows = int(std::clamp<int64_t>(kv.integer("rows", 1), 1, 32));
  for (int i = 1; i <= 256; ++i) {
    const std::string key = "slot." + std::to_string(i);
    auto v = kv.get(key);
    if (!v) break;
    const auto parts = split(*v, '|');
    if (parts.size() < 3) {
      if (err) *err = key + " needs label|column|row";
      return false;
    }
    LayoutSlot s;
    s.label = parts[0];
    s.column = std::clamp(std::atoi(parts[1].c_str()), 0, out.columns - 1);
    s.row = std::clamp(std::atoi(parts[2].c_str()), 0, out.rows - 1);
    if (parts.size() > 3) s.orientation.quarterTurns = ((std::atoi(parts[3].c_str()) % 4) + 4) % 4;
    if (parts.size() > 4) s.orientation.flipH = parts[4] == "1";
    s.region = parseCodes(kv.str(key + ".region"));
    s.structures = parseCodes(kv.str(key + ".structures"));
    out.slotList.push_back(std::move(s));
  }
  if (out.id.empty() || out.slotList.empty()) {
    if (err) *err = "layout needs an id and at least one slot";
    return false;
  }
  return true;
}

std::vector<Layout> builtinLayouts() {
  std::vector<Layout> out;

  Layout single{"single", "Single image", 1, 1, {slot("Image", 0, 0)}};
  out.push_back(single);

  Layout bw2{"bitewings-2", "Bitewings (2)", 2, 1,
             {slot("Right bitewing", 0, 0), slot("Left bitewing", 1, 0)}};
  out.push_back(bw2);

  Layout bw4{"bitewings-4", "Bitewings (4)", 4, 1,
             {slot("Right molar bitewing", 0, 0), slot("Right premolar bitewing", 1, 0),
              slot("Left premolar bitewing", 2, 0), slot("Left molar bitewing", 3, 0)}};
  out.push_back(bw4);

  Layout fmx{"fmx-18", "Full mouth series (18)", 7, 3, {}};
  const char* upper[] = {"Upper right molars", "Upper right premolars", "Upper right canine",
                         "Upper incisors",     "Upper left canine",     "Upper left premolars",
                         "Upper left molars"};
  const char* bite[] = {"Right molar bitewing", "Right premolar bitewing", "Left premolar bitewing",
                        "Left molar bitewing"};
  const int biteCols[] = {0, 1, 5, 6};
  const char* lower[] = {"Lower right molars", "Lower right premolars", "Lower right canine",
                         "Lower incisors",     "Lower left canine",     "Lower left premolars",
                         "Lower left molars"};
  for (int i = 0; i < 7; ++i) fmx.slotList.push_back(slot(upper[i], i, 0));
  for (int i = 0; i < 4; ++i) fmx.slotList.push_back(slot(bite[i], biteCols[i], 1));
  for (int i = 0; i < 7; ++i) fmx.slotList.push_back(slot(lower[i], i, 2));
  out.push_back(fmx);

  return out;
}

std::vector<Layout> loadLayouts(const fs::Path& dir, std::vector<std::string>* problems) {
  std::vector<Layout> out = builtinLayouts();
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return out;
  std::vector<fs::Path> files;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    if (e.path().extension() == ".layout") files.push_back(e.path());
  std::sort(files.begin(), files.end());
  for (const auto& file : files) {
    Kv kv;
    Layout l;
    std::string err;
    if (!Kv::load(file, kv, &err) || !Layout::fromKv(kv, l, &err)) {
      if (problems) problems->push_back(fs::toUtf8(file.filename()) + ": " + err);
      continue;
    }
    auto it = std::find_if(out.begin(), out.end(), [&](const Layout& x) { return x.id == l.id; });
    if (it != out.end()) *it = l;
    else out.push_back(l);
  }
  return out;
}

}  // namespace ta
