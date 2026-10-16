#pragma once

#include <string>
#include <vector>

#include "patient.hpp"

namespace ta {

// How a practice program passes the current patient on the command line.
//
// A template is a sequence of flags and placeholders, for example
//   /PATID {id} /NAME {given} /SURNAME {family}
// A flag followed by a placeholder means "the argument after this flag is that
// field". Flags match case-insensitively and also as "flag=value" or
// "flag:value". A placeholder with no flag before it takes the next argument
// that is not a flag, in order.
//
// Fields: {id} {family} {given} {birth} {sex} {accession}, {file} for the path
// of a handoff file (read with a HandoffFileMapping), and {skip} for an
// argument to ignore. {ssn} is accepted so a template can name it, and the
// value is always thrown away.
struct HandoffTemplate {
  std::string id;
  std::string name;
  std::string pattern;
  std::string dateFormat = "YYYYMMDD";  // YYYYMMDD, YYYY-MM-DD, MM/DD/YYYY, DD.MM.YYYY, DD/MM/YYYY
};

std::vector<HandoffTemplate> handoffPresets();

struct HandoffResult {
  PatientInfo patient;
  bool matched = false;              // at least one field came from the arguments
  std::string file;                  // {file}: a handoff file to read as well
  std::vector<std::string> problems; // e.g. a date that does not fit the format
};

// Where each field sits in a handoff file of key=value lines, optionally under
// [section] headers. Each entry is "Key" or "Section/Key", matched without
// regard to case; empty means the file does not carry that field.
// `name` holds the whole name in one value: "Family, Given" and
// "Family^Given" split at the separator; otherwise the last word is the
// family name.
struct HandoffFileMapping {
  std::string id, family, given, name, birth, sex, accession;
  std::string dateFormat = "YYYYMMDD";
  bool any() const {
    return !(id.empty() && family.empty() && given.empty() && name.empty() && birth.empty() && sex.empty() &&
             accession.empty());
  }
};

// Handoff files are written by other programs in UTF-8 or in the Windows
// code page; returns UTF-8 with any byte-order mark removed. Bytes that are
// not valid UTF-8 are read as Windows-1252.
std::string decodeHandoffText(const std::string& bytes);

HandoffResult parseHandoffFile(const std::string& utf8, const HandoffFileMapping& m);

// Fill every empty field of `into` from `from`.
void fillBlanks(PatientInfo& into, const PatientInfo& from);

// Parse `args` (the program's arguments, without the program name).
HandoffResult parseHandoff(const HandoffTemplate& t, const std::vector<std::string>& args);

// Convert a date written in `format` to YYYYMMDD; empty when it does not fit.
std::string convertDate(const std::string& value, const std::string& format);

// Normalise sex to M, F or O; empty when unrecognised.
std::string normaliseSex(const std::string& value);

}  // namespace ta
