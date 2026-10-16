#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>
#include <memory>

#include "AppTypes.hpp"
#include "fs.hpp"

// Owns the staging session on its own thread, so writing and flushing images
// never stalls the interface. Every change is on disk before it is reported.
class Stager : public QObject {
  Q_OBJECT

 public:
  explicit Stager(QString root, QObject* parent = nullptr);

  static constexpr int kThumbSize = 192;

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

 signals:
  void sessionChanged(ta::SessionState state, QString dir);
  void sessionClosed(QString dir);
  void thumbnailReady(uint seq, QImage thumbnail);
  void imageStaged(uint seq, int slot);
  void imageLoaded(uint seq, FramePtr frame);
  void problem(QString text);

 private:
  void emitState();
  void makeThumbnail(uint seq, const ta::Frame& frame, const ta::Orientation& o);

  ta::fs::Path root_;
  std::unique_ptr<ta::StagingSession> session_;
};
