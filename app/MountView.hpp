/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <QHash>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QWidget>
#include <optional>
#include <vector>

#include "AppTypes.hpp"
#include "layout.hpp"

// The mount: the layout's slots with the image placed in each, plus a strip
// of images not placed in any slot. The highlighted slot receives the next
// exposure. Images can be dragged between slots.
class MountView : public QWidget {
  Q_OBJECT

 public:
  explicit MountView(QWidget* parent = nullptr);

  void setLayout(const ta::Layout& layout);
  void setSession(const ta::SessionState& state);
  void setThumbnail(uint seq, const QImage& image);
  void clearSession();

  void setTargetSlot(int slot);
  int targetSlot() const { return target_; }
  void setSelectedImage(int seq);
  int selectedImage() const { return selected_; }

  // seq of the image in a slot, or -1.
  int imageInSlot(int slot) const;
  std::vector<bool> filledSlots() const;

  QSize sizeHint() const override { return {520, 300}; }

 signals:
  void slotChosen(int slot, int seq);  // seq -1 when the slot is empty
  void imageChosen(int seq);
  void moveRequested(uint seq, int slot);  // slot -1 = unplace
  void contextRequested(int seq, QPoint globalPos);

 protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void contextMenuEvent(QContextMenuEvent*) override;
  void dragEnterEvent(QDragEnterEvent*) override;
  void dragMoveEvent(QDragMoveEvent*) override;
  void dropEvent(QDropEvent*) override;

 private:
  struct Cell {
    QRect rect;
    int slot = -1;  // -1 = unplaced strip
    int seq = -1;
    QString label;
  };
  std::vector<Cell> cells() const;
  std::optional<Cell> cellAt(const QPoint& p) const;

  ta::Layout layout_;
  ta::SessionState state_;
  QHash<uint, QImage> thumbs_;
  int target_ = 0;
  int selected_ = -1;
  QPoint pressPos_;
  int pressSeq_ = -1;
};
