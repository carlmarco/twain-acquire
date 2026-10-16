#include "MountView.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>

namespace {
constexpr const char* kMime = "application/x-twain-acquire-image";
constexpr int kGap = 6;
}  // namespace

MountView::MountView(QWidget* parent) : QWidget(parent) {
  setAcceptDrops(true);
  setMinimumSize(240, 160);
}

void MountView::setLayout(const ta::Layout& layout) {
  layout_ = layout;
  target_ = std::clamp(target_, 0, std::max(0, int(layout_.slotList.size()) - 1));
  update();
}

void MountView::setSession(const ta::SessionState& state) {
  state_ = state;
  update();
}

void MountView::setThumbnail(uint seq, const QImage& image) {
  thumbs_[seq] = image;
  update();
}

void MountView::clearSession() {
  state_ = ta::SessionState{};
  thumbs_.clear();
  selected_ = -1;
  target_ = 0;
  update();
}

void MountView::setTargetSlot(int slot) {
  target_ = slot;
  update();
}

void MountView::setSelectedImage(int seq) {
  selected_ = seq;
  update();
}

int MountView::imageInSlot(int slot) const {
  for (const auto& img : state_.images)
    if (!img.discarded && img.slot == slot) return int(img.seq);
  return -1;
}

std::vector<bool> MountView::filledSlots() const {
  std::vector<bool> filled(layout_.slotList.size(), false);
  for (const auto& img : state_.images)
    if (!img.discarded && img.slot >= 0 && img.slot < int(filled.size())) filled[size_t(img.slot)] = true;
  return filled;
}

std::vector<MountView::Cell> MountView::cells() const {
  std::vector<Cell> out;
  std::vector<int> unplaced;
  for (const auto& img : state_.images)
    if (!img.discarded && (img.slot < 0 || img.slot >= int(layout_.slotList.size()))) unplaced.push_back(int(img.seq));

  const int stripH = unplaced.empty() ? 0 : std::max(60, height() / 5);
  const QRect grid = rect().adjusted(kGap, kGap, -kGap, -kGap - (stripH ? stripH + kGap : 0));
  const int cols = std::max(1, layout_.columns), rows = std::max(1, layout_.rows);
  const int cw = (grid.width() - (cols - 1) * kGap) / cols;
  const int ch = (grid.height() - (rows - 1) * kGap) / rows;
  for (size_t i = 0; i < layout_.slotList.size(); ++i) {
    const auto& s = layout_.slotList[i];
    Cell c;
    c.rect = QRect(grid.left() + s.column * (cw + kGap), grid.top() + s.row * (ch + kGap), cw, ch);
    c.slot = int(i);
    c.seq = imageInSlot(int(i));
    c.label = QString::fromStdString(s.label);
    out.push_back(c);
  }
  if (stripH) {
    const QRect strip(kGap, height() - kGap - stripH, width() - 2 * kGap, stripH);
    const int w = std::min(strip.height(), (strip.width() - kGap * (int(unplaced.size()) - 1)) / int(unplaced.size()));
    for (size_t i = 0; i < unplaced.size(); ++i) {
      Cell c;
      c.rect = QRect(strip.left() + int(i) * (w + kGap), strip.top(), w, strip.height());
      c.slot = -1;
      c.seq = unplaced[i];
      c.label = tr("Not placed");
      out.push_back(c);
    }
  }
  return out;
}

std::optional<MountView::Cell> MountView::cellAt(const QPoint& p) const {
  for (const auto& c : cells())
    if (c.rect.contains(p)) return c;
  return std::nullopt;
}

void MountView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), palette().window());
  const QColor frameColor = palette().mid().color();
  const QColor targetColor(30, 120, 220);
  const QColor selectColor(240, 170, 20);
  for (const auto& c : cells()) {
    p.fillRect(c.rect, QColor(20, 20, 20));
    const QRect inner = c.rect.adjusted(3, 3, -3, -18);
    if (c.seq >= 0 && thumbs_.contains(uint(c.seq))) {
      const QImage& t = thumbs_[uint(c.seq)];
      QSize s = t.size().scaled(inner.size(), Qt::KeepAspectRatio);
      QRect r(QPoint(0, 0), s);
      r.moveCenter(inner.center());
      p.drawImage(r, t);
    } else if (c.seq < 0) {
      p.setPen(QColor(110, 110, 110));
      p.drawText(inner, Qt::AlignCenter, tr("empty"));
    }
    p.setPen(QColor(220, 220, 220));
    p.drawText(QRect(c.rect.left() + 3, c.rect.bottom() - 16, c.rect.width() - 6, 15),
               Qt::AlignCenter | Qt::TextSingleLine, p.fontMetrics().elidedText(c.label, Qt::ElideRight, c.rect.width() - 6));
    QPen pen(frameColor, 1);
    if (c.slot >= 0 && c.slot == target_) pen = QPen(targetColor, 3);
    if (c.seq >= 0 && c.seq == selected_) pen = QPen(selectColor, 3);
    p.setPen(pen);
    p.drawRect(c.rect.adjusted(1, 1, -1, -1));
  }
}

void MountView::mousePressEvent(QMouseEvent* e) {
  const auto c = cellAt(e->pos());
  pressPos_ = e->pos();
  pressSeq_ = c ? c->seq : -1;
  if (!c || e->button() != Qt::LeftButton) return;
  if (c->slot >= 0) {
    target_ = c->slot;
    emit slotChosen(c->slot, c->seq);
  }
  if (c->seq >= 0) {
    selected_ = c->seq;
    emit imageChosen(c->seq);
  }
  update();
}

void MountView::mouseMoveEvent(QMouseEvent* e) {
  if (!(e->buttons() & Qt::LeftButton) || pressSeq_ < 0) return;
  if ((e->pos() - pressPos_).manhattanLength() < QApplication::startDragDistance()) return;
  auto* mime = new QMimeData;
  mime->setData(kMime, QByteArray::number(pressSeq_));
  auto* drag = new QDrag(this);
  drag->setMimeData(mime);
  if (thumbs_.contains(uint(pressSeq_))) drag->setPixmap(QPixmap::fromImage(thumbs_[uint(pressSeq_)].scaled(64, 64, Qt::KeepAspectRatio)));
  pressSeq_ = -1;
  drag->exec(Qt::MoveAction);
}

void MountView::contextMenuEvent(QContextMenuEvent* e) {
  const auto c = cellAt(e->pos());
  if (c && c->seq >= 0) emit contextRequested(c->seq, e->globalPos());
}

void MountView::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasFormat(kMime)) e->acceptProposedAction();
}

void MountView::dragMoveEvent(QDragMoveEvent* e) {
  if (e->mimeData()->hasFormat(kMime) && cellAt(e->position().toPoint()).has_value()) e->acceptProposedAction();
  else e->ignore();
}

void MountView::dropEvent(QDropEvent* e) {
  const auto c = cellAt(e->position().toPoint());
  if (!c || !e->mimeData()->hasFormat(kMime)) return;
  const uint seq = e->mimeData()->data(kMime).toUInt();
  emit moveRequested(seq, c->slot);
  e->acceptProposedAction();
}
