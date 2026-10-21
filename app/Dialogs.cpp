/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "Dialogs.hpp"

#include "handoff.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QHeaderView>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <memory>

namespace {

QComboBox* combo(const QStringList& labels, const QVariantList& values, const QVariant& current) {
  auto* c = new QComboBox;
  for (int i = 0; i < labels.size(); ++i) c->addItem(labels[i], values[i]);
  const int at = c->findData(current);
  c->setCurrentIndex(at < 0 ? 0 : at);
  return c;
}

QSpinBox* spin(int lo, int hi, int value, const QString& special = {}) {
  auto* s = new QSpinBox;
  s->setRange(lo, hi);
  s->setValue(value);
  if (!special.isEmpty()) s->setSpecialValueText(special);
  return s;
}

QCheckBox* check(const QString& text, bool on) {
  auto* c = new QCheckBox(text);
  c->setChecked(on);
  return c;
}

QDialogButtonBox* okCancel(QDialog* d) {
  auto* b = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QObject::connect(b, &QDialogButtonBox::accepted, d, &QDialog::accept);
  QObject::connect(b, &QDialogButtonBox::rejected, d, &QDialog::reject);
  return b;
}

}  // namespace

// ---- ProfileDialog --------------------------------------------------------

ProfileDialog::ProfileDialog(const ta::SourceProfile& p, QWidget* parent) : QDialog(parent), base_(p) {
  setWindowTitle(tr("Source settings: %1").arg(QString::fromStdString(p.source)));
  using T = ta::SourceProfile::Transfer;
  using B = ta::NormalizeOptions::ByteOrder;
  using P = ta::NormalizeOptions::Polarity;

  helperBits_ = combo({tr("32-bit"), tr("64-bit")}, {32, 64}, p.helperBits);
  transfer_ = combo({tr("Automatic (memory, then file, then native)"), tr("Memory"), tr("File (TIFF)"), tr("Native")},
                    {int(T::Auto), int(T::Memory), int(T::File), int(T::Native)}, int(p.transfer));
  bitDepth_ = combo({tr("Source default"), tr("8 bits"), tr("16 bits")}, {0, 8, 16}, p.bitDepth);
  showUI_ = check(tr("Show the source's own window"), p.showUI);
  modalUI_ = check(tr("Source window is modal"), p.modalUI);
  count_ = spin(-1, 99, p.transferCount, tr("As many as offered"));
  using AB = ta::SourceProfile::AfterBatch;
  afterBatch_ = combo({tr("Re-arm for the next empty slot"), tr("Disarm"), tr("Stay armed")},
                      {int(AB::Rearm), int(AB::Disarm), int(AB::Stay)}, int(p.afterBatch));

  byteOrder_ = combo({tr("As reported"), tr("Little-endian"), tr("Big-endian")},
                     {int(B::Auto), int(B::Little), int(B::Big)}, int(p.normalize.byteOrder));
  polarity_ = combo({tr("As reported"), tr("0 is black"), tr("0 is white")},
                    {int(P::Auto), int(P::MinIsBlack), int(P::MinIsWhite)}, int(p.normalize.polarity));
  bitsStored_ = spin(7, 16, p.normalize.bitsStored ? p.normalize.bitsStored : 7, tr("As transferred"));
  invert_ = check(tr("Invert grey scale"), p.normalize.invert);
  dpi_ = new QDoubleSpinBox;
  dpi_->setRange(0, 10000);
  dpi_->setDecimals(1);
  dpi_->setValue(p.normalize.dpiOverride);
  dpi_->setSpecialValueText(tr("As reported"));
  dpi_->setSuffix(tr(" dpi"));

  callTimeout_ = spin(0, 300, p.callTimeoutMs / 1000, tr("None"));
  callTimeout_->setSuffix(tr(" s"));
  armTimeout_ = spin(0, 3600, p.armTimeoutMs / 1000, tr("None"));
  armTimeout_->setSuffix(tr(" s"));

  turns_ = combo({tr("None"), tr("90° clockwise"), tr("180°"), tr("90° anticlockwise")}, {0, 1, 2, 3},
                 p.defaultOrientation.quarterTurns);
  flip_ = check(tr("Mirror"), p.defaultOrientation.flipH);
  autoAdvance_ = check(tr("Move to the next empty slot after each image"), p.autoAdvance);

  auto* neg = new QFormLayout;
  neg->addRow(tr("Helper"), helperBits_);
  neg->addRow(tr("Transfer"), transfer_);
  neg->addRow(tr("Bit depth"), bitDepth_);
  neg->addRow(tr("Images per arming"), count_);
  neg->addRow(showUI_);
  neg->addRow(modalUI_);
  neg->addRow(tr("After each batch"), afterBatch_);
  auto* g1 = new QGroupBox(tr("Negotiation"));
  g1->setLayout(neg);

  auto* norm = new QFormLayout;
  norm->addRow(tr("16-bit byte order"), byteOrder_);
  norm->addRow(tr("Polarity"), polarity_);
  norm->addRow(tr("Significant bits"), bitsStored_);
  norm->addRow(tr("Resolution"), dpi_);
  norm->addRow(invert_);
  auto* g2 = new QGroupBox(tr("Image corrections"));
  g2->setLayout(norm);

  auto* rest = new QFormLayout;
  rest->addRow(tr("Request timeout"), callTimeout_);
  rest->addRow(tr("Disarm if no exposure within"), armTimeout_);
  rest->addRow(tr("Default rotation"), turns_);
  rest->addRow(flip_);
  rest->addRow(autoAdvance_);
  auto* g3 = new QGroupBox(tr("Timing and placement"));
  g3->setLayout(rest);

  auto* v = new QVBoxLayout(this);
  v->addWidget(g1);
  v->addWidget(g2);
  v->addWidget(g3);
  v->addWidget(okCancel(this));
}

ta::SourceProfile ProfileDialog::profile() const {
  ta::SourceProfile p = base_;
  p.helperBits = helperBits_->currentData().toInt();
  p.transfer = ta::SourceProfile::Transfer(transfer_->currentData().toInt());
  p.bitDepth = bitDepth_->currentData().toInt();
  p.showUI = showUI_->isChecked();
  p.modalUI = modalUI_->isChecked();
  p.transferCount = count_->value();
  p.afterBatch = ta::SourceProfile::AfterBatch(afterBatch_->currentData().toInt());
  p.normalize.byteOrder = ta::NormalizeOptions::ByteOrder(byteOrder_->currentData().toInt());
  p.normalize.polarity = ta::NormalizeOptions::Polarity(polarity_->currentData().toInt());
  p.normalize.bitsStored = bitsStored_->value() >= 8 ? bitsStored_->value() : 0;
  p.normalize.invert = invert_->isChecked();
  p.normalize.dpiOverride = dpi_->value();
  p.callTimeoutMs = callTimeout_->value() * 1000;
  p.armTimeoutMs = armTimeout_->value() * 1000;
  p.defaultOrientation.quarterTurns = turns_->currentData().toInt();
  p.defaultOrientation.flipH = flip_->isChecked();
  p.autoAdvance = autoAdvance_->isChecked();
  return p;
}

// ---- TargetDialog ---------------------------------------------------------

TargetDialog::TargetDialog(const ta::ExportTarget& t, QWidget* parent) : QDialog(parent), base_(t) {
  setWindowTitle(tr("Export destination"));
  using F = ta::ExportTarget::Format;
  name_ = new QLineEdit(QString::fromStdString(t.name));
  enabled_ = check(tr("Enabled"), t.enabled);
  format_ = combo({tr("DICOM, for presentation"), tr("DICOM, for processing"), tr("TIFF, 16-bit"), tr("TIFF, 8-bit")},
                  {int(F::DicomPresentation), int(F::DicomProcessing), int(F::Tiff16), int(F::Tiff8)}, int(t.format));
  folder_ = new QLineEdit(QString::fromStdString(ta::fs::toUtf8(t.folder)));
  auto* browse = new QPushButton(tr("Browse…"));
  connect(browse, &QPushButton::clicked, this, [this] {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Export folder"), folder_->text());
    if (!dir.isEmpty()) folder_->setText(dir);
  });
  auto* folderRow = new QHBoxLayout;
  folderRow->addWidget(folder_);
  folderRow->addWidget(browse);
  pattern_ = new QLineEdit(QString::fromStdString(t.fileNamePattern));
  pattern_->setToolTip(tr("Tokens: {patient-id} {family} {given} {date} {time} {seq} {slot} {session} {uid}"));
  orient_ = check(tr("Apply rotation and mirroring to the pixels"), t.applyOrientation);
  scale_ = check(tr("16-bit TIFF: stretch to the full 16-bit range"), t.scaleToFullRange);
  window8_ = check(tr("8-bit TIFF: apply the automatic display window (as on screen)"), t.windowFor8Bit);

  uidRoot_ = new QLineEdit(QString::fromStdString(t.uidRoot));
  uidRoot_->setPlaceholderText(tr("empty = UUID-derived (2.25.…)"));
  institution_ = new QLineEdit(QString::fromStdString(t.institution));
  manufacturer_ = new QLineEdit(QString::fromStdString(t.manufacturer));
  model_ = new QLineEdit(QString::fromStdString(t.modelName));
  relationship_ = combo({"LIN", "LOG", "OTHER"}, {"LIN", "LOG", "OTHER"}, QString::fromStdString(t.intensityRelationship));
  sign_ = combo({"+1", "-1"}, {1, -1}, t.intensitySign);
  window_ = check(tr("Write a display window"), t.writeWindow);

  auto* f = new QFormLayout;
  f->addRow(tr("Name"), name_);
  f->addRow(enabled_);
  f->addRow(tr("Format"), format_);
  f->addRow(tr("Folder"), folderRow);
  f->addRow(tr("File name"), pattern_);
  f->addRow(orient_);
  f->addRow(scale_);
  f->addRow(window8_);
  auto* dicom = new QFormLayout;
  dicom->addRow(tr("UID root"), uidRoot_);
  dicom->addRow(tr("Institution"), institution_);
  dicom->addRow(tr("Manufacturer"), manufacturer_);
  dicom->addRow(tr("Model"), model_);
  dicom->addRow(tr("Pixel intensity relationship"), relationship_);
  dicom->addRow(tr("Relationship sign"), sign_);
  dicom->addRow(window_);
  auto* g = new QGroupBox(tr("DICOM"));
  g->setLayout(dicom);

  auto* v = new QVBoxLayout(this);
  v->addLayout(f);
  v->addWidget(g);
  auto* buttons = okCancel(this);
  disconnect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    if (folder_->text().trimmed().isEmpty()) {
      QMessageBox::warning(this, windowTitle(), tr("Choose a folder for this destination."));
      return;
    }
    accept();
  });
  v->addWidget(buttons);
}

ta::ExportTarget TargetDialog::target() const {
  ta::ExportTarget t = base_;
  if (t.id.empty()) t.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
  t.name = name_->text().trimmed().toStdString();
  if (t.name.empty()) t.name = folder_->text().trimmed().toStdString();
  t.enabled = enabled_->isChecked();
  t.format = ta::ExportTarget::Format(format_->currentData().toInt());
  t.folder = ta::fs::fromUtf8(folder_->text().trimmed().toStdString());
  t.fileNamePattern = pattern_->text().trimmed().toStdString();
  t.applyOrientation = orient_->isChecked();
  t.scaleToFullRange = scale_->isChecked();
  t.windowFor8Bit = window8_->isChecked();
  t.uidRoot = uidRoot_->text().trimmed().toStdString();
  t.institution = institution_->text().trimmed().toStdString();
  t.manufacturer = manufacturer_->text().trimmed().toStdString();
  t.modelName = model_->text().trimmed().toStdString();
  t.intensityRelationship = relationship_->currentData().toString().toStdString();
  t.intensitySign = sign_->currentData().toInt();
  t.writeWindow = window_->isChecked();
  return t;
}

// ---- TargetsDialog --------------------------------------------------------

TargetsDialog::TargetsDialog(std::vector<ta::ExportTarget> targets, QWidget* parent)
    : QDialog(parent), targets_(std::move(targets)) {
  setWindowTitle(tr("Export destinations"));
  list_ = new QListWidget;
  auto* add = new QPushButton(tr("Add…"));
  auto* edit = new QPushButton(tr("Edit…"));
  auto* remove = new QPushButton(tr("Remove"));
  connect(add, &QPushButton::clicked, this, [this] {
    TargetDialog d(ta::ExportTarget{}, this);
    if (d.exec() == QDialog::Accepted) {
      targets_.push_back(d.target());
      refresh();
    }
  });
  auto editCurrent = [this] {
    const int row = list_->currentRow();
    if (row < 0) return;
    TargetDialog d(targets_[size_t(row)], this);
    if (d.exec() == QDialog::Accepted) {
      targets_[size_t(row)] = d.target();
      refresh();
    }
  };
  connect(edit, &QPushButton::clicked, this, editCurrent);
  connect(list_, &QListWidget::itemDoubleClicked, this, editCurrent);
  connect(remove, &QPushButton::clicked, this, [this] {
    const int row = list_->currentRow();
    if (row < 0) return;
    targets_.erase(targets_.begin() + row);
    refresh();
  });
  auto* side = new QVBoxLayout;
  side->addWidget(add);
  side->addWidget(edit);
  side->addWidget(remove);
  side->addStretch();
  auto* h = new QHBoxLayout;
  h->addWidget(list_, 1);
  h->addLayout(side);
  auto* v = new QVBoxLayout(this);
  v->addLayout(h);
  v->addWidget(okCancel(this));
  refresh();
  resize(560, 320);
}

void TargetsDialog::refresh() {
  list_->clear();
  for (const auto& t : targets_) {
    QString text = QString("%1  —  %2  —  %3")
                       .arg(QString::fromStdString(t.name), QString::fromLatin1(ta::formatName(t.format)),
                            QString::fromStdString(ta::fs::toUtf8(t.folder)));
    if (!t.enabled) text += tr("  (off)");
    list_->addItem(text);
  }
}

// ---- OptionsDialog --------------------------------------------------------

OptionsDialog::OptionsDialog(const AppOptions& o, const QString& dataDir, QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("Options"));
  sim_ = check(tr("Show simulated sources"), o.simulatedSources);
  requireId_ = check(tr("Require a patient ID before exporting"), o.requirePatientId);
  disarmFull_ = check(tr("Disarm when every slot is filled"), o.disarmWhenFull);
  retention_ = spin(0, 3650, o.retentionDays, tr("Never"));
  retention_->setSuffix(tr(" days"));
  retention_->setToolTip(tr("Images and patient details of exported sessions are deleted from this PC after this "
                            "many days. Sessions not yet exported are never deleted."));
  auto* v = new QVBoxLayout(this);
  v->addWidget(sim_);
  v->addWidget(requireId_);
  v->addWidget(disarmFull_);
  auto* retentionRow = new QHBoxLayout;
  retentionRow->addWidget(new QLabel(tr("Delete exported sessions after")));
  retentionRow->addWidget(retention_);
  retentionRow->addStretch();
  v->addLayout(retentionRow);
  sourceBits_ = combo({tr("32-bit sources (most dental sensors)"), tr("64-bit sources"), tr("Both")}, {32, 64, 0},
                      o.sourceBits);
  auto* bitsRow = new QHBoxLayout;
  bitsRow->addWidget(new QLabel(tr("TWAIN sources")));
  bitsRow->addWidget(sourceBits_, 1);
  v->addLayout(bitsRow);
  dsmPath_ = new QLineEdit(o.dsmPath);
  dsmPath_->setPlaceholderText(tr("System TWAINDSM.dll (must match the source bitness)"));
  auto* dsmBrowse = new QPushButton(tr("Browse…"));
  connect(dsmBrowse, &QPushButton::clicked, this, [this] {
    const QString f = QFileDialog::getOpenFileName(this, tr("TWAIN Data Source Manager"), dsmPath_->text(),
                                                   tr("TWAINDSM.dll (TWAINDSM.dll);;DLL files (*.dll)"));
    if (!f.isEmpty()) dsmPath_->setText(QDir::toNativeSeparators(f));
  });
  auto* dsmRow = new QHBoxLayout;
  dsmRow->addWidget(new QLabel(tr("TWAIN DSM")));
  dsmRow->addWidget(dsmPath_, 1);
  dsmRow->addWidget(dsmBrowse);
  v->addLayout(dsmRow);
  dsmLog_ = check(tr("Ask the TWAIN DSM to write a diagnostic log to the data folder"), o.dsmLog);
  v->addWidget(dsmLog_);

  // Patient handoff: how the practice software passes the patient when it starts us.
  QStringList presetNames;
  QVariantList presetIds;
  for (const auto& t : ta::handoffPresets()) {
    presetNames << QString::fromStdString(t.name);
    presetIds << QString::fromStdString(t.id);
  }
  presetNames << tr("Custom");
  presetIds << QString("custom");
  handoffPreset_ = combo(presetNames, presetIds, o.handoffPreset);
  handoffPattern_ = new QLineEdit;
  handoffPattern_->setToolTip(tr("Flags and placeholders, e.g. /PATID {id} /NAME {given} /SURNAME {family}\n"
                                 "Placeholders: {id} {family} {given} {birth} {sex} {accession} {skip} {ssn}\n"
                                 "{ssn} is accepted and always discarded."));
  handoffDate_ = combo({"YYYYMMDD", "YYYY-MM-DD", "MM/DD/YYYY", "DD.MM.YYYY", "DD/MM/YYYY"},
                       {"YYYYMMDD", "YYYY-MM-DD", "MM/DD/YYYY", "DD.MM.YYYY", "DD/MM/YYYY"}, o.handoffDateFormat);
  // The custom text survives switching to a preset and back.
  auto custom = std::make_shared<QString>(o.handoffPattern);
  auto showPattern = [this, custom] {
    const QString id = handoffPreset_->currentData().toString();
    const bool isCustom = id == "custom";
    handoffPattern_->setReadOnly(!isCustom);
    if (isCustom) {
      handoffPattern_->setText(*custom);
      return;
    }
    for (const auto& t : ta::handoffPresets())
      if (QString::fromStdString(t.id) == id) handoffPattern_->setText(QString::fromStdString(t.pattern));
  };
  connect(handoffPattern_, &QLineEdit::textEdited, this, [custom](const QString& t) { *custom = t; });
  connect(handoffPreset_, &QComboBox::currentIndexChanged, this, showPattern);
  showPattern();
  auto* hf = new QFormLayout;
  hf->addRow(tr("Format"), handoffPreset_);
  hf->addRow(tr("Arguments"), handoffPattern_);
  hf->addRow(tr("Birth date"), handoffDate_);
  auto* hg = new QGroupBox(tr("Patient from the practice software"));
  hg->setLayout(hf);
  v->addWidget(hg);

  // Handoff file: a key=value / INI file named by {file} in the arguments, or
  // a fixed file the practice software rewrites for the current patient.
  auto key = [](const QString& value) {
    auto* e = new QLineEdit(value);
    e->setPlaceholderText(tr("Key or Section/Key"));
    return e;
  };
  fileId_ = key(o.fileId);
  fileFamily_ = key(o.fileFamily);
  fileGiven_ = key(o.fileGiven);
  fileName_ = key(o.fileName);
  fileName_->setToolTip(tr("Whole name in one value: \"Family, Given\" or \"Family^Given\"; "
                           "otherwise the last word is the family name."));
  fileBirth_ = key(o.fileBirth);
  fileSex_ = key(o.fileSex);
  fileAccession_ = key(o.fileAccession);
  fileDate_ = combo({"YYYYMMDD", "YYYY-MM-DD", "MM/DD/YYYY", "DD.MM.YYYY", "DD/MM/YYYY"},
                    {"YYYYMMDD", "YYYY-MM-DD", "MM/DD/YYYY", "DD.MM.YYYY", "DD/MM/YYYY"}, o.fileDateFormat);
  watchFile_ = new QLineEdit(o.watchFile);
  watchFile_->setPlaceholderText(tr("Optional: a file to watch for the current patient"));
  auto* watchBrowse = new QPushButton(tr("Browse…"));
  connect(watchBrowse, &QPushButton::clicked, this, [this] {
    const QString f = QFileDialog::getOpenFileName(this, tr("Handoff file"), watchFile_->text());
    if (!f.isEmpty()) watchFile_->setText(QDir::toNativeSeparators(f));
  });
  auto* watchRow = new QHBoxLayout;
  watchRow->addWidget(watchFile_, 1);
  watchRow->addWidget(watchBrowse);
  deleteFile_ = check(tr("Delete the file after reading it"), o.deleteFileAfterRead);
  auto* ff = new QFormLayout;
  ff->addRow(tr("Patient ID"), fileId_);
  ff->addRow(tr("Family name"), fileFamily_);
  ff->addRow(tr("Given name"), fileGiven_);
  ff->addRow(tr("Whole name"), fileName_);
  ff->addRow(tr("Birth date"), fileBirth_);
  ff->addRow(tr("Birth date format"), fileDate_);
  ff->addRow(tr("Sex"), fileSex_);
  ff->addRow(tr("Accession"), fileAccession_);
  ff->addRow(tr("Watch file"), watchRow);
  ff->addRow(deleteFile_);
  auto* fg = new QGroupBox(tr("Handoff file"));
  fg->setLayout(ff);
  v->addWidget(fg);
  auto* where = new QLabel(tr("Data folder: %1").arg(dataDir));
  where->setTextInteractionFlags(Qt::TextSelectableByMouse);
  where->setWordWrap(true);
  v->addWidget(where);
  v->addWidget(okCancel(this));
}

AppOptions OptionsDialog::options() const {
  AppOptions o;
  o.simulatedSources = sim_->isChecked();
  o.requirePatientId = requireId_->isChecked();
  o.disarmWhenFull = disarmFull_->isChecked();
  o.dsmPath = dsmPath_->text().trimmed();
  o.sourceBits = sourceBits_->currentData().toInt();
  o.dsmLog = dsmLog_->isChecked();
  o.handoffPreset = handoffPreset_->currentData().toString();
  o.handoffPattern = o.handoffPreset == "custom" ? handoffPattern_->text().trimmed() : QString();
  o.handoffDateFormat = handoffDate_->currentData().toString();
  o.fileId = fileId_->text().trimmed();
  o.fileFamily = fileFamily_->text().trimmed();
  o.fileGiven = fileGiven_->text().trimmed();
  o.fileName = fileName_->text().trimmed();
  o.fileBirth = fileBirth_->text().trimmed();
  o.fileSex = fileSex_->text().trimmed();
  o.fileAccession = fileAccession_->text().trimmed();
  o.fileDateFormat = fileDate_->currentData().toString();
  o.watchFile = watchFile_->text().trimmed();
  o.deleteFileAfterRead = deleteFile_->isChecked();
  o.retentionDays = retention_->value();
  return o;
}

// ---- MountDialog ----------------------------------------------------------

MountDialog::MountDialog(const ta::Layout& layout, bool customised, QWidget* parent)
    : QDialog(parent), base_(layout) {
  setWindowTitle(tr("Mount settings: %1").arg(QString::fromStdString(layout.name)));
  table_ = new QTableWidget(int(layout.slotList.size()), 3);
  table_->setHorizontalHeaderLabels({tr("Slot"), tr("Rotation"), tr("Mirror")});
  table_->verticalHeader()->hide();
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->setSelectionMode(QAbstractItemView::NoSelection);
  for (int i = 0; i < int(layout.slotList.size()); ++i) {
    const auto& slot = layout.slotList[size_t(i)];
    auto* label = new QTableWidgetItem(QString::fromStdString(slot.label));
    label->setFlags(Qt::ItemIsEnabled);
    table_->setItem(i, 0, label);
    table_->setCellWidget(i, 1, combo({tr("None"), tr("90° clockwise"), tr("180°"), tr("90° anticlockwise")},
                                      {0, 1, 2, 3}, slot.orientation.quarterTurns));
    auto* mirror = new QTableWidgetItem;
    mirror->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    mirror->setCheckState(slot.orientation.flipH ? Qt::Checked : Qt::Unchecked);
    table_->setItem(i, 2, mirror);
  }
  table_->resizeColumnsToContents();
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);

  mirror_ = check(tr("Mirror the mount (show the right side of the mouth on the right)"), layout.mirrored);
  auto* note = new QLabel(tr("Rotation and mirroring apply to images taken into a slot from now on, on top of the "
                             "source's default. Images already taken keep theirs; turn them with the Image menu."));
  note->setWordWrap(true);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  auto* restore = buttons->addButton(tr("Restore defaults"), QDialogButtonBox::ResetRole);
  restore->setEnabled(customised);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(restore, &QPushButton::clicked, this, [this] {
    restore_ = true;
    accept();
  });

  auto* v = new QVBoxLayout(this);
  v->addWidget(table_, 1);
  v->addWidget(mirror_);
  v->addWidget(note);
  v->addWidget(buttons);
  resize(520, 460);
}

ta::Layout MountDialog::layout() const {
  ta::Layout l = base_;
  l.mirrored = mirror_->isChecked();
  for (int i = 0; i < int(l.slotList.size()); ++i) {
    auto* turns = qobject_cast<QComboBox*>(table_->cellWidget(i, 1));
    l.slotList[size_t(i)].orientation.quarterTurns = turns ? turns->currentData().toInt() : 0;
    l.slotList[size_t(i)].orientation.flipH = table_->item(i, 2)->checkState() == Qt::Checked;
  }
  return l;
}
