#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ta {

// An ordered set of key=value pairs in the ta_kv text format.
class Kv {
 public:
  static bool parse(std::string_view text, Kv& out, std::string* err = nullptr);
  static bool load(const std::filesystem::path& file, Kv& out, std::string* err = nullptr);

  std::string serialize() const;
  bool save(const std::filesystem::path& file, std::string* err = nullptr) const;

  void set(std::string_view key, std::string_view value);
  void setInt(std::string_view key, int64_t value);
  void setReal(std::string_view key, double value);
  void setBool(std::string_view key, bool value);

  std::optional<std::string> get(std::string_view key) const;
  std::string str(std::string_view key, std::string_view def = {}) const;
  int64_t integer(std::string_view key, int64_t def = 0) const;
  double real(std::string_view key, double def = 0) const;
  bool boolean(std::string_view key, bool def = false) const;

  const std::vector<std::pair<std::string, std::string>>& items() const { return items_; }

 private:
  std::vector<std::pair<std::string, std::string>> items_;
};

// Escape one value for a single-line record (journal lines use this too).
std::string kvEscape(std::string_view value);
std::optional<std::string> kvUnescape(std::string_view value);

}  // namespace ta
