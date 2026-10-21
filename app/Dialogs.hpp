/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <QDialog>
#include <QString>
#include <vector>

#include "export.hpp"
#include "layout.hpp"
#include "profile.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QTableWidget;

// Every per-source setting. Defaults follow the source; change only what a
// particular source gets wrong.
class ProfileDialog : public QDialog {
  Q_OBJECT
 public:
  ProfileDialog(const ta::SourceProfile& profile, QWidget* parent = nullptr);
  ta::SourceProfile profile() const;

 private:
  ta::SourceProfile base_;
  QComboBox *helperBits_, *transfer_, *bitDepth_, *byteOrder_, *polarity_, *turns_;
  QCheckBox *showUI_, *modalUI_, *invert_, *flip_, *autoAdvance_;
  QComboBox* afterBatch_;
  QSpinBox *count_, *bitsStored_, *callTimeout_, *armTimeout_;
  QDoubleSpinBox* dpi_;
};

class TargetDialog : public QDialog {
  Q_OBJECT
 public:
  TargetDialog(const ta::ExportTarget& target, QWidget* parent = nullptr);
  ta::ExportTarget target() const;

 private:
  ta::ExportTarget base_;
  QLineEdit *name_, *folder_, *pattern_, *subfolder_, *uidRoot_, *institution_, *manufacturer_, *model_;
  QComboBox *format_, *relationship_, *sign_;
  QCheckBox *enabled_, *orient_, *scale_, *window_, *window8_, *manifest_;
};

// The list of export destinations.
class TargetsDialog : public QDialog {
  Q_OBJECT
 public:
  TargetsDialog(std::vector<ta::ExportTarget> targets, QWidget* parent = nullptr);
  std::vector<ta::ExportTarget> targets() const { return targets_; }

 private:
  void refresh();
  std::vector<ta::ExportTarget> targets_;
  QListWidget* list_;
};

struct AppOptions {
  bool simulatedSources = false;
  bool requirePatientId = true;
  bool disarmWhenFull = true;
  QString dsmPath;   // empty = the system TWAINDSM.dll
  int sourceBits = 32;  // which TWAIN sources to search: 32, 64, or 0 for both
  bool dsmLog = false;  // ask the DSM to write its own diagnostic log
  QString handoffPreset = "twain-acquire";  // a preset id, or "custom"
  QString handoffPattern;                    // used when the preset is "custom"
  QString handoffDateFormat = "YYYYMMDD";
  // Handoff file: where each field sits ("Key" or "Section/Key"), and an
  // optional file to watch for the current patient.
  QString fileId, fileFamily, fileGiven, fileName, fileBirth, fileSex, fileAccession;
  QString fileDateFormat = "YYYYMMDD";
  QString watchFile;
  bool deleteFileAfterRead = false;
  int retentionDays = 7;  // delete exported sessions after this many days; 0 = keep
};

class OptionsDialog : public QDialog {
  Q_OBJECT
 public:
  OptionsDialog(const AppOptions& options, const QString& dataDir, QWidget* parent = nullptr);
  AppOptions options() const;

 private:
  QCheckBox *sim_, *requireId_, *disarmFull_, *dsmLog_;
  QComboBox *sourceBits_, *handoffPreset_, *handoffDate_;
  QLineEdit* handoffPattern_;
  QLineEdit *fileId_, *fileFamily_, *fileGiven_, *fileName_, *fileBirth_, *fileSex_, *fileAccession_, *watchFile_;
  QComboBox* fileDate_;
  QCheckBox* deleteFile_;
  QSpinBox* retention_;
  QLineEdit* dsmPath_;
};

// How each slot of a mount turns the images placed in it, and whether the
// whole mount is shown mirrored. Saved as a .layout file that replaces the
// built-in one of the same id.
class MountDialog : public QDialog {
  Q_OBJECT
 public:
  MountDialog(const ta::Layout& layout, bool customised, QWidget* parent = nullptr);
  ta::Layout layout() const;
  bool restoreDefaults() const { return restore_; }

 private:
  ta::Layout base_;
  QTableWidget* table_;
  QCheckBox* mirror_;
  bool restore_ = false;
};
