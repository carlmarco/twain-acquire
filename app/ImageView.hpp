#pragma once

#include <QImage>
#include <QPointF>
#include <QWidget>
#include <vector>

#include "AppTypes.hpp"
#include "render.hpp"

// Shows one image at full fidelity. Rendering is a CPU lookup table over the
// visible area only, redone when the view changes, so it stays fast on
// integrated graphics and needs no GPU.
//
// Mouse: wheel zooms at the cursor, left-drag pans, right-drag changes
// window (horizontal) and level (vertical), double-click fits.
class ImageView : public QWidget {
  Q_OBJECT

 public:
  explicit ImageView(QWidget* parent = nullptr);

  void setImage(FramePtr frame, const ta::Orientation& orientation);
  void setOrientation(const ta::Orientation& orientation);
  void clear();
  void fit();
  void resetWindow();
  void setInverted(bool on);
  void setGamma(double gamma);
  bool hasImage() const { return frame_ != nullptr; }

 signals:
  void windowChanged(double center, double width);

 protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;

 private:
  void rebuildLut();
  void invalidate();
  double fitScale() const;

  FramePtr frame_;
  ta::Orientation orientation_;
  ta::OrientMap map_;
  ta::WindowLevel window_;
  std::vector<uint8_t> lut_;
  bool inverted_ = false;
  double gamma_ = 1.0;

  double scale_ = 1.0;       // screen pixels per image pixel
  QPointF center_;           // image point at the widget centre
  bool fitted_ = true;

  QImage cache_;
  bool dirty_ = true;
  qreal dpr_ = 1.0;

  enum class Drag { None, Pan, Window } drag_ = Drag::None;
  QPointF dragStart_;
  QPointF centerAtDrag_;
  ta::WindowLevel windowAtDrag_;
};
