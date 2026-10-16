#include "handoff.hpp"

#include <algorithm>
#include <cctype>
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

}  // namespace ta
