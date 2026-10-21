/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>
#include <memory>
#include <vector>

#include "AppTypes.hpp"
#include "fs.hpp"

// Owns the staging session on its own thread, so writing and flushing images
// never stalls the interface. Every change is on disk before it is reported.
class Stager : public QObject {
  Q_OBJECT

 public:
  explicit Stager(QString root, QObject* parent = nullptr);

  static constexpr int kThumbSize = 384;

 public slots:
  void startSession(QString layout, QString source, ta::PatientInfo patient);
  void resumeSession(QString dir);
  void addFrame(QByteArray payload, ta::NormalizeOptions options, ta::Orientation orientation, int slot);
  void place(uint seq, int slot);
  void setOrientation(uint seq, ta::Orientation orientation);
  void discard(uint seq);
  void setPatient(ta::PatientInfo patient);
  void closeSession(QString reason);
  void loadImage(uint seq);

  // Images that could not be written (disk full, no permission, no session)
  // are kept here, in order, until one of these resolves them.
  void retryPending();
  void savePendingTo(QString dir);
  void discardPending();

 signals:
  void sessionChanged(ta::SessionState state, QString dir);
  void sessionClosed(QString dir);
  void thumbnailReady(uint seq, QImage thumbnail);
  void imageStaged(uint seq, int slot);
  void imageLoaded(uint seq, FramePtr frame);
  void problem(QString text);
  // `count` images are held in memory, not saved; the newest reason is given.
  void stagingFailed(int count, QString reason);
  void pendingCleared();

 private:
  void emitState();
  void makeThumbnail(uint seq, const ta::Frame& frame, const ta::Orientation& o);

  struct Pending {
    ta::Frame frame;
    ta::Orientation orientation;
    int slot = -1;
  };
  bool stage(Pending& p, std::string* err);

  ta::fs::Path root_;
  std::vector<Pending> pending_;
  std::unique_ptr<ta::StagingSession> session_;
};
