/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QStringList>
#include <atomic>
#include <memory>
#include <vector>

#include "AppTypes.hpp"
#include "fs.hpp"
#include "layout.hpp"
#include "manifest.hpp"

class ImageView;
class MountView;
class QThread;

// Shows images that were exported earlier, as they were mounted. Read-only:
// nothing here can change the files, a session or the sensor, and turning an
// image only changes how it is shown.
class ViewerWindow : public QMainWindow {
  Q_OBJECT
 public:
  // From a mount file in `folder`.
  ViewerWindow(const ta::fs::Path& folder, const ta::MountManifest& manifest, QWidget* parent = nullptr);
  // Image files with no mount file, shown in name order.
  ViewerWindow(const ta::fs::Path& folder, const std::vector<ta::fs::Path>& files, QWidget* parent = nullptr);
  ~ViewerWindow() override;

 private:
  struct Item {
    ta::fs::Path file;
    uint seq = 0;
    int slot = -1;
    ta::Orientation orientation;  // still to apply for display
  };

  void build(const ta::Layout& layout, const QString& title);
  void startLoading();
  void imageLoaded(uint seq, FramePtr frame, const QImage& thumb, const QString& error);
  void showImage(int seq);
  void turn(int quarterTurns, bool mirror);

  MountView* mount_ = nullptr;
  ImageView* view_ = nullptr;
  ta::fs::Path folder_;
  std::vector<Item> items_;
  QHash<uint, FramePtr> frames_;
  QHash<uint, ta::Orientation> shown_;  // orientation per image as currently shown
  int current_ = -1;
  int loaded_ = 0;
  QStringList problems_;
  std::shared_ptr<std::atomic_bool> cancel_ = std::make_shared<std::atomic_bool>(false);
  QPointer<QThread> loader_;
};
