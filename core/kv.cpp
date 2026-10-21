/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "kv.hpp"

#include <cstdio>
#include <cstdlib>

#include "fs.hpp"
#include "ta_kv.h"

namespace ta {

std::string kvEscape(std::string_view value) {
  std::string out(ta_kv_escaped_len(value.data(), value.size()), '\0');
  ta_kv_escape(value.data(), value.size(), out.data());
  return out;
}

std::optional<std::string> kvUnescape(std::string_view value) {
  std::string out(value.size(), '\0');
  size_t n = ta_kv_unescape(value.data(), value.size(), out.data());
  if (n == static_cast<size_t>(-1)) return std::nullopt;
  out.resize(n);
  return out;
}

bool Kv::parse(std::string_view text, Kv& out, std::string* err) {
  out.items_.clear();
  std::string key(128, '\0');
  std::string value(text.size() + 1, '\0');
  size_t pos = 0;
  int line = 0;
  for (;;) {
    ++line;
    int r = ta_kv_next(text.data(), text.size(), &pos, key.data(), key.size(), value.data(),
                       value.size());
    if (r == 0) return true;
    if (r < 0) {
      if (err) *err = "malformed key=value line near line " + std::to_string(line);
      return false;
    }
    out.set(key.c_str(), value.c_str());
  }
}

bool Kv::load(const std::filesystem::path& file, Kv& out, std::string* err) {
  std::string text;
  if (!fs::readText(file, text, err)) return false;
  return parse(text, out, err);
}

std::string Kv::serialize() const {
  std::string out;
  for (const auto& [k, v] : items_) {
    out += k;
    out += '=';
    out += kvEscape(v);
    out += '\n';
  }
  return out;
}

bool Kv::save(const std::filesystem::path& file, std::string* err) const {
  const std::string text = serialize();
  return fs::atomicWrite(file, text.data(), text.size(), err);
}

void Kv::set(std::string_view key, std::string_view value) {
  for (auto& item : items_) {
    if (item.first == key) {
      item.second = std::string(value);
      return;
    }
  }
  items_.emplace_back(std::string(key), std::string(value));
}

void Kv::setInt(std::string_view key, int64_t value) { set(key, std::to_string(value)); }

void Kv::setReal(std::string_view key, double value) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", value);
  set(key, buf);
}

void Kv::setBool(std::string_view key, bool value) { set(key, value ? "true" : "false"); }

std::optional<std::string> Kv::get(std::string_view key) const {
  for (const auto& item : items_)
    if (item.first == key) return item.second;
  return std::nullopt;
}

std::string Kv::str(std::string_view key, std::string_view def) const {
  auto v = get(key);
  return v ? *v : std::string(def);
}

int64_t Kv::integer(std::string_view key, int64_t def) const {
  auto v = get(key);
  if (!v || v->empty()) return def;
  char* end = nullptr;
  long long n = std::strtoll(v->c_str(), &end, 10);
  return (end && *end == '\0') ? n : def;
}

double Kv::real(std::string_view key, double def) const {
  auto v = get(key);
  if (!v || v->empty()) return def;
  char* end = nullptr;
  double d = std::strtod(v->c_str(), &end);
  return (end && *end == '\0') ? d : def;
}

bool Kv::boolean(std::string_view key, bool def) const {
  auto v = get(key);
  if (!v) return def;
  if (*v == "true" || *v == "1" || *v == "yes") return true;
  if (*v == "false" || *v == "0" || *v == "no") return false;
  return def;
}

}  // namespace ta
