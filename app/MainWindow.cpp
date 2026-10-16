#include "MainWindow.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <algorithm>
#include <system_error>

#include "ImageView.hpp"
#include "MountView.hpp"
#include "Stager.hpp"

namespace {

constexpr int kListTimeoutMs = 20000;
constexpr int kShutdownMs = 5000;

QString q(const std::string& s) { return QString::fromStdString(s); }

}  // namespace

MainWindow::MainWindow(const LaunchOptions& launch, QWidget* parent) : QMainWindow(parent), launch_(launch) {
  dataDir_ = launch.dataDir;
  QDir().mkpath(dataDir_);
  const ta::fs::Path root = ta::fs::fromUtf8(dataDir_.toStdString());
  profilesDir_ = root / "profiles";
  targetsDir_ = root / "destinations";
  layoutsDir_ = root / "layouts";
  std::error_code ec;
  for (const auto& d : {profilesDir_, targetsDir_, layoutsDir_}) std::filesystem::create_directories(d, ec);

  loadConfig();
  buildUi();

  stager_ = new Stager(dataDir_);
  stager_->moveToThread(&stagerThread_);
  connect(&stagerThread_, &QThread::finished, stager_, &QObject::deleteLater);
  connect(stager_, &Stager::sessionChanged, this, &MainWindow::onSessionChanged);
  connect(stager_, &Stager::sessionClosed, this, &MainWindow::onSessionClosed);
  connect(stager_, &Stager::thumbnailReady, mount_, &MountView::setThumbnail);
  connect(stager_, &Stager::imageStaged, this, &MainWindow::onImageStaged);
  connect(stager_, &Stager::imageLoaded, this, [this](uint seq, FramePtr frame) {
    if (int(seq) != viewedSeq_) return;  // the operator has moved on to another image
    const ta::StagedImage* img = session_.image(seq);
    view_->setImage(frame, img ? img->orientation : ta::Orientation{});
  });
  connect(stager_, &Stager::problem, this, [this](const QString& text) {
    log("error", text);
    QMessageBox::warning(this, tr("Staging"), text);
  });
  stagerThread_.setObjectName("stager");
  stagerThread_.start(QThread::LowPriority);

  queue_ = std::make_unique<ta::ExportQueue>(root, [this](const std::string& id, ta::ExportTarget& out) {
    return lookupTarget(id, out);
  });
  queue_->onChanged = [this] { QMetaObject::invokeMethod(this, [this] { refreshExportStatus(); }, Qt::QueuedConnection); };
  queue_->start();
  connect(&exportTimer_, &QTimer::timeout, this, &MainWindow::refreshExportStatus);
  exportTimer_.start(2000);
  refreshExportStatus();

  armTimer_.setSingleShot(true);
  connect(&armTimer_, &QTimer::timeout, this, [this] {
    disarm(tr("No exposure arrived within %1 s; the sensor was disarmed.").arg(profile_.armTimeoutMs / 1000));
  });

  if (!launch.patient.empty()) setPatientFields(launch.patient);
  QTimer::singleShot(0, this, [this] {
    offerRecovery();
    scanSources();
  });
}

MainWindow::~MainWindow() {
  if (queue_) queue_->stop();
  stagerThread_.quit();
  stagerThread_.wait();
}

// ---- configuration ---------------------------------------------------------

void MainWindow::loadConfig() {
  QSettings s;
  options_.simulatedSources = s.value("simulatedSources", false).toBool() || launch_.simulate;
  options_.requirePatientId = s.value("requirePatientId", true).toBool();
  options_.disarmWhenFull = s.value("disarmWhenFull", true).toBool();
  options_.dsmPath = s.value("dsmPath").toString();
  options_.sourceBits = s.value("sourceBits", 32).toInt();
  options_.dsmLog = s.value("dsmLog", false).toBool();

  std::vector<std::string> problems;
  layouts_ = ta::loadLayouts(layoutsDir_, &problems);
  for (const auto& p : problems) log("warn", tr("Layout ignored: %1").arg(q(p)));

  targets_.clear();
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(targetsDir_, ec)) {
    if (e.path().extension() != ".target") continue;
    ta::Kv kv;
    if (ta::Kv::load(e.path(), kv)) targets_.push_back(ta::ExportTarget::fromKv(kv));
  }
}

bool MainWindow::lookupTarget(const std::string& id, ta::ExportTarget& out) const {
  // Called from the export thread: read the file, not the in-memory list.
  ta::Kv kv;
  if (!ta::Kv::load(targetsDir_ / ta::fs::fromUtf8(id + ".target"), kv)) return false;
  out = ta::ExportTarget::fromKv(kv);
  return true;
}

const ta::Layout& MainWindow::layout() const {
  const int i = layoutBox_ ? layoutBox_->currentIndex() : 0;
  return layouts_[size_t(std::clamp(i, 0, int(layouts_.size()) - 1))];
}

// ---- interface -------------------------------------------------------------

void MainWindow::buildUi() {
  setWindowTitle(tr("twain-acquire"));

  // Patient
  patientId_ = new QLineEdit;
  family_ = new QLineEdit;
  given_ = new QLineEdit;
  birth_ = new QLineEdit;
  birth_->setPlaceholderText(tr("YYYY-MM-DD"));
  birth_->setMaximumWidth(110);
  sexBox_ = new QComboBox;
  sexBox_->addItem("", "");
  sexBox_->addItem(tr("Female"), "F");
  sexBox_->addItem(tr("Male"), "M");
  sexBox_->addItem(tr("Other"), "O");
  for (QLineEdit* e : {patientId_, family_, given_, birth_})
    connect(e, &QLineEdit::editingFinished, this, &MainWindow::pushPatient);
  connect(sexBox_, &QComboBox::currentIndexChanged, this, &MainWindow::pushPatient);

  auto* patientRow = new QHBoxLayout;
  patientRow->addWidget(new QLabel(tr("Patient ID")));
  patientRow->addWidget(patientId_);
  patientRow->addWidget(new QLabel(tr("Family name")));
  patientRow->addWidget(family_, 1);
  patientRow->addWidget(new QLabel(tr("Given name")));
  patientRow->addWidget(given_, 1);
  patientRow->addWidget(new QLabel(tr("Born")));
  patientRow->addWidget(birth_);
  patientRow->addWidget(new QLabel(tr("Sex")));
  patientRow->addWidget(sexBox_);

  // Source and layout
  sourceBox_ = new QComboBox;
  sourceBox_->setMinimumWidth(260);
  connect(sourceBox_, &QComboBox::currentIndexChanged, this, &MainWindow::onSourceChanged);
  auto* refresh = new QPushButton(tr("Find sources"));
  connect(refresh, &QPushButton::clicked, this, &MainWindow::scanSources);
  auto* settings = new QPushButton(tr("Source settings…"));
  connect(settings, &QPushButton::clicked, this, &MainWindow::editProfile);
  layoutBox_ = new QComboBox;
  for (const auto& l : layouts_) layoutBox_->addItem(q(l.name), q(l.id));
  if (!launch_.layout.isEmpty()) {
    const int at = layoutBox_->findData(launch_.layout);
    if (at >= 0) layoutBox_->setCurrentIndex(at);
  } else {
    const int at = layoutBox_->findData(QSettings().value("layout").toString());
    if (at >= 0) layoutBox_->setCurrentIndex(at);
  }
  connect(layoutBox_, &QComboBox::currentIndexChanged, this, [this] {
    if (haveSession_ && session_.layout != layout().id) {
      // The layout belongs to the session; changing it starts a new one.
      if (!settleCurrentSession(tr("Changing the layout starts a new session."))) {
        const QSignalBlocker block(layoutBox_);
        layoutBox_->setCurrentIndex(layoutBox_->findData(q(session_.layout)));
        return;
      }
    }
    mount_->setLayout(layout());
    QSettings().setValue("layout", q(layout().id));
  });

  armButton_ = new QPushButton(tr("Arm sensor"));
  armButton_->setMinimumHeight(36);
  armButton_->setMinimumWidth(140);
  QFont f = armButton_->font();
  f.setBold(true);
  armButton_->setFont(f);
  connect(armButton_, &QPushButton::clicked, this, &MainWindow::toggleArm);

  auto* exportButton = new QPushButton(tr("Export…"));
  exportButton->setMinimumHeight(36);
  connect(exportButton, &QPushButton::clicked, this, &MainWindow::exportSession);

  auto* controlRow = new QHBoxLayout;
  controlRow->addWidget(new QLabel(tr("Source")));
  controlRow->addWidget(sourceBox_, 1);
  controlRow->addWidget(refresh);
  controlRow->addWidget(settings);
  controlRow->addSpacing(16);
  controlRow->addWidget(new QLabel(tr("Layout")));
  controlRow->addWidget(layoutBox_);
  controlRow->addSpacing(16);
  controlRow->addWidget(armButton_);
  controlRow->addWidget(exportButton);

  banner_ = new QLabel;
  banner_->setAlignment(Qt::AlignCenter);
  banner_->setMinimumHeight(30);
  QFont bf = banner_->font();
  bf.setBold(true);
  bf.setPointSizeF(bf.pointSizeF() * 1.2);
  banner_->setFont(bf);

  mount_ = new MountView;
  mount_->setLayout(layout());
  view_ = new ImageView;
  connect(mount_, &MountView::imageChosen, this, &MainWindow::selectImage);
  connect(mount_, &MountView::slotChosen, this, [this](int, int seq) {
    if (seq < 0) {
      viewedSeq_ = -1;
      view_->clear();
    }
  });
  connect(mount_, &MountView::moveRequested, this, [this](uint seq, int slot) {
    QMetaObject::invokeMethod(stager_, "place", Qt::QueuedConnection, Q_ARG(uint, seq), Q_ARG(int, slot));
  });
  connect(mount_, &MountView::contextRequested, this, &MainWindow::showImageMenu);
  auto* split = new QSplitter;
  split->addWidget(mount_);
  split->addWidget(view_);
  split->setStretchFactor(0, 2);
  split->setStretchFactor(1, 3);

  auto* central = new QWidget;
  auto* v = new QVBoxLayout(central);
  v->addLayout(patientRow);
  v->addLayout(controlRow);
  v->addWidget(banner_);
  v->addWidget(split, 1);
  setCentralWidget(central);

  // Menus
  auto* sessionMenu = menuBar()->addMenu(tr("&Session"));
  sessionMenu->addAction(tr("&New session"), this, &MainWindow::newSession);
  sessionMenu->addAction(tr("&Export…"), QKeySequence(Qt::CTRL | Qt::Key_E), this, &MainWindow::exportSession);
  sessionMenu->addSeparator();
  sessionMenu->addAction(tr("E&xit"), this, &QWidget::close);
  auto* imageMenu = menuBar()->addMenu(tr("&Image"));
  imageMenu->addAction(tr("Rotate &left"), QKeySequence(Qt::CTRL | Qt::Key_L), this, [this] { rotateSelected(-1); });
  imageMenu->addAction(tr("Rotate &right"), QKeySequence(Qt::CTRL | Qt::Key_R), this, [this] { rotateSelected(1); });
  imageMenu->addAction(tr("&Mirror"), QKeySequence(Qt::CTRL | Qt::Key_M), this, &MainWindow::mirrorSelected);
  imageMenu->addAction(tr("&Discard…"), QKeySequence::Delete, this, &MainWindow::discardSelected);
  imageMenu->addSeparator();
  auto* invert = imageMenu->addAction(tr("&Invert display"));
  invert->setCheckable(true);
  connect(invert, &QAction::toggled, view_, &ImageView::setInverted);
  imageMenu->addAction(tr("Reset &window"), view_, &ImageView::resetWindow);
  imageMenu->addAction(tr("&Fit"), view_, &ImageView::fit);
  auto* setup = menuBar()->addMenu(tr("Se&tup"));
  setup->addAction(tr("Export &destinations…"), this, &MainWindow::editTargets);
  setup->addAction(tr("&Source settings…"), this, &MainWindow::editProfile);
  setup->addAction(tr("&Options…"), this, &MainWindow::editOptions);

  logView_ = new QPlainTextEdit;
  logView_->setReadOnly(true);
  logView_->setMaximumBlockCount(2000);
  auto* dock = new QDockWidget(tr("Log"), this);
  dock->setWidget(logView_);
  dock->setObjectName("log");
  addDockWidget(Qt::BottomDockWidgetArea, dock);
  dock->hide();
  setup->addAction(dock->toggleViewAction());

  helperLabel_ = new QLabel;
  exportLabel_ = new QLabel;
  retryButton_ = new QPushButton(tr("Retry failed exports"));
  retryButton_->hide();
  connect(retryButton_, &QPushButton::clicked, this, [this] {
    queue_->retryFailed();
    refreshExportStatus();
  });
  statusBar()->addWidget(helperLabel_);
  statusBar()->addPermanentWidget(exportLabel_);
  statusBar()->addPermanentWidget(retryButton_);

  updateBanner();
  resize(1280, 800);
}

void MainWindow::log(const QString& level, const QString& text) {
  const QString line = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") + "  " + level + "  " + text;
  if (logView_) logView_->appendPlainText(line);
  QFile f(dataDir_ + "/twain-acquire.log");
  if (f.open(QIODevice::Append | QIODevice::Text)) f.write((line + "\n").toUtf8());
}

void MainWindow::status(const QString& text) {
  statusBar()->showMessage(text, 8000);
  log("info", text);
}

// ---- recovery --------------------------------------------------------------

void MainWindow::offerRecovery() {
  const auto open = ta::StagingSession::listOpen(ta::fs::fromUtf8(dataDir_.toStdString()));
  if (open.empty()) return;
  ta::SessionState s;
  if (!ta::StagingSession::readState(open.front(), s)) return;
  int kept = 0;
  for (const auto& img : s.images) kept += !img.discarded;
  if (kept == 0) {
    QMetaObject::invokeMethod(stager_, "resumeSession", Qt::BlockingQueuedConnection,
                              Q_ARG(QString, q(ta::fs::toUtf8(open.front()))));
    QMetaObject::invokeMethod(stager_, "closeSession", Qt::BlockingQueuedConnection, Q_ARG(QString, "empty"));
    return;
  }
  const QString when = QDateTime::fromMSecsSinceEpoch(s.startedUnixMs).toString("yyyy-MM-dd HH:mm");
  const QString who = s.patient.empty() ? tr("no patient entered") : q(s.patient.displayName());
  QMessageBox box(QMessageBox::Question, tr("Unfinished session"),
                  tr("A session from %1 (%2) has %3 image(s) that were not exported.\n\nOpen it now?")
                      .arg(when, who)
                      .arg(kept),
                  QMessageBox::NoButton, this);
  auto* resume = box.addButton(tr("Open it"), QMessageBox::AcceptRole);
  box.addButton(tr("Later"), QMessageBox::RejectRole);
  if (open.size() > 1) box.setInformativeText(tr("%1 more unfinished session(s) are kept.").arg(open.size() - 1));
  box.exec();
  if (box.clickedButton() != resume) return;
  const int at = layoutBox_->findData(q(s.layout));
  if (at >= 0) {
    const QSignalBlocker block(layoutBox_);
    layoutBox_->setCurrentIndex(at);
    mount_->setLayout(layout());
  }
  setPatientFields(s.patient);
  QMetaObject::invokeMethod(stager_, "resumeSession", Qt::QueuedConnection, Q_ARG(QString, q(ta::fs::toUtf8(open.front()))));
}

// ---- sources ---------------------------------------------------------------

QStringList MainWindow::helperPrograms(QStringList* backends) const {
  QStringList programs;
  const QString dir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
  // One helper per bitness searched: each loads the DSM of its own bitness,
  // which lists the sources under Windows\twain_32 or Windows\twain_64.
  for (const char* bits : {"32", "64"}) {
    if (options_.sourceBits != 0 && QString::number(options_.sourceBits) != bits) continue;
    const QString p = dir + "/ta-helper" + bits + ".exe";
    if (QFileInfo::exists(p)) {
      programs << p;
      *backends << "twain";
    }
  }
  if (options_.simulatedSources) {
    const QString p = QFileInfo::exists(dir + "/ta-helper64.exe") ? dir + "/ta-helper64.exe" : dir + "/ta-helper32.exe";
    programs << p;
    *backends << "sim";
  }
#else
  if (options_.simulatedSources) {
    programs << dir + "/ta-helper";
    *backends << "sim";
  }
#endif
  return programs;
}

void MainWindow::scanSources() {
  if (helperState_ == "armed" || helperState_ == "transferring") {
    status(tr("Disarm before searching for sources."));
    return;
  }
  for (HelperClient* s : scanners_) s->deleteLater();
  scanners_.clear();
  sources_.clear();
  sourceBox_->clear();
  QStringList backends;
  const QStringList programs = helperPrograms(&backends);
  if (programs.isEmpty()) {
    sourceBox_->addItem(tr("No acquisition helper found"));
    log("warn", tr("No helper programs next to %1").arg(QCoreApplication::applicationDirPath()));
    return;
  }
  helperLabel_->setText(tr("Searching for sources…"));
  auto remaining = std::make_shared<int>(int(programs.size()));
  auto problems = std::make_shared<QStringList>();
  auto finishOne = [this, remaining, problems] {
    if (--*remaining > 0) return;
    const QSignalBlocker block(sourceBox_);
    sourceBox_->clear();
    for (const auto& s : sources_) {
      QString label = s.name;
      if (s.backend == "twain") label += QString(" (%1-bit)").arg(s.bits);
      sourceBox_->addItem(label);
    }
    if (sources_.isEmpty()) sourceBox_->addItem(tr("No TWAIN sources found"));
    sourceBox_->setToolTip(problems->join("\n"));
    if (sources_.isEmpty() && !problems->isEmpty()) {
      QMessageBox box(QMessageBox::Warning, tr("TWAIN sources"),
                      tr("No TWAIN sources were found.") + "\n\n" + problems->join("\n\n"), QMessageBox::Ok, this);
      box.setInformativeText(tr("Setup > Log lists the data source files that were found on disk."));
      box.exec();
    }
    int pick = 0;
    const QString want = launch_.source.isEmpty() ? QSettings().value("source").toString() : launch_.source;
    for (int i = 0; i < sources_.size(); ++i)
      if (sources_[i].name == want) pick = i;
    sourceBox_->setCurrentIndex(pick);
    helperLabel_->setText(tr("%1 source(s) found").arg(sources_.size()));
    onSourceChanged();
  };
  for (int i = 0; i < programs.size(); ++i) {
    auto* h = new HelperClient(this);
    scanners_ << h;
    connect(h, &HelperClient::sourcesListed, this, [this, h, finishOne](QList<HelperClient::Source> list) {
      sources_ += list;
      h->shutdown(kShutdownMs);
      finishOne();
    });
    const QString who = backends[i] == "sim" ? tr("Simulated") : (programs[i].contains("64") ? tr("64-bit") : tr("32-bit"));
    connect(h, &HelperClient::died, this, [this, finishOne, problems, who](const QString& why) {
      log("warn", tr("Source search (%1): %2").arg(who, why));
      *problems << who + ": " + why;
      finishOne();
    });
    connect(h, &HelperClient::failed, this, [this, problems, who](const QString&, const QString& text, bool) {
      log("warn", tr("Source search (%1): %2").arg(who, text));
      *problems << who + ": " + text;
    });
    connect(h, &HelperClient::logLine, this, [this, who](const QString& level, const QString& text) {
      log(level, who + ": " + text);
    });
    connect(h, &HelperClient::hello, this, [h] { h->list(kListTimeoutMs); });
    h->start(programs[i], backends[i], helperArgs(backends[i]), helperEnv(programs[i], backends[i]));
  }
}

QStringList MainWindow::helperArgs(const QString& backend) const {
  if (backend == "twain" && !options_.dsmPath.isEmpty()) return {"--dsm", options_.dsmPath};
  return {};
}

QStringList MainWindow::helperEnv(const QString& program, const QString& backend) const {
  if (backend != "twain" || !options_.dsmLog) return {};
  const QString bits = program.contains("64") ? "64" : "32";
  return {"TWAINDSM_LOG=" + QDir::toNativeSeparators(dataDir_ + "/twaindsm-" + bits + ".log")};
}

const HelperClient::Source* MainWindow::currentSource() const {
  const int i = sourceBox_->currentIndex();
  return (i >= 0 && i < sources_.size()) ? &sources_[i] : nullptr;
}

void MainWindow::onSourceChanged() {
  const HelperClient::Source* s = currentSource();
  if (!s) return;
  const bool known = std::filesystem::exists(ta::SourceProfile::pathFor(profilesDir_, s->name.toStdString()));
  profile_ = ta::SourceProfile::load(profilesDir_, s->name.toStdString());
  if (!known && s->bits) profile_.helperBits = s->bits;
  QSettings().setValue("source", s->name);
  // A different source needs a fresh open.
  if (helper_ && helper_->isRunning() && helperState_ != "armed" && helperState_ != "transferring") {
    helper_->shutdown(kShutdownMs);
    helperState_ = "closed";
  }
}

void MainWindow::editProfile() {
  if (!currentSource()) return;
  if (helperState_ == "armed" || helperState_ == "transferring") {
    status(tr("Disarm before changing source settings."));
    return;
  }
  ProfileDialog d(profile_, this);
  if (d.exec() != QDialog::Accepted) return;
  profile_ = d.profile();
  std::string err;
  if (!profile_.save(profilesDir_, &err)) QMessageBox::warning(this, tr("Source settings"), q(err));
  // Settings are negotiated at open; reopen next time.
  if (helper_ && helper_->isRunning()) {
    helper_->shutdown(kShutdownMs);
    helperState_ = "closed";
  }
}

bool MainWindow::ensureHelper() {
  const HelperClient::Source* s = currentSource();
  if (!s) {
    QMessageBox::information(this, tr("Arm"), tr("No source is selected."));
    return false;
  }
  QString program = s->program;
#ifdef Q_OS_WIN
  if (s->backend == "twain") {
    const QString wanted = QCoreApplication::applicationDirPath() + QString("/ta-helper%1.exe").arg(profile_.helperBits);
    if (QFileInfo::exists(wanted)) program = wanted;
  }
#endif
  if (helper_ && helper_->isRunning() && helper_->program() == program && helper_->backend() == s->backend) return true;
  if (helper_) {
    helper_->shutdown(kShutdownMs);
    helper_->deleteLater();
  }
  helper_ = new HelperClient(this);
  helperReady_ = false;
  helper_->setTransferTimeout(profile_.callTimeoutMs > 0 ? std::max(profile_.callTimeoutMs, 30000) : 0);
  connect(helper_, &HelperClient::hello, this, [this](const QString& backend, int bits) {
    log("info", tr("Helper started (%1, %2-bit)").arg(backend).arg(bits));
    helperReady_ = true;
    if (armAfterOpen_) openSource();
  });
  connect(helper_, &HelperClient::opened, this, [this](const ta::Kv& kv) {
    QStringList notes;
    for (const auto& [k, v] : kv.items()) {
      log("info", tr("Negotiated %1 = %2").arg(q(k), q(v)));
      if (k.rfind("note", 0) == 0) notes << q(v);
    }
    if (!notes.isEmpty()) status(notes.join("; "));
    if (armAfterOpen_) {
      armAfterOpen_ = false;
      helper_->enable(profile_.callTimeoutMs);
    }
  });
  connect(helper_, &HelperClient::stateChanged, this, &MainWindow::onHelperState);
  connect(helper_, &HelperClient::frameReceived, this, &MainWindow::onFrame);
  connect(helper_, &HelperClient::failed, this, [this](const QString& code, const QString& text, bool fatal) {
    armAfterOpen_ = false;
    log("error", code + ": " + text);
    QMessageBox::warning(this, tr("Source"), text);
    if (fatal && helper_) helper_->shutdown(kShutdownMs);
    updateBanner();
  });
  connect(helper_, &HelperClient::logLine, this, [this](const QString& level, const QString& text) { log(level, text); });
  connect(helper_, &HelperClient::died, this, &MainWindow::onHelperDied);
  helperState_ = "closed";
  helper_->start(program, s->backend, helperArgs(s->backend), helperEnv(program, s->backend));
  return true;
}

void MainWindow::openSource() {
  const HelperClient::Source* s = currentSource();
  if (!s || !helper_) return;
  ta::Kv kv = profile_.toKv();
  kv.set("source", s->name.toStdString());
  helper_->open(kv, profile_.callTimeoutMs);
}

// ---- arming ----------------------------------------------------------------

void MainWindow::toggleArm() {
  if (helperState_ == "armed" || helperState_ == "transferring") disarm(tr("Disarmed."));
  else arm();
}

void MainWindow::arm() {
  if (!currentSource()) {
    QMessageBox::information(this, tr("Arm"), tr("No source is selected."));
    return;
  }
  if (!ensureSession()) return;
  const auto filled = mount_->filledSlots();
  const int target = mount_->targetSlot();
  if (target >= 0 && target < int(filled.size()) && filled[size_t(target)]) {
    const auto answer = QMessageBox::question(
        this, tr("Arm"),
        tr("The selected slot already has an image. A new exposure will replace it there (the earlier image is "
           "kept, unplaced).\n\nArm for a retake?"));
    if (answer != QMessageBox::Yes) return;
  }
  stateUnknown_ = false;
  if (!ensureHelper()) return;
  if (helperState_ == "open") {
    helper_->enable(profile_.callTimeoutMs);
  } else {
    // Enabled once the helper reports the source open.
    armAfterOpen_ = true;
    if (helperReady_) openSource();
  }
  updateBanner();
}

void MainWindow::disarm(const QString& why) {
  armTimer_.stop();
  armAfterOpen_ = false;
  if (helper_ && helper_->isRunning()) helper_->disable(profile_.callTimeoutMs);
  if (!why.isEmpty()) status(why);
}

void MainWindow::onHelperState(const QString& state, const QString& reason) {
  helperState_ = state;
  if (state == "armed") {
    if (profile_.armTimeoutMs > 0) armTimer_.start(profile_.armTimeoutMs);
  } else if (state != "transferring") {
    armTimer_.stop();
  }
  if (!reason.isEmpty() && reason != "disarmed") log("info", tr("Source: %1 (%2)").arg(state, reason));
  updateBanner();
}

void MainWindow::onHelperDied(const QString& why) {
  const bool wasLive = helperState_ != "closed";
  helperState_ = "closed";
  armAfterOpen_ = false;
  armTimer_.stop();
  stateUnknown_ = wasLive;
  log("error", why);
  updateBanner();
  QMessageBox::critical(this, tr("Acquisition stopped"),
                        why + "\n\n" + tr("Images already received are kept.") +
                            (wasLive ? "\n" + tr("Check the sensor before the next exposure; it may still be armed. "
                                                 "Unplugging and reconnecting it resets it.")
                                     : QString()));
}

void MainWindow::onFrame(const QByteArray& payload) {
  if (profile_.armTimeoutMs > 0 && helperState_ == "armed") armTimer_.start(profile_.armTimeoutMs);
  const int slot = mount_->targetSlot();
  ta::Orientation o = profile_.defaultOrientation;
  if (slot >= 0 && slot < int(layout().slotList.size())) {
    const auto& so = layout().slotList[size_t(slot)].orientation;
    o = o.rotated(so.quarterTurns);
    if (so.flipH) o = o.mirrored();
  }
  QMetaObject::invokeMethod(stager_, "addFrame", Qt::QueuedConnection, Q_ARG(QByteArray, payload),
                            Q_ARG(ta::NormalizeOptions, profile_.normalize), Q_ARG(ta::Orientation, o),
                            Q_ARG(int, slot));
}

void MainWindow::onImageStaged(uint seq, int slot) {
  viewedSeq_ = int(seq);
  mount_->setSelectedImage(int(seq));
  const auto filled = mount_->filledSlots();
  int next = slot;
  if (profile_.autoAdvance && slot >= 0) next = layout().nextEmpty(slot, filled);
  if (next >= 0) mount_->setTargetSlot(next);
  const bool armed = helperState_ == "armed" || helperState_ == "transferring";
  if (armed && profile_.disarmAfterEachImage) disarm(tr("Image received; disarmed."));
  else if (armed && next < 0 && options_.disarmWhenFull) disarm(tr("Every slot is filled; the sensor was disarmed."));
}

void MainWindow::updateBanner() {
  const bool armed = helperState_ == "armed" || helperState_ == "transferring";
  if (armed) {
    banner_->setText(helperState_ == "transferring" ? tr("RECEIVING IMAGE…")
                                                    : tr("ARMED — the sensor will capture the next exposure"));
    banner_->setStyleSheet("background:#c62828;color:white;border-radius:4px;");
    armButton_->setText(tr("Disarm"));
  } else if (stateUnknown_) {
    banner_->setText(tr("SENSOR STATE UNKNOWN — the source stopped; check the sensor"));
    banner_->setStyleSheet("background:#f9a825;color:black;border-radius:4px;");
    armButton_->setText(tr("Arm sensor"));
  } else if (armAfterOpen_) {
    banner_->setText(tr("Opening the source…"));
    banner_->setStyleSheet("background:#455a64;color:white;border-radius:4px;");
    armButton_->setText(tr("Arm sensor"));
  } else {
    banner_->setText(tr("Disarmed"));
    banner_->setStyleSheet("background:#2e7d32;color:white;border-radius:4px;");
    armButton_->setText(tr("Arm sensor"));
  }
  helperLabel_->setText(helper_ && helper_->isRunning() ? tr("Source %1").arg(helperState_) : tr("Source not open"));
}

// ---- session ---------------------------------------------------------------

ta::PatientInfo MainWindow::patientFromFields(QString* problem) const {
  ta::PatientInfo p;
  p.id = patientId_->text().trimmed().toStdString();
  p.familyName = family_->text().trimmed().toStdString();
  p.givenName = given_->text().trimmed().toStdString();
  p.sex = sexBox_->currentData().toString().toStdString();
  const QString b = birth_->text().trimmed();
  if (!b.isEmpty()) {
    const QDate d = QDate::fromString(b, "yyyy-MM-dd");
    if (d.isValid()) p.birthDate = d.toString("yyyyMMdd").toStdString();
    else if (problem) *problem = tr("The birth date must be written as YYYY-MM-DD.");
  }
  return p;
}

void MainWindow::setPatientFields(const ta::PatientInfo& p) {
  const QSignalBlocker b1(patientId_), b2(family_), b3(given_), b4(birth_), b5(sexBox_);
  patientId_->setText(q(p.id));
  family_->setText(q(p.familyName));
  given_->setText(q(p.givenName));
  const QDate d = QDate::fromString(q(p.birthDate), "yyyyMMdd");
  birth_->setText(d.isValid() ? d.toString("yyyy-MM-dd") : QString());
  const int at = sexBox_->findData(q(p.sex));
  sexBox_->setCurrentIndex(at < 0 ? 0 : at);
}

void MainWindow::pushPatient() {
  if (!haveSession_) return;
  QMetaObject::invokeMethod(stager_, "setPatient", Qt::QueuedConnection, Q_ARG(ta::PatientInfo, patientFromFields(nullptr)));
}

bool MainWindow::ensureSession() {
  if (haveSession_) return true;
  const QString source = currentSource() ? currentSource()->name : QString();
  QMetaObject::invokeMethod(stager_, "startSession", Qt::BlockingQueuedConnection, Q_ARG(QString, q(layout().id)),
                            Q_ARG(QString, source), Q_ARG(ta::PatientInfo, patientFromFields(nullptr)));
  QCoreApplication::processEvents();  // deliver sessionChanged
  return haveSession_;
}

void MainWindow::onSessionChanged(const ta::SessionState& state, const QString& dir) {
  session_ = state;
  sessionDir_ = dir;
  haveSession_ = true;
  mount_->setSession(state);
  if (viewedSeq_ >= 0) {
    const ta::StagedImage* img = state.image(uint(viewedSeq_));
    if (!img || img->discarded) {
      viewedSeq_ = -1;
      view_->clear();
    } else {
      view_->setOrientation(img->orientation);
    }
  }
}

void MainWindow::onSessionClosed() {
  haveSession_ = false;
  session_ = ta::SessionState{};
  sessionDir_.clear();
  viewedSeq_ = -1;
  mount_->clearSession();
  view_->clear();
}

// Returns true when the current session (if any) has been dealt with.
bool MainWindow::settleCurrentSession(const QString& question) {
  if (!haveSession_) return true;
  if (helperState_ == "armed" || helperState_ == "transferring") {
    QMessageBox::information(this, tr("Session"), tr("Disarm the sensor first."));
    return false;
  }
  int kept = 0;
  for (const auto& img : session_.images) kept += !img.discarded;
  if (kept > 0) {
    QMessageBox box(QMessageBox::Question, tr("Session"),
                    question + "\n\n" + tr("This session has %1 image(s) that were not exported.").arg(kept),
                    QMessageBox::NoButton, this);
    auto* keep = box.addButton(tr("Keep it for later"), QMessageBox::AcceptRole);
    auto* drop = box.addButton(tr("Close it without exporting"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == keep) {
      // Left open on disk; it is offered again at the next start.
      onSessionClosed();
      return true;
    }
    if (box.clickedButton() != drop) return false;
  }
  QMetaObject::invokeMethod(stager_, "closeSession", Qt::BlockingQueuedConnection,
                            Q_ARG(QString, kept ? QString("closed without export") : QString("empty")));
  QCoreApplication::processEvents();
  return true;
}

void MainWindow::newSession() {
  if (!settleCurrentSession(tr("Start a new session?"))) return;
  setPatientFields(ta::PatientInfo{});
  mount_->setLayout(layout());
}

// ---- images ----------------------------------------------------------------

void MainWindow::selectImage(int seq) {
  if (seq < 0) return;
  viewedSeq_ = seq;
  QMetaObject::invokeMethod(stager_, "loadImage", Qt::QueuedConnection, Q_ARG(uint, uint(seq)));
}

void MainWindow::rotateSelected(int turns) {
  const int seq = mount_->selectedImage();
  const ta::StagedImage* img = seq >= 0 ? session_.image(uint(seq)) : nullptr;
  if (!img) return;
  QMetaObject::invokeMethod(stager_, "setOrientation", Qt::QueuedConnection, Q_ARG(uint, uint(seq)),
                            Q_ARG(ta::Orientation, img->orientation.rotated(turns)));
}

void MainWindow::mirrorSelected() {
  const int seq = mount_->selectedImage();
  const ta::StagedImage* img = seq >= 0 ? session_.image(uint(seq)) : nullptr;
  if (!img) return;
  QMetaObject::invokeMethod(stager_, "setOrientation", Qt::QueuedConnection, Q_ARG(uint, uint(seq)),
                            Q_ARG(ta::Orientation, img->orientation.mirrored()));
}

void MainWindow::discardSelected() {
  const int seq = mount_->selectedImage();
  if (seq < 0 || !session_.image(uint(seq))) return;
  if (QMessageBox::question(this, tr("Discard"), tr("Discard the selected image? It will not be exported.")) !=
      QMessageBox::Yes)
    return;
  QMetaObject::invokeMethod(stager_, "discard", Qt::QueuedConnection, Q_ARG(uint, uint(seq)));
}

void MainWindow::showImageMenu(int seq, const QPoint& at) {
  mount_->setSelectedImage(seq);
  selectImage(seq);
  QMenu menu(this);
  menu.addAction(tr("Rotate left"), this, [this] { rotateSelected(-1); });
  menu.addAction(tr("Rotate right"), this, [this] { rotateSelected(1); });
  menu.addAction(tr("Mirror"), this, &MainWindow::mirrorSelected);
  menu.addAction(tr("Remove from slot"), this, [this, seq] {
    QMetaObject::invokeMethod(stager_, "place", Qt::QueuedConnection, Q_ARG(uint, uint(seq)), Q_ARG(int, -1));
  });
  menu.addSeparator();
  menu.addAction(tr("Discard…"), this, &MainWindow::discardSelected);
  menu.exec(at);
}

// ---- export ----------------------------------------------------------------

void MainWindow::editTargets() {
  TargetsDialog d(targets_, this);
  if (d.exec() != QDialog::Accepted) return;
  const auto updated = d.targets();
  std::error_code ec;
  for (const auto& old : targets_) {
    bool still = false;
    for (const auto& t : updated) still |= t.id == old.id;
    if (!still) std::filesystem::remove(targetsDir_ / ta::fs::fromUtf8(old.id + ".target"), ec);
  }
  for (const auto& t : updated) {
    std::string err;
    if (!t.toKv().save(targetsDir_ / ta::fs::fromUtf8(t.id + ".target"), &err))
      QMessageBox::warning(this, tr("Export destinations"), q(err));
  }
  targets_ = updated;
}

void MainWindow::exportSession() {
  if (!haveSession_) {
    QMessageBox::information(this, tr("Export"), tr("There is nothing to export yet."));
    return;
  }
  if (helperState_ == "armed" || helperState_ == "transferring") {
    QMessageBox::information(this, tr("Export"), tr("Disarm the sensor before exporting."));
    return;
  }
  QString problem;
  const ta::PatientInfo p = patientFromFields(&problem);
  if (!problem.isEmpty()) {
    QMessageBox::warning(this, tr("Export"), problem);
    return;
  }
  if (options_.requirePatientId && p.id.empty()) {
    QMessageBox::warning(this, tr("Export"), tr("Enter the patient ID before exporting."));
    patientId_->setFocus();
    return;
  }
  std::vector<ta::ExportTarget> enabled;
  for (const auto& t : targets_)
    if (t.enabled) enabled.push_back(t);
  if (enabled.empty()) {
    QMessageBox::information(this, tr("Export"), tr("Set up an export destination first."));
    editTargets();
    return;
  }

  // Bring the journal up to date with the fields, then export from what is on disk.
  QMetaObject::invokeMethod(stager_, "setPatient", Qt::BlockingQueuedConnection, Q_ARG(ta::PatientInfo, p));
  ta::SessionState disk;
  std::string err;
  if (!ta::StagingSession::readState(ta::fs::fromUtf8(sessionDir_.toStdString()), disk, &err)) {
    QMessageBox::critical(this, tr("Export"), q(err));
    return;
  }
  int kept = 0, unplaced = 0;
  for (const auto& img : disk.images) {
    kept += !img.discarded;
    unplaced += !img.discarded && img.slot < 0;
  }
  if (kept == 0) {
    QMessageBox::information(this, tr("Export"), tr("There are no images to export."));
    return;
  }

  // The operator confirms the patient every time; nothing here can know
  // which chart is open in the receiving software.
  QStringList dests;
  for (const auto& t : enabled) dests << q(t.name);
  const QString name = p.empty() ? tr("(no name)") : q(p.displayName());
  QString text = tr("Export %1 image(s) for:\n\n    %2\n    ID %3%4\n\nTo: %5")
                     .arg(kept)
                     .arg(name, p.id.empty() ? tr("(none)") : q(p.id),
                          p.birthDate.empty() ? QString() : tr(", born %1").arg(birth_->text()), dests.join(", "));
  if (unplaced) text += "\n\n" + tr("%1 image(s) are not placed in a slot and will be exported last.").arg(unplaced);
  QMessageBox box(QMessageBox::Question, tr("Confirm export"), text, QMessageBox::NoButton, this);
  auto* go = box.addButton(tr("Export for %1").arg(name), QMessageBox::AcceptRole);
  box.addButton(QMessageBox::Cancel);
  box.setDefaultButton(QMessageBox::Cancel);
  box.exec();
  if (box.clickedButton() != go) return;

  const int jobs = queue_->enqueueSession(ta::fs::fromUtf8(sessionDir_.toStdString()), disk, layout(), enabled, &err);
  if (jobs == 0) {
    QMessageBox::critical(this, tr("Export"), q(err));
    return;
  }
  log("info", tr("Queued %1 export job(s)").arg(jobs));
  QMetaObject::invokeMethod(stager_, "closeSession", Qt::BlockingQueuedConnection, Q_ARG(QString, "exported"));
  QCoreApplication::processEvents();
  setPatientFields(ta::PatientInfo{});
  refreshExportStatus();
  status(tr("Export queued for %1.").arg(name));
}

void MainWindow::refreshExportStatus() {
  int pending = 0, failed = 0;
  QString lastError;
  for (const auto& j : queue_->snapshot()) {
    if (j.state == ta::ExportJob::State::Pending) ++pending;
    if (j.state == ta::ExportJob::State::Failed) {
      ++failed;
      lastError = q(j.lastError);
    }
  }
  QString text = pending ? tr("Exporting: %1 waiting").arg(pending) : tr("Exports up to date");
  if (failed) text += tr(" — %1 FAILED").arg(failed);
  exportLabel_->setText(text);
  exportLabel_->setToolTip(lastError);
  exportLabel_->setStyleSheet(failed ? "color:#c62828;font-weight:bold;" : "");
  retryButton_->setVisible(failed > 0);
}

void MainWindow::editOptions() {
  OptionsDialog d(options_, dataDir_, this);
  if (d.exec() != QDialog::Accepted) return;
  const bool rescan = d.options().simulatedSources != options_.simulatedSources || d.options().dsmPath != options_.dsmPath ||
                      d.options().sourceBits != options_.sourceBits || d.options().dsmLog != options_.dsmLog;
  options_ = d.options();
  QSettings s;
  s.setValue("simulatedSources", options_.simulatedSources);
  s.setValue("requirePatientId", options_.requirePatientId);
  s.setValue("disarmWhenFull", options_.disarmWhenFull);
  s.setValue("dsmPath", options_.dsmPath);
  s.setValue("sourceBits", options_.sourceBits);
  s.setValue("dsmLog", options_.dsmLog);
  if (rescan) scanSources();
}

void MainWindow::closeEvent(QCloseEvent* e) {
  if (helperState_ == "armed" || helperState_ == "transferring") {
    if (QMessageBox::question(this, tr("Exit"), tr("The sensor is armed. Disarm and exit?")) != QMessageBox::Yes) {
      e->ignore();
      return;
    }
  }
  if (helper_) helper_->shutdown(kShutdownMs);
  // An unexported session stays on disk and is offered at the next start.
  e->accept();
}
