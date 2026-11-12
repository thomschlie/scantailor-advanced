// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "Thumbnail.h"

#include <QPainter>
#include <algorithm>
#include <utility>

namespace deskew {
Thumbnail::Thumbnail(std::shared_ptr<ThumbnailPixmapCache> thumbnailCache,
                     const QSizeF& maxSize,
                     const ImageId& imageId,
                     const ImageTransformation& xform,
                     bool deviant,
                     bool uncertain)
    : ThumbnailBase(std::move(thumbnailCache), maxSize, imageId, xform), m_deviant(deviant), m_uncertain(uncertain) {}

void Thumbnail::prePaintOverImage(QPainter& painter,
                                  const QTransform& imageToDisplay,
                                  const QTransform& thumbToDisplay) {
  painter.setRenderHint(QPainter::Antialiasing, false);

  QPen pen(QColor(0, 0, 0xd1, 70));
  pen.setWidth(1);
  pen.setCosmetic(true);
  painter.setPen(pen);

  const QRectF boundingRect(this->boundingRect());

  const double cellSize = 8;
  const double left = boundingRect.left();
  const double right = boundingRect.right();
  const double top = boundingRect.top();
  const double bottom = boundingRect.bottom();
  const double w = boundingRect.width();
  const double h = boundingRect.height();

  const QPointF center(boundingRect.center());
  QVector<QLineF> lines;
  for (double y = center.y(); y > 0.0; y -= cellSize) {
    lines.push_back(QLineF(left, y, right, y));
  }
  for (double y = center.y(); (y += cellSize) < h;) {
    lines.push_back(QLineF(left, y, right, y));
  }
  for (double x = center.x(); x > 0.0; x -= cellSize) {
    lines.push_back(QLineF(x, top, x, bottom));
  }
  for (double x = center.x(); (x += cellSize) < w;) {
    lines.push_back(QLineF(x, top, x, bottom));
  }
  painter.drawLines(lines);

  if (m_deviant) {
    paintDeviant(painter);
  }
  if (m_uncertain) {
    paintUncertain(painter);
  }
}  // Thumbnail::paintOverImage

void Thumbnail::paintUncertain(QPainter& painter) {
  const QRectF bounds(boundingRect());
  const double d = std::max(10.0, std::min(bounds.width(), bounds.height()) / 4.0);
  const QRectF badge(bounds.right() - d - 2.0, bounds.top() + 2.0, d, d);

  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(0xe0, 0x70, 0x00, 0xe0));
  painter.drawEllipse(badge);
  QFont font;
  font.setBold(true);
  font.setPixelSize(static_cast<int>(d * 0.75));
  painter.setFont(font);
  painter.setPen(Qt::white);
  painter.drawText(badge, Qt::AlignCenter, QStringLiteral("?"));
  painter.restore();
}
}  // namespace deskew