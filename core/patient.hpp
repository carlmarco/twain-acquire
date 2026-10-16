#pragma once

#include <string>

namespace ta {

// What the operator or the calling program says about the patient. Nothing
// here is looked up; it is carried into the exported files as given.
struct PatientInfo {
  std::string id;
  std::string familyName;
  std::string givenName;
  std::string birthDate;  // YYYYMMDD or empty
  std::string sex;        // M, F, O or empty
  std::string accession;

  bool empty() const { return id.empty() && familyName.empty() && givenName.empty(); }
  std::string displayName() const;
};

}  // namespace ta
