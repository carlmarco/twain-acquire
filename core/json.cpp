/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ta {

namespace {

constexpr int kMaxDepth = 64;

class Parser {
 public:
  explicit Parser(std::string_view t) : t_(t) {}

  bool document(Json& out, std::string* err) {
    if (!value(out, 0)) {
      if (err) *err = why_ + " at offset " + std::to_string(at_);
      return false;
    }
    ws();
    if (at_ != t_.size()) {
      if (err) *err = "unexpected text after the value at offset " + std::to_string(at_);
      return false;
    }
    return true;
  }

 private:
  bool fail(const char* why) {
    if (why_.empty()) why_ = why;
    return false;
  }
  void ws() {
    while (at_ < t_.size() && (t_[at_] == ' ' || t_[at_] == '\t' || t_[at_] == '\n' || t_[at_] == '\r')) ++at_;
  }
  bool literal(std::string_view word) {
    if (t_.substr(at_, word.size()) != word) return fail("unknown literal");
    at_ += word.size();
    return true;
  }

  bool value(Json& out, int depth) {
    if (depth > kMaxDepth) return fail("nested too deeply");
    ws();
    if (at_ >= t_.size()) return fail("unexpected end");
    const char c = t_[at_];
    if (c == '{') return object(out, depth);
    if (c == '[') return array(out, depth);
    if (c == '"') {
      out.type = Json::Type::String;
      return string(out.string);
    }
    if (c == 't') {
      out.type = Json::Type::Bool;
      out.boolean = true;
      return literal("true");
    }
    if (c == 'f') {
      out.type = Json::Type::Bool;
      out.boolean = false;
      return literal("false");
    }
    if (c == 'n') {
      out.type = Json::Type::Null;
      return literal("null");
    }
    return number(out);
  }

  bool object(Json& out, int depth) {
    out.type = Json::Type::Object;
    ++at_;
    ws();
    if (at_ < t_.size() && t_[at_] == '}') {
      ++at_;
      return true;
    }
    for (;;) {
      ws();
      if (at_ >= t_.size() || t_[at_] != '"') return fail("expected a member name");
      std::string key;
      if (!string(key)) return false;
      ws();
      if (at_ >= t_.size() || t_[at_] != ':') return fail("expected ':'");
      ++at_;
      Json v;
      if (!value(v, depth + 1)) return false;
      out.object.emplace_back(std::move(key), std::move(v));
      ws();
      if (at_ < t_.size() && t_[at_] == ',') {
        ++at_;
        continue;
      }
      if (at_ < t_.size() && t_[at_] == '}') {
        ++at_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  bool array(Json& out, int depth) {
    out.type = Json::Type::Array;
    ++at_;
    ws();
    if (at_ < t_.size() && t_[at_] == ']') {
      ++at_;
      return true;
    }
    for (;;) {
      Json v;
      if (!value(v, depth + 1)) return false;
      out.array.push_back(std::move(v));
      ws();
      if (at_ < t_.size() && t_[at_] == ',') {
        ++at_;
        continue;
      }
      if (at_ < t_.size() && t_[at_] == ']') {
        ++at_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  static void utf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
      s += char(cp);
    } else if (cp < 0x800) {
      s += char(0xC0 | (cp >> 6));
      s += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      s += char(0xE0 | (cp >> 12));
      s += char(0x80 | ((cp >> 6) & 0x3F));
      s += char(0x80 | (cp & 0x3F));
    } else {
      s += char(0xF0 | (cp >> 18));
      s += char(0x80 | ((cp >> 12) & 0x3F));
      s += char(0x80 | ((cp >> 6) & 0x3F));
      s += char(0x80 | (cp & 0x3F));
    }
  }

  bool hex4(uint32_t& v) {
    if (at_ + 4 > t_.size()) return fail("short \\u escape");
    v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = t_[at_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else return fail("bad \\u escape");
    }
    return true;
  }

  bool string(std::string& out) {
    ++at_;  // opening quote
    for (;;) {
      if (at_ >= t_.size()) return fail("unterminated string");
      const char c = t_[at_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in a string");
      if (c != '\\') {
        out += c;
        continue;
      }
      if (at_ >= t_.size()) return fail("unterminated string");
      const char e = t_[at_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00) {
            uint32_t lo;
            if (t_.substr(at_, 2) != "\\u") return fail("unpaired surrogate");
            at_ += 2;
            if (!hex4(lo) || lo < 0xDC00 || lo >= 0xE000) return fail("unpaired surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp < 0xE000) {
            return fail("unpaired surrogate");
          }
          utf8(out, cp);
          break;
        }
        default:
          return fail("bad escape");
      }
    }
  }

  bool number(Json& out) {
    const size_t start = at_;
    if (at_ < t_.size() && t_[at_] == '-') ++at_;
    auto digits = [&] {
      const size_t s = at_;
      while (at_ < t_.size() && t_[at_] >= '0' && t_[at_] <= '9') ++at_;
      return at_ > s;
    };
    if (!digits()) return fail("expected a value");
    if (at_ < t_.size() && t_[at_] == '.') {
      ++at_;
      if (!digits()) return fail("bad number");
    }
    if (at_ < t_.size() && (t_[at_] == 'e' || t_[at_] == 'E')) {
      ++at_;
      if (at_ < t_.size() && (t_[at_] == '+' || t_[at_] == '-')) ++at_;
      if (!digits()) return fail("bad number");
    }
    const std::string text(t_.substr(start, at_ - start));
    out.type = Json::Type::Number;
    out.number = std::strtod(text.c_str(), nullptr);
    if (!std::isfinite(out.number)) return fail("number out of range");
    return true;
  }

  std::string_view t_;
  size_t at_ = 0;
  std::string why_;
};

}  // namespace

const Json* Json::find(std::string_view key) const {
  if (type != Type::Object) return nullptr;
  for (const auto& [k, v] : object)
    if (k == key) return &v;
  return nullptr;
}

std::string Json::str(std::string_view key, std::string_view def) const {
  const Json* v = find(key);
  return v && v->type == Type::String ? v->string : std::string(def);
}

int64_t Json::integer(std::string_view key, int64_t def) const {
  const Json* v = find(key);
  if (!v || v->type != Type::Number || v->number < -9.0e15 || v->number > 9.0e15) return def;
  return int64_t(std::llround(v->number));
}

bool Json::flag(std::string_view key, bool def) const {
  const Json* v = find(key);
  return v && v->type == Type::Bool ? v->boolean : def;
}

bool Json::parse(std::string_view text, Json& out, std::string* err) {
  out = Json{};
  if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);  // UTF-8 BOM
  return Parser(text).document(out, err);
}

std::string jsonQuote(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", unsigned(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

void JsonWriter::prefix(std::string_view key) {
  if (!first_.empty()) {
    if (!first_.back()) out_ += ',';
    first_.back() = false;
    out_ += '\n';
    out_.append(first_.size() * 2, ' ');
  }
  if (!key.empty()) out_ += jsonQuote(key) + ": ";
}

void JsonWriter::beginObject() { beginObject({}); }

void JsonWriter::beginObject(std::string_view key) {
  prefix(key);
  out_ += '{';
  first_.push_back(true);
}

void JsonWriter::endObject() {
  const bool empty = first_.back();
  first_.pop_back();
  if (!empty) {
    out_ += '\n';
    out_.append(first_.size() * 2, ' ');
  }
  out_ += '}';
  if (first_.empty()) out_ += '\n';
}

void JsonWriter::beginArray(std::string_view key) {
  prefix(key);
  out_ += '[';
  first_.push_back(true);
}

void JsonWriter::endArray() {
  const bool empty = first_.back();
  first_.pop_back();
  if (!empty) {
    out_ += '\n';
    out_.append(first_.size() * 2, ' ');
  }
  out_ += ']';
}

void JsonWriter::str(std::string_view key, std::string_view value) {
  prefix(key);
  out_ += jsonQuote(value);
}

void JsonWriter::integer(std::string_view key, int64_t value) {
  prefix(key);
  out_ += std::to_string(value);
}

void JsonWriter::flag(std::string_view key, bool value) {
  prefix(key);
  out_ += value ? "true" : "false";
}

void JsonWriter::null(std::string_view key) {
  prefix(key);
  out_ += "null";
}

}  // namespace ta
