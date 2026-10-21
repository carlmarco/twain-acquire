/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ViewerWindow.hpp"

#include <QApplication>
#include <QKeySequence>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <algorithm>

#include "ImageView.hpp"
#include "MountView.hpp"
#include "Thumbnail.hpp"

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

// More loose files than this are not shown: a mount is not a file browser.
constexpr size_t kMaxLooseFiles = 6 * 32;

}  // namespace

ViewerWindow::ViewerWindow(const ta::fs::Path& folder, const ta::MountManifest& manifest, QWidget* parent)
    : QMainWindow(parent), folder_(folder) {
  for (const auto& img : manifest.images) {
    Item it;
    it.file = folder / ta::fs::fromUtf8(img.file);
    it.seq = img.seq;
    it.slot = img.slot;
    // Pixels already turned at export are shown as they are.
    if (!img.orientationApplied) it.orientation = img.orientation;
    items_.push_back(it);
  }
  // Sequence numbers come from the file; make sure they are unique here.
  uint next = 1;
  for (const auto& it : items_) next = std::max(next, it.seq + 1);
  std::vector<uint> seen;
  for (auto& it : items_) {
    if (std::find(seen.begin(), seen.end(), it.seq) != seen.end()) it.seq = next++;
    seen.push_back(it.seq);
  }

  QString who = q(manifest.patient.displayName());
  if (!manifest.patient.id.empty()) who += (who.isEmpty() ? "" : " ") + tr("(ID %1)").arg(q(manifest.patient.id));
  if (who.isEmpty()) who = tr("No patient");
  QString when = q(manifest.exported);
  when.replace('T', ' ');
  build(manifest.layout, tr("%1 - %2 - exported %3").arg(who, q(manifest.layout.name), when));
}

ViewerWindow::ViewerWindow(const ta::fs::Path& folder, const std::vector<ta::fs::Path>& files, QWidget* parent)
    : QMainWindow(parent), folder_(folder) {
  const size_t n = std::min(files.size(), kMaxLooseFiles);
  ta::Layout layout;
  layout.id = "files";
  layout.name = tr("Images in folder").toStdString();
  layout.columns = int(std::min<size_t>(n, 6));
  layout.rows = int((n + size_t(layout.columns) - 1) / size_t(layout.columns));
  for (size_t i = 0; i < n; ++i) {
    ta::LayoutSlot s;
    s.label = ta::fs::toUtf8(files[i].filename());
    s.key = "image-" + std::to_string(i + 1);
    s.column = int(i % size_t(layout.columns));
    s.row = int(i / size_t(layout.columns));
    layout.slotList.push_back(s);
    items_.push_back(Item{files[i], uint(i + 1), int(i), {}});
  }
  if (files.size() > n) problems_ << tr("Only the first %1 of %2 images are shown.").arg(n).arg(files.size());
  build(layout, tr("%1 - no mount file, shown in name order").arg(q(ta::fs::toUtf8(folder.filename()))));
}

ViewerWindow::~ViewerWindow() {
  *cancel_ = true;
  // The loader stops after the file it is reading; never leave it running
  // into a destroyed window or a closing application.
  if (loader_) loader_->wait(5000);
}

void ViewerWindow::build(const ta::Layout& layout, const QString& title) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Viewing: %1").arg(title));
  resize(1200, 760);

  mount_ = new MountView;
  view_ = new ImageView;
  auto* split = new QSplitter(Qt::Horizontal);
  split->addWidget(mount_);
  split->addWidget(view_);
  split->setStretchFactor(0, 2);
  split->setStretchFactor(1, 3);
  setCentralWidget(split);

  ta::SessionState state;
  state.layout = layout.id;
  for (const auto& it : items_) {
    ta::StagedImage img;
    img.seq = it.seq;
    img.slot = it.slot;
    img.orientation = it.orientation;
    img.file = ta::fs::toUtf8(it.file.filename());
    state.images.push_back(img);
    shown_[it.seq] = it.orientation;
  }
  mount_->setLayout(layout);
  mount_->setSession(state);
  mount_->setTargetSlot(-1);  // nothing is being acquired here
  // Moving images between slots is not offered: the mount shows what was exported.
  connect(mount_, &MountView::imageChosen, this, &ViewerWindow::showImage);
  connect(mount_, &MountView::slotChosen, this, [this](int, int seq) {
    if (seq >= 0) showImage(seq);
  });

  auto* image = menuBar()->addMenu(tr("&Image"));
  image->addAction(tr("Rotate &left"), QKeySequence(Qt::CTRL | Qt::Key_L), this, [this] { turn(-1, false); });
  image->addAction(tr("Rotate &right"), QKeySequence(Qt::CTRL | Qt::Key_R), this, [this] { turn(1, false); });
  image->addAction(tr("&Mirror"), QKeySequence(Qt::CTRL | Qt::Key_M), this, [this] { turn(0, true); });
  image->addSeparator();
  auto* invert = image->addAction(tr("&Invert display"));
  invert->setCheckable(true);
  connect(invert, &QAction::toggled, view_, &ImageView::setInverted);
  image->addAction(tr("Reset &window"), view_, &ImageView::resetWindow);
  image->addAction(tr("&Fit"), view_, &ImageView::fit);
  image->addSeparator();
  image->addAction(tr("&Close"), QKeySequence::Close, this, &QWidget::close);

  statusBar()->showMessage(tr("Read-only. Loading %1 images from %2…")
                               .arg(items_.size())
                               .arg(QString::fromStdString(ta::fs::toUtf8(folder_))));
  startLoading();
}

void ViewerWindow::startLoading() {
  QPointer<ViewerWindow> self(this);
  auto cancel = cancel_;
  auto items = items_;
  loader_ = QThread::create([self, cancel, items] {
    for (const auto& it : items) {
      if (*cancel) return;
      auto frame = std::make_shared<ta::Frame>();
      std::string err;
      FramePtr result;
      QImage thumb;
      if (ta::readImageFile(it.file, *frame, &err)) {
        thumb = renderThumbnail(*frame, it.orientation);
        result = frame;
      }
      const QString error = QString::fromStdString(err);
      QMetaObject::invokeMethod(
          qApp,
          [self, seq = it.seq, result, thumb, error] {
            if (self) self->imageLoaded(seq, result, thumb, error);
          },
          Qt::QueuedConnection);
    }
  });
  connect(loader_, &QThread::finished, loader_, &QObject::deleteLater);
  loader_->start(QThread::LowPriority);
}

void ViewerWindow::imageLoaded(uint seq, FramePtr frame, const QImage& thumb, const QString& error) {
  ++loaded_;
  if (frame) {
    frames_[seq] = frame;
    mount_->setThumbnail(seq, thumb);
    if (current_ < 0) showImage(int(seq));
  } else {
    problems_ << error;
  }
  if (loaded_ < int(items_.size())) {
    statusBar()->showMessage(tr("Read-only. Loaded %1 of %2 images…").arg(loaded_).arg(items_.size()));
    return;
  }
  statusBar()->showMessage(tr("Read-only. %1 of %2 images shown, from %3")
                               .arg(frames_.size())
                               .arg(items_.size())
                               .arg(QString::fromStdString(ta::fs::toUtf8(folder_))));
  if (!problems_.isEmpty())
    QMessageBox::warning(this, windowTitle(), tr("Some images could not be shown:\n\n%1").arg(problems_.join("\n")));
}

void ViewerWindow::showImage(int seq) {
  const auto it = frames_.find(uint(seq));
  if (it == frames_.end()) return;
  current_ = seq;
  mount_->setSelectedImage(seq);
  view_->setImage(it.value(), shown_.value(uint(seq)));
}

void ViewerWindow::turn(int quarterTurns, bool mirror) {
  if (current_ < 0) return;
  const uint seq = uint(current_);
  ta::Orientation o = shown_.value(seq);
  if (quarterTurns) o = o.rotated(quarterTurns);
  if (mirror) o = o.mirrored();
  shown_[seq] = o;
  view_->setOrientation(o);
  if (const auto it = frames_.find(seq); it != frames_.end()) mount_->setThumbnail(seq, renderThumbnail(*it.value(), o));
}
