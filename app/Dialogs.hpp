#pragma once

#include <QDialog>
#include <QString>
#include <vector>

#include "export.hpp"
#include "profile.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QSpinBox;

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
  QCheckBox *showUI_, *modalUI_, *disarmEach_, *invert_, *flip_, *autoAdvance_;
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
  QLineEdit *name_, *folder_, *pattern_, *uidRoot_, *institution_, *manufacturer_, *model_;
  QComboBox *format_, *relationship_, *sign_;
  QCheckBox *enabled_, *orient_, *scale_, *window_;
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
  QLineEdit* dsmPath_;
};
