/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "Stager.hpp"

#include <algorithm>
#include <vector>

#include "render.hpp"
#include "tiff.hpp"

#include <cstdio>

void registerAppTypes() {
  qRegisterMetaType<ta::Kv>();
  qRegisterMetaType<ta::SessionState>();
  qRegisterMetaType<ta::PatientInfo>();
  qRegisterMetaType<ta::Orientation>();
  qRegisterMetaType<ta::NormalizeOptions>();
  qRegisterMetaType<FramePtr>();
}

Stager::Stager(QString root, QObject* parent) : QObject(parent), root_(ta::fs::fromUtf8(root.toStdString())) {}

void Stager::emitState() {
  if (session_) emit sessionChanged(session_->state(), QString::fromStdString(ta::fs::toUtf8(session_->dir())));
}

void Stager::startSession(QString layout, QString source, ta::PatientInfo patient) {
  std::string err;
  auto s = ta::StagingSession::create(root_, layout.toStdString(), source.toStdString(), patient, &err);
  if (!s) {
    emit problem(tr("Could not start a session: %1").arg(QString::fromStdString(err)));
    return;
  }
  session_ = std::move(s);
  emitState();
}

void Stager::resumeSession(QString dir) {
  std::string err;
  auto s = ta::StagingSession::resume(ta::fs::fromUtf8(dir.toStdString()), &err);
  if (!s) {
    emit problem(tr("Could not reopen the session: %1").arg(QString::fromStdString(err)));
    return;
  }
  session_ = std::move(s);
  emitState();
  for (const auto& img : session_->state().images) {
    if (img.discarded) continue;
    ta::Frame f;
    if (session_->loadImage(img.seq, f, &err)) makeThumbnail(img.seq, f, img.orientation);
    else emit problem(QString::fromStdString(err));
  }
}

void Stager::makeThumbnail(uint seq, const ta::Frame& f, const ta::Orientation& o) {
  const ta::OrientMap m = ta::orientMap(f.width, f.height, o);
  const double scale = std::min(double(kThumbSize) / m.width, double(kThumbSize) / m.height);
  const int w = std::max(1, int(m.width * scale)), h = std::max(1, int(m.height * scale));
  std::vector<uint8_t> lut;
  ta::buildLut(ta::autoWindow(f), f.bitsStored, 1.0, false, lut);
  QImage img(w, h, QImage::Format_Grayscale8);
  ta::renderGray8(f, o, lut, 0, 0, m.width, m.height, img.bits(), w, h, int(img.bytesPerLine()));
  emit thumbnailReady(seq, img);
}

void Stager::addFrame(QByteArray payload, ta::NormalizeOptions options, ta::Orientation orientation, int slot) {
  ta::Frame f;
  std::string err;
  if (!ta::normalizeTransfer(reinterpret_cast<const uint8_t*>(payload.constData()), size_t(payload.size()),
                             options, f, &err)) {
    emit problem(tr("An image could not be read: %1").arg(QString::fromStdString(err)));
    return;
  }
  f.acquiredUnixMs = ta::fs::nowUnixMs();
  Pending p{std::move(f), orientation, slot};
  // Later images queue behind an unsaved one, so order is kept.
  if (!pending_.empty() || !stage(p, &err)) {
    if (err.empty()) err = tr("an earlier image is still unsaved").toStdString();
    pending_.push_back(std::move(p));
    emit stagingFailed(int(pending_.size()), QString::fromStdString(err));
  }
}

bool Stager::stage(Pending& p, std::string* err) {
  if (!session_) {
    if (err) *err = tr("no session is open").toStdString();
    return false;
  }
  uint32_t seq = 0;
  if (!session_->addImage(p.frame, p.slot, &seq, err)) return false;
  std::string ignored;
  if (!(p.orientation == ta::Orientation{})) session_->setOrientation(seq, p.orientation, &ignored);
  // A retake into a filled slot keeps the earlier image, unplaced.
  for (const auto& img : session_->state().images)
    if (p.slot >= 0 && img.slot == p.slot && img.seq != seq && !img.discarded) session_->place(img.seq, -1, &ignored);
  makeThumbnail(seq, p.frame, p.orientation);
  emitState();
  emit imageStaged(seq, p.slot);
  emit imageLoaded(seq, std::make_shared<const ta::Frame>(std::move(p.frame)));
  return true;
}

void Stager::retryPending() {
  std::string err;
  while (!pending_.empty()) {
    if (!stage(pending_.front(), &err)) {
      emit stagingFailed(int(pending_.size()), QString::fromStdString(err));
      return;
    }
    pending_.erase(pending_.begin());
  }
  emit pendingCleared();
}

void Stager::savePendingTo(QString dir) {
  const ta::fs::Path out = ta::fs::fromUtf8(dir.toStdString());
  int n = 0;
  while (!pending_.empty()) {
    const Pending& p = pending_.front();
    const std::vector<uint8_t> bytes = ta::encodeTiff(p.frame, 16);
    char name[64];
    std::snprintf(name, sizeof name, "unsaved-%lld-%d.tif", static_cast<long long>(p.frame.acquiredUnixMs), ++n);
    std::string err;
    if (bytes.empty() || !ta::fs::atomicWrite(out / name, bytes.data(), bytes.size(), &err)) {
      emit stagingFailed(int(pending_.size()), QString::fromStdString(err.empty() ? "image too large" : err));
      return;
    }
    pending_.erase(pending_.begin());
  }
  emit pendingCleared();
}

void Stager::discardPending() {
  pending_.clear();
  emit pendingCleared();
}

void Stager::place(uint seq, int slot) {
  if (!session_) return;
  std::string err;
  // A slot holds one image: whatever was there becomes unplaced.
  for (const auto& img : session_->state().images)
    if (slot >= 0 && img.slot == slot && img.seq != seq && !img.discarded) session_->place(img.seq, -1, &err);
  if (!session_->place(seq, slot, &err)) emit problem(QString::fromStdString(err));
  emitState();
}

void Stager::setOrientation(uint seq, ta::Orientation orientation) {
  if (!session_) return;
  std::string err;
  if (!session_->setOrientation(seq, orientation, &err)) {
    emit problem(QString::fromStdString(err));
    return;
  }
  ta::Frame f;
  if (session_->loadImage(seq, f, &err)) makeThumbnail(seq, f, orientation);
  emitState();
}

void Stager::discard(uint seq) {
  if (!session_) return;
  std::string err;
  if (!session_->discard(seq, &err)) emit problem(QString::fromStdString(err));
  emitState();
}

void Stager::setPatient(ta::PatientInfo patient) {
  if (!session_) return;
  std::string err;
  if (!session_->setPatient(patient, &err)) emit problem(QString::fromStdString(err));
  emitState();
}

void Stager::closeSession(QString reason) {
  if (!session_) return;
  std::string err;
  if (!session_->close(reason.toStdString(), &err)) emit problem(QString::fromStdString(err));
  const QString dir = QString::fromStdString(ta::fs::toUtf8(session_->dir()));
  session_.reset();
  emit sessionClosed(dir);
}

void Stager::loadImage(uint seq) {
  if (!session_) return;
  auto f = std::make_shared<ta::Frame>();
  std::string err;
  if (!session_->loadImage(seq, *f, &err)) {
    emit problem(QString::fromStdString(err));
    return;
  }
  emit imageLoaded(seq, f);
}
