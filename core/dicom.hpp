#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "frame.hpp"
#include "patient.hpp"

namespace ta {

struct DicomCode {
  std::string value;
  std::string scheme;
  std::string meaning;
};

struct DicomStudy {
  std::string studyUid;
  std::string seriesUid;
  std::string studyDate;  // YYYYMMDD
  std::string studyTime;  // HHMMSS
  std::string studyId;
  std::string referringPhysician;
  int seriesNumber = 1;
};

struct DicomImage {
  std::string sopUid;
  int instanceNumber = 1;
  int64_t creationUnixMs = 0;  // Instance Creation Date/Time; fixed so a retried export is byte-identical
  std::string contentDate;  // YYYYMMDD
  std::string contentTime;  // HHMMSS
  bool forProcessing = false;
  std::string imageComments;  // e.g. the mount slot label
  std::vector<DicomCode> anatomicRegion;
  std::vector<DicomCode> primaryStructures;  // e.g. teeth
  std::string manufacturer;
  std::string modelName;
  std::string institution;
  std::string softwareVersion;
  std::string pixelIntensityRelationship = "LIN";
  int pixelIntensitySign = 1;
  double windowCenter = 0;  // width <= 0 = omit the window
  double windowWidth = 0;
};

// A Part 10 file (explicit VR little endian) holding one Digital Intra-Oral
// X-Ray image. `f` is written as given; orient it first if required.
std::vector<uint8_t> encodeDicom(const Frame& f, const PatientInfo& patient, const DicomStudy& study,
                                 const DicomImage& image);

// A new UID. With an empty root it is a UUID-derived "2.25." UID, which needs
// no registered root; otherwise root + "." + time + "." + random, at most 64 chars.
std::string makeUid(const std::string& root = {});

// DA and TM values for a Unix time in local time.
std::string dicomDate(int64_t unixMs);
std::string dicomTime(int64_t unixMs);

}  // namespace ta
