/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "handoff.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>

namespace ta {

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool isPlaceholder(const std::string& tok) { return tok.size() > 2 && tok.front() == '{' && tok.back() == '}'; }

bool looksLikeFlag(const std::string& arg) {
  return arg.size() > 1 && (arg[0] == '/' || arg[0] == '-') && !std::isdigit(static_cast<unsigned char>(arg[1]));
}

struct Slot {
  std::string flag;   // lower-cased; empty = positional
  std::string field;  // id, family, given, birth, sex, accession, skip, ssn
};

void assign(HandoffResult& r, const HandoffTemplate& t, const std::string& field, const std::string& value) {
  if (value.empty() || field == "skip" || field == "ssn") return;
  if (field == "file") {
    r.file = value;
    return;
  }
  PatientInfo& p = r.patient;
  if (field == "id") p.id = value;
  else if (field == "family") p.familyName = value;
  else if (field == "given") p.givenName = value;
  else if (field == "accession") p.accession = value;
  else if (field == "sex") {
    p.sex = normaliseSex(value);
    if (p.sex.empty()) r.problems.push_back("unrecognised sex value");
  } else if (field == "birth") {
    p.birthDate = convertDate(value, t.dateFormat);
    if (p.birthDate.empty()) r.problems.push_back("birth date does not match " + t.dateFormat);
  } else {
    return;
  }
  r.matched = true;
}

}  // namespace

std::vector<HandoffTemplate> handoffPresets() {
  return {
      {"twain-acquire", "twain-acquire flags",
       "--patient-id {id} --family {family} --given {given} --birth {birth} --sex {sex} --accession {accession}",
       "YYYYMMDD"},
      // As documented by Open Dental for its RayMage program link.
      {"raymage", "Open Dental, RayMage-style link", "/PATID {id} /NAME {given} /SURNAME {family}", "YYYYMMDD"},
  };
}

std::string convertDate(const std::string& value, const std::string& format) {
  std::string digits;
  for (char c : value)
    if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
  if (digits.size() != 8) return {};
  std::string y, m, d;
  if (format == "YYYYMMDD" || format == "YYYY-MM-DD") {
    y = digits.substr(0, 4);
    m = digits.substr(4, 2);
    d = digits.substr(6, 2);
  } else if (format == "MM/DD/YYYY") {
    m = digits.substr(0, 2);
    d = digits.substr(2, 2);
    y = digits.substr(4, 4);
  } else if (format == "DD.MM.YYYY" || format == "DD/MM/YYYY") {
    d = digits.substr(0, 2);
    m = digits.substr(2, 2);
    y = digits.substr(4, 4);
  } else {
    return {};
  }
  const int mi = std::stoi(m), di = std::stoi(d), yi = std::stoi(y);
  if (yi < 1850 || yi > 2200 || mi < 1 || mi > 12 || di < 1 || di > 31) return {};
  return y + m + d;
}

std::string normaliseSex(const std::string& value) {
  const std::string v = lower(value);
  if (v == "m" || v == "male") return "M";
  if (v == "f" || v == "female") return "F";
  if (v == "o" || v == "other" || v == "u" || v == "unknown") return "O";
  return {};
}

HandoffResult parseHandoff(const HandoffTemplate& t, const std::vector<std::string>& args) {
  HandoffResult r;
  std::vector<Slot> slots;
  {
    std::istringstream in(t.pattern);
    std::string tok, pendingFlag;
    while (in >> tok) {
      if (isPlaceholder(tok)) {
        slots.push_back({lower(pendingFlag), lower(tok.substr(1, tok.size() - 2))});
        pendingFlag.clear();
      } else {
        pendingFlag = tok;
      }
    }
  }

  std::vector<std::string> positionalFields;
  for (const auto& s : slots)
    if (s.flag.empty()) positionalFields.push_back(s.field);
  size_t nextPositional = 0;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    const std::string la = lower(arg);
    bool used = false;
    for (const auto& s : slots) {
      if (s.flag.empty()) continue;
      if (la == s.flag) {
        // A flag followed directly by another known flag has no value.
        const bool nextIsFlag = i + 1 < args.size() && std::any_of(slots.begin(), slots.end(), [&](const Slot& o) {
          return !o.flag.empty() && lower(args[i + 1]) == o.flag;
        });
        if (i + 1 < args.size() && !nextIsFlag) assign(r, t, s.field, args[++i]);
        used = true;
        break;
      }
      if (la.size() > s.flag.size() && la.compare(0, s.flag.size(), s.flag) == 0 &&
          (la[s.flag.size()] == '=' || la[s.flag.size()] == ':')) {
        assign(r, t, s.field, arg.substr(s.flag.size() + 1));
        used = true;
        break;
      }
    }
    if (used) continue;
    if (!looksLikeFlag(arg) && nextPositional < positionalFields.size())
      assign(r, t, positionalFields[nextPositional++], arg);
  }
  return r;
}

namespace {

bool validUtf8(const std::string& s) {
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t len;
    uint32_t min;
    if ((c & 0xE0) == 0xC0) { len = 2; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; min = 0x10000; }
    else return false;
    if (i + len > n) return false;
    uint32_t cp = c & (0xFFu >> (len + 1));
    for (size_t k = 1; k < len; ++k) {
      const unsigned char b = static_cast<unsigned char>(s[i + k]);
      if ((b & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF) return false;
    i += len;
  }
  return true;
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

// Windows-1252 0x80..0x9F; the rest of the range matches Latin-1.
const uint16_t kCp1252High[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                  0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
                                  0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                  0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::string unquote(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
    return s.substr(1, s.size() - 2);
  return s;
}

}  // namespace

std::string decodeHandoffText(const std::string& bytes) {
  std::string s = bytes;
  if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
      static_cast<unsigned char>(s[2]) == 0xBF)
    s.erase(0, 3);
  if (validUtf8(s)) return s;
  std::string out;
  for (unsigned char c : s) {
    if (c >= 0x80 && c < 0xA0) appendUtf8(out, kCp1252High[c - 0x80]);
    else appendUtf8(out, c);
  }
  return out;
}

HandoffResult parseHandoffFile(const std::string& utf8, const HandoffFileMapping& m) {
  // "section/key" (lower case) -> value, plus "key" alone for the first occurrence.
  std::vector<std::pair<std::string, std::string>> values;
  std::istringstream in(utf8);
  std::string line, section;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = lower(trim(line.substr(1, line.size() - 2)));
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = lower(trim(line.substr(0, eq)));
    const std::string value = unquote(trim(line.substr(eq + 1)));
    values.emplace_back(section + "/" + key, value);
  }
  auto lookup = [&](const std::string& spec) -> std::string {
    if (spec.empty()) return {};
    const std::string want = lower(trim(spec));
    const bool qualified = want.find('/') != std::string::npos;
    for (const auto& [k, v] : values) {
      if (qualified ? k == want : k.substr(k.find('/') + 1) == want) return v;
    }
    return {};
  };

  HandoffResult r;
  HandoffTemplate t;  // only its date format is used by assign()
  t.dateFormat = m.dateFormat;
  assign(r, t, "id", lookup(m.id));
  assign(r, t, "family", lookup(m.family));
  assign(r, t, "given", lookup(m.given));
  assign(r, t, "birth", lookup(m.birth));
  assign(r, t, "sex", lookup(m.sex));
  assign(r, t, "accession", lookup(m.accession));
  const std::string whole = trim(lookup(m.name));
  if (!whole.empty()) {
    std::string fam, giv;
    const size_t sep = whole.find_first_of(",^");
    if (sep != std::string::npos) {
      fam = trim(whole.substr(0, sep));
      giv = trim(whole.substr(sep + 1));
    } else {
      const size_t sp = whole.find_last_of(' ');
      fam = sp == std::string::npos ? whole : trim(whole.substr(sp + 1));
      giv = sp == std::string::npos ? std::string() : trim(whole.substr(0, sp));
    }
    if (r.patient.familyName.empty()) assign(r, t, "family", fam);
    if (r.patient.givenName.empty()) assign(r, t, "given", giv);
  }
  return r;
}

void fillBlanks(PatientInfo& into, const PatientInfo& from) {
  if (into.id.empty()) into.id = from.id;
  if (into.familyName.empty()) into.familyName = from.familyName;
  if (into.givenName.empty()) into.givenName = from.givenName;
  if (into.birthDate.empty()) into.birthDate = from.birthDate;
  if (into.sex.empty()) into.sex = from.sex;
  if (into.accession.empty()) into.accession = from.accession;
}

}  // namespace ta
