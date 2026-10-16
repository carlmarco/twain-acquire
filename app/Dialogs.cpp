#include "Dialogs.hpp"

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
#include <QUuid>
#include <QVBoxLayout>

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
  disarmEach_ = check(tr("Disarm after each image"), p.disarmAfterEachImage);

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
  neg->addRow(disarmEach_);
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
  p.disarmAfterEachImage = disarmEach_->isChecked();
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
  auto* v = new QVBoxLayout(this);
  v->addWidget(sim_);
  v->addWidget(requireId_);
  v->addWidget(disarmFull_);
  dsmPath_ = new QLineEdit(o.dsmPath);
  dsmPath_->setPlaceholderText(tr("System TWAINDSM.dll"));
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
  return o;
}
