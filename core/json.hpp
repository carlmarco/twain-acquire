/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ta {

// A JSON value. Small and strict: enough for the files this application
// writes and reads back, not a general-purpose library.
struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Json> array;
  std::vector<std::pair<std::string, Json>> object;  // in file order

  // Member of an object, or nullptr.
  const Json* find(std::string_view key) const;
  std::string str(std::string_view key, std::string_view def = {}) const;
  int64_t integer(std::string_view key, int64_t def = 0) const;
  bool flag(std::string_view key, bool def = false) const;

  static bool parse(std::string_view text, Json& out, std::string* err = nullptr);
};

// Writes JSON text with two-space indentation.
class JsonWriter {
 public:
  void beginObject();
  void endObject();
  void beginArray(std::string_view key = {});
  void endArray();
  void beginObject(std::string_view key);
  void str(std::string_view key, std::string_view value);
  void integer(std::string_view key, int64_t value);
  void flag(std::string_view key, bool value);
  void null(std::string_view key);
  const std::string& text() const { return out_; }

 private:
  void prefix(std::string_view key);
  std::string out_;
  std::vector<bool> first_;  // per open container: nothing written yet
};

std::string jsonQuote(std::string_view s);

}  // namespace ta
