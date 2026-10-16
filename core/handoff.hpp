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
// Fields: {id} {family} {given} {birth} {sex} {accession}, and {skip} for an
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
  std::vector<std::string> problems; // e.g. a date that does not fit the format
};

// Parse `args` (the program's arguments, without the program name).
HandoffResult parseHandoff(const HandoffTemplate& t, const std::vector<std::string>& args);

// Convert a date written in `format` to YYYYMMDD; empty when it does not fit.
std::string convertDate(const std::string& value, const std::string& format);

// Normalise sex to M, F or O; empty when unrecognised.
std::string normaliseSex(const std::string& value);

}  // namespace ta
