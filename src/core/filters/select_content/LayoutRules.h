// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_SELECT_CONTENT_LAYOUTRULES_H_
#define SCANTAILOR_SELECT_CONTENT_LAYOUTRULES_H_

#include <QString>
#include <QStringList>
#include <optional>
#include <utility>
#include <vector>

#include "LayoutImage.h"

namespace select_content {
namespace layout {
/**
 * Post-processing of a layout model's output and the image rules that turn its detections
 * into page elements (page number, thumb-index tab, header, marginalia). A port of the Python
 * reference of scantailor-cli (ml/onnx_predict.py, ml/models/mapping.py, ml/rules.py,
 * ml/combine.py); every function is pure.
 *
 * "Frame" coordinates are those of the stage's image (ImageTransformation::transform(), source
 * pixels after deskew); the rules work on the page rendered at a lower resolution (\p dpi).
 */
struct Box {
  double x0 = 0;
  double y0 = 0;
  double x1 = 0;
  double y1 = 0;

  double width() const { return x1 - x0; }

  double height() const { return y1 - y0; }

  bool isValid() const { return x1 > x0 && y1 > y0; }

  Box scaled(double f) const { return {x0 * f, y0 * f, x1 * f, y1 * f}; }

  bool operator==(const Box& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
};

/** One detection of the model. */
struct Detection {
  QString raw;      // the model's class name
  QString cls;      // ours (page_number, tab, header, marginalia) or empty
  QString clsRule;  // cls, or "tab" by the side-strip rule (tabRule)
  double score = 0;
  Box box;
};

/** One final page element. */
struct Element {
  QString type;  // page_number, tab, header, marginalia
  QString raw;
  double score = 0;
  Box box;        // final box (tabs grown, clipped to the paper)
  Box modelBox;   // the model's box
};

/** Classes in the order of the fine-tuned Heron head (scantailor-cli ml/ft_heron.py). */
std::vector<QString> defaultLabels();

/**
 * RT-DETR post-processing (transformers' RTDetrImageProcessor.post_process_object_detection
 * with focal loss, then torchvision's class-wise NMS): sigmoid scores, top numQueries over all
 * (query, class) pairs, score > threshold, boxes (cx, cy, w, h in 0..1) scaled to
 * imgWidth x imgHeight, NMS per class at IoU > nmsIou. Sorted by decreasing score.
 * Only raw, score and box are set.
 */
std::vector<Detection> decodeDetections(const float* logits,
                                        const float* boxes,
                                        int numQueries,
                                        int numClasses,
                                        const std::vector<QString>& labels,
                                        int imgWidth,
                                        int imgHeight,
                                        double threshold,
                                        double nmsIou);

/** IoU of two boxes (torchvision's box_iou). */
double iou(const Box& a, const Box& b);

/** Model class -> ours (mapping.map_class); \p box in frame px of \p frameDpi. Empty = not ours. */
QString mapClass(const QString& raw, const Box& box, double frameDpi);

/**
 * mapping.tab_rule: a detection in the side strip of the page (<= 18 mm from its left or right
 * edge), <= 18 x 50 mm, centre outside the automatic content box -> tab.
 */
bool tabRule(const Box& box, const Box& pageRect, const std::optional<Box>& autoBox, double frameDpi);

/** First longest run of true values as [begin, end); (0, n) if there is none. */
std::pair<int, int> longestRun(const std::vector<bool>& mask);

/** The run of true values through \p centre (or the nearest one), as [begin, end). */
std::optional<std::pair<int, int>> runThrough(const std::vector<bool>& mask, double centre);

/** Intersection; empty if it has no area. */
std::optional<Box> clip(const Box& box, const Box& rect);

/**
 * rules.paper_rect: the paper inside the page (the scanner background is near-black): blur
 * 1 mm, a column (row) is paper if >= 50 % of its pixels are > 90; the longest run of them.
 */
Box paperRect(const Plane8& gray, double dpi);

/** rules.grow_tab: the tab around a model's tab box, halftone-robust. Empty = keep the model box. */
std::optional<Box> growTab(const Plane8& gray, double dpi, const Box& box, const Box& paper);

/** Gap from the box to the nearer side of the paper, mm. */
double outerGapMm(const Box& box, const Box& paper, double dpi);

/** rules.outer_ok: centre on the paper and gap to the nearer side <= 18 mm. */
bool outerOk(const Box& box, const Box& paper, double dpi);

/**
 * combine.py, per page: tabs (cls or clsRule "tab") that pass outerOk, grown with growTab (else
 * clipped to the paper), then page_number / header / marginalia (by cls) clipped to the paper.
 * \p dets and the result are in the rendered page's px (\p gray, \p dpi).
 */
std::vector<Element> selectElements(const std::vector<Detection>& dets,
                                    const Plane8& gray,
                                    double dpi,
                                    const Box& paper);

/**
 * What the content box takes in beyond the automatic box: the per-page toggles of the
 * "Select Content" stage (a bit set; 0 = the automatic box unchanged).
 */
enum IncludeFlag : unsigned {
  kIncludePageNumbers = 1u << 0,
  kIncludeTabs = 1u << 1,
  kIncludeHeaders = 1u << 2,
  kIncludeMarginalia = 1u << 3,
  kCleanPaperEdge = 1u << 4,
};

constexpr unsigned kIncludeAll
    = kIncludePageNumbers | kIncludeTabs | kIncludeHeaders | kIncludeMarginalia | kCleanPaperEdge;

/** The flag of an element type (page_number, tab, header, marginalia); 0 for anything else. */
unsigned includeFlagOfType(const QString& type);

/**
 * "page-numbers,tabs,headers,marginalia,clean-edge" (any subset, any order; "none" = 0; "all")
 * -> flags. Unknown names set \p error and return 0.
 */
unsigned parseIncludeList(const QString& list, QString* error);

/** Flags -> the names of parseIncludeList, in that order. */
QStringList includeListNames(unsigned flags);

enum class PageSide { Single, Left, Right };

constexpr double kEdgeTouchMm = 3;    // a box side this close to the paper edge (or beyond) "touches" it
constexpr double kEdgePadMm = 1;      // added to the model's extent (its boxes end short of the print)
constexpr double kEdgeSeenScore = 0.5;  // detections counted for the extent

/** Result of finalBox(). */
struct FinalBox {
  std::optional<Box> box;     // empty: no content (no automatic box, nothing included)
  std::vector<int> included;  // indices of the included elements
  QStringList cleaned;        // the paper-edge cuts made ("outer to tab", "left to model extent", ...)
};

/**
 * The content box with the toggles \p flags (a port of scantailor-cli's ml/apply_boxes.py):
 *
 * 1. automatic box U every element whose type is included, each clipped to \p pageRect
 *    (ml/score.final_with_elements);
 * 2. with kCleanPaperEdge (apply_boxes.clean), only ever shrinking:
 *    - outer side of a left / right page that has a tab (the tab of highest score): cut at the
 *      tab's outer edge;
 *    - every side within kEdgeTouchMm of the paper edge or beyond it: cut back to the extent of
 *      everything the model saw (detections >= kEdgeSeenScore of any class, body text included,
 *      and all elements, included or not) + kEdgePadMm; not the outer side of a tab page.
 *    A cut that leaves no area is dropped (the box before the cuts is kept).
 *
 * The paper-edge cut uses every element, whatever is included. All boxes in frame px of
 * \p frameDpi.
 */
FinalBox finalBox(const std::optional<Box>& autoBox,
                  const Box& pageRect,
                  const std::vector<Element>& elements,
                  const std::vector<Detection>& detections,
                  const Box& paper,
                  unsigned flags,
                  PageSide side,
                  double frameDpi);
}  // namespace layout
}  // namespace select_content
#endif  // SCANTAILOR_SELECT_CONTENT_LAYOUTRULES_H_
