// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_SELECT_CONTENT_LAYOUTDETECTOR_H_
#define SCANTAILOR_SELECT_CONTENT_LAYOUTDETECTOR_H_

#include <QCoreApplication>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <memory>
#include <optional>
#include <vector>

#include "LayoutRules.h"

class ImageTransformation;
class PageId;
class QImage;
class QJsonObject;

namespace select_content {
/**
 * What the layout detector found on one page of the "Select Content" stage. All rectangles are
 * in the stage's coordinates (those of the content box).
 */
struct LayoutResult {
  // Inputs the result depends on (a changed page geometry or model means a new run; in the
  // automatic content mode also a changed automatic box, see autoBox).
  QTransform xform;
  QPolygonF cropArea;
  QRectF pageRect;
  QString modelSha256;
  double frameDpi = 600;

  QRectF autoBox;  // automatic content box used by the side-strip tab rule
  QRectF paper;    // the paper inside the page (scanner background excluded)
  std::vector<layout::Detection> detections;  // every model detection above the threshold
  std::vector<layout::Element> elements;      // the page elements after the rules
  double seconds = 0;

  bool matches(const ImageTransformation& xform, const QRectF& pageRect, const QString& modelSha256) const;

  /**
   * The content box with the toggles \p flags (layout::finalBox) from the automatic box
   * \p autoBox (may be null) and this page's elements.
   */
  layout::FinalBox finalBox(const QRectF& autoBox, unsigned flags, layout::PageSide side) const;

  /** For the CLI report. */
  QJsonObject toJson() const;
};

/** Rectangle <-> layout::Box. */
QRectF layoutBoxToRect(const layout::Box& b);

std::optional<layout::Box> rectToLayoutBox(const QRectF& r);

/** Left / right page of a split scan, or single. */
layout::PageSide pageSideOf(const PageId& pageId);

/**
 * Layout model (ONNX, e.g. a fine-tuned Docling Heron / RT-DETRv2: input pixel_values
 * 1x3xHxW, outputs logits 1xQxC and pred_boxes 1xQx4) run through ONNX Runtime on the CPU, plus
 * the rules that make page elements of its detections (LayoutRules.h).
 *
 * The model is chosen by configure() (CLI --layout-model) or the environment variable
 * SCANTAILOR_LAYOUT_MODEL; SCANTAILOR_LAYOUT_MODEL_SHA256 / --layout-model-sha256 make loading
 * fail unless the file has that hash. Without a model, or in a build without ONNX Runtime
 * (ENABLE_LAYOUT_DETECTOR), instance() is null and nothing changes.
 *
 * Deterministic: fixed number of ONNX Runtime threads, sequential execution, one model run at a
 * time.
 */
class LayoutDetector {
  Q_DECLARE_TR_FUNCTIONS(select_content::LayoutDetector)
 public:
  static constexpr double kScoreThreshold = 0.3;  // the fine-tuned model's validation threshold
  static constexpr double kNmsIou = 0.5;
  static constexpr double kRenderDpi = 300;       // the rules and the model input work at this resolution
  static constexpr int kModelLongSide = 1024;     // page scaled to this before the model's input size
  static constexpr int kIntraOpThreads = 4;

  /** Whether this build contains the detector (ONNX Runtime found at build time). */
  static bool isBuilt();

  /**
   * Loads the model and makes it the instance. Returns an error message (empty = fine).
   * \p expectedSha256 (hex) may be empty; otherwise a file with another hash is refused.
   */
  static QString configure(const QString& modelPath, const QString& expectedSha256);

  /**
   * The configured detector, or null. The first call without configure() reads
   * SCANTAILOR_LAYOUT_MODEL (and SCANTAILOR_LAYOUT_MODEL_SHA256); a failure is printed once.
   */
  static std::shared_ptr<LayoutDetector> instance();

  /** No detector from now on (the environment is not read again). */
  static void unload();

  /**
   * GUI start-up and settings: the model of the application settings if \p modelPath is not
   * empty (a failure leaves no detector), else the environment as instance() would read it.
   * Returns an error message (empty = fine or no model).
   */
  static QString configureFromSettings(const QString& modelPath, const QString& expectedSha256);

  ~LayoutDetector();

  const QString& modelPath() const { return m_path; }

  const QString& modelSha256() const { return m_sha256; }

  /**
   * Runs the model and the rules on the page as the stage sees it: \p image transformed by
   * \p xform, rendered at kRenderDpi over [0, pageRect.right()] x [0, pageRect.bottom()], white
   * outside the page outline. \p autoBox: the automatic content box (may be null).
   * Thread-safe; model runs are serialised.
   */
  std::shared_ptr<LayoutResult> analyze(const QImage& image,
                                        const ImageTransformation& xform,
                                        const QRectF& pageRect,
                                        const QRectF& autoBox) const;

 private:
  struct Impl;

  LayoutDetector();

  std::unique_ptr<Impl> m_impl;
  QString m_path;
  QString m_sha256;
};
}  // namespace select_content
#endif  // SCANTAILOR_SELECT_CONTENT_LAYOUTDETECTOR_H_
