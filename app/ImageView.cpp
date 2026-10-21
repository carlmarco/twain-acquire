/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ImageView.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

ImageView::ImageView(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_OpaquePaintEvent);
  setMouseTracking(false);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(200, 200);
}

void ImageView::setImage(FramePtr frame, const ta::Orientation& orientation) {
  frame_ = std::move(frame);
  orientation_ = orientation;
  if (frame_) {
    map_ = ta::orientMap(frame_->width, frame_->height, orientation_);
    window_ = ta::autoWindow(*frame_);
    rebuildLut();
    fit();
  }
  invalidate();
}

void ImageView::setOrientation(const ta::Orientation& orientation) {
  if (!frame_) return;
  orientation_ = orientation;
  map_ = ta::orientMap(frame_->width, frame_->height, orientation_);
  fit();
}

void ImageView::clear() {
  frame_.reset();
  invalidate();
}

double ImageView::fitScale() const {
  if (!frame_ || map_.width == 0 || map_.height == 0) return 1.0;
  return std::min(double(width()) / map_.width, double(height()) / map_.height);
}

void ImageView::fit() {
  fitted_ = true;
  scale_ = fitScale();
  center_ = QPointF(map_.width / 2.0, map_.height / 2.0);
  invalidate();
}

void ImageView::resetWindow() {
  if (!frame_) return;
  window_ = ta::autoWindow(*frame_);
  rebuildLut();
  invalidate();
}

void ImageView::setInverted(bool on) {
  inverted_ = on;
  rebuildLut();
  invalidate();
}

void ImageView::setGamma(double gamma) {
  gamma_ = gamma;
  rebuildLut();
  invalidate();
}

void ImageView::rebuildLut() {
  if (!frame_) return;
  ta::buildLut(window_, frame_->bitsStored, gamma_, inverted_, lut_);
  emit windowChanged(window_.center, window_.width);
}

void ImageView::invalidate() {
  dirty_ = true;
  update();
}

void ImageView::resizeEvent(QResizeEvent*) {
  if (fitted_ && frame_) scale_ = fitScale();
  invalidate();
}

void ImageView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  if (!frame_) {
    p.fillRect(rect(), Qt::black);
    p.setPen(QColor(150, 150, 150));
    p.drawText(rect(), Qt::AlignCenter, tr("No image selected"));
    return;
  }
  dpr_ = devicePixelRatioF();
  const int dw = std::max(1, int(std::lround(width() * dpr_)));
  const int dh = std::max(1, int(std::lround(height() * dpr_)));
  if (dirty_ || cache_.width() != dw || cache_.height() != dh) {
    if (cache_.width() != dw || cache_.height() != dh) cache_ = QImage(dw, dh, QImage::Format_Grayscale8);
    const double perDevice = 1.0 / (scale_ * dpr_);  // image pixels per device pixel
    const double sx = center_.x() - dw / 2.0 * perDevice;
    const double sy = center_.y() - dh / 2.0 * perDevice;
    ta::renderGray8(*frame_, orientation_, lut_, sx, sy, dw * perDevice, dh * perDevice, cache_.bits(), dw, dh,
                    int(cache_.bytesPerLine()));
    cache_.setDevicePixelRatio(dpr_);
    dirty_ = false;
  }
  p.drawImage(0, 0, cache_);
}

void ImageView::wheelEvent(QWheelEvent* e) {
  if (!frame_) return;
  const double steps = e->angleDelta().y() / 120.0;
  const double factor = std::pow(1.25, steps);
  const double newScale = std::clamp(scale_ * factor, fitScale() * 0.5, 16.0);
  // Keep the image point under the cursor where it is.
  const QPointF pos = e->position();
  const QPointF offset(pos.x() - width() / 2.0, pos.y() - height() / 2.0);
  const QPointF under = center_ + offset / scale_;
  center_ = under - offset / newScale;
  scale_ = newScale;
  fitted_ = false;
  invalidate();
}

void ImageView::mousePressEvent(QMouseEvent* e) {
  if (!frame_) return;
  dragStart_ = e->position();
  if (e->button() == Qt::LeftButton) {
    drag_ = Drag::Pan;
    centerAtDrag_ = center_;
  } else if (e->button() == Qt::RightButton) {
    drag_ = Drag::Window;
    windowAtDrag_ = window_;
    if (windowAtDrag_.width <= 0) {
      windowAtDrag_.width = frame_->maxValue() + 1.0;
      windowAtDrag_.center = windowAtDrag_.width / 2.0;
    }
  }
}

void ImageView::mouseMoveEvent(QMouseEvent* e) {
  if (!frame_ || drag_ == Drag::None) return;
  const QPointF d = e->position() - dragStart_;
  if (drag_ == Drag::Pan) {
    center_ = centerAtDrag_ - d / scale_;
    fitted_ = false;
    invalidate();
  } else {
    const double range = frame_->maxValue() + 1.0;
    window_.width = std::clamp(windowAtDrag_.width * std::pow(1.005, d.x()), 2.0, range * 2);
    window_.center = std::clamp(windowAtDrag_.center - d.y() * range / 1000.0, 0.0, range);
    rebuildLut();
    invalidate();
  }
}

void ImageView::mouseReleaseEvent(QMouseEvent*) { drag_ = Drag::None; }

void ImageView::mouseDoubleClickEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton) fit();
  else if (e->button() == Qt::RightButton) resetWindow();
}
