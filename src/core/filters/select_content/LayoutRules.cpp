// Copyright (C) 2026  scantailor-cli contributors
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "LayoutRules.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace select_content {
namespace layout {
namespace {
constexpr double kMaxTabWidthMm = 20;
constexpr double kMaxTabHeightMm = 50;
constexpr double kOuterMm = 18;
constexpr double kBlurMm = 0.25;  // merges halftone dots, keeps the light line between tab and edge strip
constexpr double kLevelTol = 30;  // grey levels: a tab column's dark level must match the tab's

bool in(const QString& s, std::initializer_list<const char*> set) {
  return std::any_of(set.begin(), set.end(), [&](const char* v) { return s == QLatin1String(v); });
}

// Python's int() of a non-negative float: truncation.
inline int trunc(double v) {
  return static_cast<int>(v);
}

inline int quarter(int v) {
  return v >= 0 ? v / 4 : -((-v + 3) / 4);
}

// Morphological closing of a 0/1 mask with a vertical line of kh pixels (anchor kh / 2), as
// cv::morphologyEx(MORPH_CLOSE) with the default border (pixels outside never count).
std::vector<uint8_t> closeVertical(const std::vector<uint8_t>& m, int w, int h, int kh) {
  if (kh <= 1) {
    return m;
  }
  const int anchor = kh / 2;
  auto pass = [&](const std::vector<uint8_t>& src, bool dilate) {
    std::vector<uint8_t> dst(src.size());
    for (int x = 0; x < w; ++x) {
      for (int y = 0; y < h; ++y) {
        const int a = std::max(0, y - anchor);
        const int b = std::min(h - 1, y - anchor + kh - 1);
        uint8_t v = dilate ? 0 : 1;
        for (int yy = a; yy <= b; ++yy) {
          const uint8_t s = src[static_cast<size_t>(yy) * w + x];
          if (dilate ? s != 0 : s == 0) {
            v = dilate ? 1 : 0;
            break;
          }
        }
        dst[static_cast<size_t>(y) * w + x] = v;
      }
    }
    return dst;
  };
  return pass(pass(m, true), false);
}
}  // namespace

std::vector<QString> defaultLabels() {
  return {QStringLiteral("page_number"), QStringLiteral("tab"), QStringLiteral("header"),
          QStringLiteral("marginalia"), QStringLiteral("text")};
}

double iou(const Box& a, const Box& b) {
  const double iw = std::max(0.0, std::min(a.x1, b.x1) - std::max(a.x0, b.x0));
  const double ih = std::max(0.0, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
  const double inter = iw * ih;
  const double u = a.width() * a.height() + b.width() * b.height() - inter;
  return u > 0 ? inter / u : 0.0;
}

std::vector<Detection> decodeDetections(const float* logits,
                                        const float* boxes,
                                        const int numQueries,
                                        const int numClasses,
                                        const std::vector<QString>& labels,
                                        const int imgWidth,
                                        const int imgHeight,
                                        const double threshold,
                                        const double nmsIou) {
  const int n = numQueries * numClasses;
  std::vector<float> scores(n);
  for (int i = 0; i < n; ++i) {
    scores[i] = 1.f / (1.f + std::exp(-logits[i]));
  }
  std::vector<int> order(n);
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return scores[a] > scores[b]; });
  order.resize(std::min(n, numQueries));

  std::vector<Detection> cand;
  std::vector<int> candLabel;
  for (const int idx : order) {
    if (!(scores[idx] > threshold)) {
      continue;
    }
    const int label = idx % numClasses;
    const int q = idx / numClasses;
    const float* b = boxes + static_cast<size_t>(q) * 4;
    Detection d;
    d.raw = label < static_cast<int>(labels.size()) ? labels[label] : QString::number(label);
    d.score = scores[idx];
    d.box = {static_cast<float>(b[0] - 0.5f * b[2]) * static_cast<float>(imgWidth),
             static_cast<float>(b[1] - 0.5f * b[3]) * static_cast<float>(imgHeight),
             static_cast<float>(b[0] + 0.5f * b[2]) * static_cast<float>(imgWidth),
             static_cast<float>(b[1] + 0.5f * b[3]) * static_cast<float>(imgHeight)};
    cand.push_back(d);
    candLabel.push_back(label);
  }
  // Class-wise greedy NMS over the candidates, already sorted by decreasing score.
  std::vector<Detection> kept;
  std::vector<int> keptLabel;
  for (size_t i = 0; i < cand.size(); ++i) {
    bool suppressed = false;
    for (size_t j = 0; j < kept.size(); ++j) {
      if (keptLabel[j] == candLabel[i] && iou(kept[j].box, cand[i].box) > nmsIou) {
        suppressed = true;
        break;
      }
    }
    if (!suppressed) {
      kept.push_back(cand[i]);
      keptLabel.push_back(candLabel[i]);
    }
  }
  return kept;
}

QString mapClass(const QString& raw, const Box& box, const double frameDpi) {
  const double mmPerPx = 25.4 / frameDpi;
  const bool small = box.width() * mmPerPx <= 25 && box.height() * mmPerPx <= 7;
  if (in(raw, {"number", "page_number"})) {
    return QStringLiteral("page_number");
  }
  if (in(raw, {"chapter_tab", "tab"})) {
    return QStringLiteral("tab");
  }
  if (in(raw, {"header", "header_image", "page_header"})) {
    return small ? QStringLiteral("page_number") : QStringLiteral("header");
  }
  if (in(raw, {"footer", "footer_image", "page_footer"})) {
    return small ? QStringLiteral("page_number") : QStringLiteral("marginalia");
  }
  if (in(raw, {"aside_text", "marginalia"})) {
    return QStringLiteral("marginalia");
  }
  return QString();
}

bool tabRule(const Box& box, const Box& pageRect, const std::optional<Box>& autoBox, const double frameDpi) {
  const double mmPerPx = 25.4 / frameDpi;
  const double w = box.width() * mmPerPx;
  const double h = box.height() * mmPerPx;
  const double gap = std::min(box.x0 - pageRect.x0, pageRect.x1 - box.x1) * mmPerPx;
  const double cx = (box.x0 + box.x1) / 2;
  const double cy = (box.y0 + box.y1) / 2;
  const bool insideAuto
      = autoBox && autoBox->x0 <= cx && cx <= autoBox->x1 && autoBox->y0 <= cy && cy <= autoBox->y1;
  return gap <= 18 && w <= 18 && h <= 50 && !insideAuto;
}

std::pair<int, int> longestRun(const std::vector<bool>& mask) {
  const int n = static_cast<int>(mask.size());
  std::pair<int, int> best(0, n);
  int bestLen = -1;
  int start = -1;
  for (int i = 0; i <= n; ++i) {
    const bool v = i < n && mask[i];
    if (v && start < 0) {
      start = i;
    } else if (!v && start >= 0) {
      if (i - start > bestLen) {
        bestLen = i - start;
        best = {start, i};
      }
      start = -1;
    }
  }
  return bestLen > 0 ? best : std::pair<int, int>(0, n);
}

std::optional<std::pair<int, int>> runThrough(const std::vector<bool>& mask, const double centre) {
  int c = -1;
  double bestDist = 0;
  for (int i = 0; i < static_cast<int>(mask.size()); ++i) {
    if (mask[i] && (c < 0 || std::abs(i - centre) < bestDist)) {
      c = i;
      bestDist = std::abs(i - centre);
    }
  }
  if (c < 0) {
    return std::nullopt;
  }
  int a = c;
  while (a > 0 && mask[a - 1]) {
    --a;
  }
  int b = c;
  while (b + 1 < static_cast<int>(mask.size()) && mask[b + 1]) {
    ++b;
  }
  return std::make_pair(a, b + 1);
}

std::optional<Box> clip(const Box& box, const Box& rect) {
  const Box b{std::max(box.x0, rect.x0), std::max(box.y0, rect.y0), std::min(box.x1, rect.x1),
              std::min(box.y1, rect.y1)};
  if (b.isValid()) {
    return b;
  }
  return std::nullopt;
}

Box paperRect(const Plane8& gray, const double dpi) {
  const double mm = dpi / 25.4;
  const Plane8 b = gaussianBlur(gray, mm);
  const int w = b.width;
  const int h = b.height;
  std::vector<int> colCount(w, 0);
  for (int y = 0; y < h; ++y) {
    const uint8_t* r = b.row(y);
    for (int x = 0; x < w; ++x) {
      colCount[x] += r[x] > 90;
    }
  }
  std::vector<bool> cols(w);
  for (int x = 0; x < w; ++x) {
    cols[x] = 2 * colCount[x] >= h;
  }
  const auto [x0, x1] = longestRun(cols);
  std::vector<bool> rows(h);
  for (int y = 0; y < h; ++y) {
    const uint8_t* r = b.row(y);
    int cnt = 0;
    for (int x = x0; x < x1; ++x) {
      cnt += r[x] > 90;
    }
    rows[y] = 2 * cnt >= (x1 - x0);
  }
  const auto [y0, y1] = longestRun(rows);
  return {static_cast<double>(x0), static_cast<double>(y0), static_cast<double>(x1), static_cast<double>(y1)};
}

std::optional<Box> growTab(const Plane8& gray, const double dpi, const Box& box, const Box& paper) {
  const double mm = dpi / 25.4;
  const double pad = 10 * mm;
  const int rx0 = trunc(std::max(paper.x0, box.x0 - pad));
  const int ry0 = trunc(std::max(paper.y0, box.y0 - pad));
  const int rx1 = std::min(trunc(std::min(paper.x1, box.x1 + pad)), gray.width);
  const int ry1 = std::min(trunc(std::min(paper.y1, box.y1 + pad)), gray.height);
  if (rx1 - rx0 < 5 || ry1 - ry0 < 5) {
    return std::nullopt;
  }
  const int rw = rx1 - rx0;
  const int rh = ry1 - ry0;
  PlaneF region(rw, rh);
  for (int y = 0; y < rh; ++y) {
    const uint8_t* r = gray.row(ry0 + y);
    for (int x = 0; x < rw; ++x) {
      region.at(x, y) = r[rx0 + x];
    }
  }
  const PlaneF reg = gaussianBlur(region, kBlurMm * mm);
  const int bx0 = trunc(std::max(0.0, box.x0 - rx0));
  const int by0 = trunc(std::max(0.0, box.y0 - ry0));
  const int bx1 = trunc(std::min(static_cast<double>(rw), box.x1 - rx0));
  const int by1 = trunc(std::min(static_cast<double>(rh), box.y1 - ry0));
  if (bx1 - bx0 < 3 || by1 - by0 < 3) {
    return std::nullopt;
  }
  std::vector<float> all(reg.data);
  const auto white = static_cast<float>(percentile(all, 90));
  std::vector<float> inBox;
  inBox.reserve(static_cast<size_t>(bx1 - bx0) * (by1 - by0));
  for (int y = by0; y < by1; ++y) {
    for (int x = bx0; x < bx1; ++x) {
      inBox.push_back(reg.at(x, y));
    }
  }
  const auto tab = static_cast<float>(percentile(inBox, 20));
  if (white - tab < 25) {  // nothing darker than the paper in the box
    return std::nullopt;
  }
  const float thr = (white + tab) / 2;
  std::vector<uint8_t> m0(static_cast<size_t>(rw) * rh);
  for (size_t i = 0; i < m0.size(); ++i) {
    m0[i] = reg.data[i] < thr;
  }
  // Close vertically only (the white numbers), so the thin light line between the tab and a
  // neighbouring strip (edge shadow, the next leaves' tabs) is not bridged.
  const std::vector<uint8_t> m = closeVertical(m0, rw, rh, trunc(3 * mm));
  auto mAt = [&](int x, int y) { return m[static_cast<size_t>(y) * rw + x] != 0; };

  // x extent: dark columns in the middle half of the box's rows; y extent: dark rows in the
  // middle of that x run (away from a dark paper-edge strip the tab may touch).
  const int h = by1 - by0;
  const int midA = by0 + quarter(h);
  const int midB = by1 - quarter(h);
  const int midN = std::max(0, midB - midA);
  std::vector<double> level(rw, std::nan(""));
  std::vector<bool> cols(rw, false);
  for (int x = 0; x < rw; ++x) {
    if (midN == 0) {
      continue;
    }
    std::vector<float> col(midN);
    int dark = 0;
    for (int y = midA; y < midB; ++y) {
      col[y - midA] = reg.at(x, y);
      dark += mAt(x, y);
    }
    level[x] = percentile(col, 30);  // per column, white numbers excluded
    cols[x] = static_cast<double>(dark) / midN > 0.6;
  }
  std::vector<float> boxLevels(level.begin() + bx0, level.begin() + bx1);
  const double tabLevel = percentile(boxLevels, 50);
  for (int x = 0; x < rw; ++x) {
    cols[x] = cols[x] && std::abs(level[x] - tabLevel) < kLevelTol;
  }
  const auto xr = runThrough(cols, (bx0 + bx1) / 2.0);
  if (!xr) {
    return std::nullopt;
  }
  // Trim columns at the run's ends that are dark over the whole region height: a page-edge
  // shadow / the next leaf's strip / the scanner edge the tab touches, not the tab.
  std::vector<bool> full(rw);
  for (int x = 0; x < rw; ++x) {
    int dark = 0;
    for (int y = 0; y < rh; ++y) {
      dark += mAt(x, y);
    }
    full[x] = static_cast<double>(dark) / rh > 0.85;
  }
  int a = xr->first;
  int b = xr->second;
  while (b - a > 1 && full[a]) {
    ++a;
  }
  while (b - a > 1 && full[b - 1]) {
    --b;
  }
  const int w = b - a;
  const int ca = std::clamp(a + quarter(w), 0, rw);
  const int cb = std::clamp(b - quarter(w) + 1, 0, rw);
  std::vector<bool> rows(rh, false);
  if (cb > ca) {
    for (int y = 0; y < rh; ++y) {
      int dark = 0;
      for (int x = ca; x < cb; ++x) {
        dark += mAt(x, y);
      }
      rows[y] = static_cast<double>(dark) / (cb - ca) > 0.6;
    }
  }
  const auto yr = runThrough(rows, (by0 + by1) / 2.0);
  if (!yr) {
    return std::nullopt;
  }
  const double wmm = w / mm;
  const double hmm = (yr->second - yr->first) / mm;
  if (!(3 <= wmm && wmm <= kMaxTabWidthMm && 8 <= hmm && hmm <= kMaxTabHeightMm)) {
    return std::nullopt;
  }
  Box out{static_cast<double>(a + rx0), static_cast<double>(yr->first + ry0), static_cast<double>(b + rx0),
          static_cast<double>(yr->second + ry0)};
  out = {std::min(out.x0, box.x0), std::min(out.y0, box.y0), std::max(out.x1, box.x1), std::max(out.y1, box.y1)};
  return clip(out, paper);
}

double outerGapMm(const Box& box, const Box& paper, const double dpi) {
  return std::min(box.x0 - paper.x0, paper.x1 - box.x1) * 25.4 / dpi;
}

bool outerOk(const Box& box, const Box& paper, const double dpi) {
  const double cx = (box.x0 + box.x1) / 2;
  const double cy = (box.y0 + box.y1) / 2;
  const bool onPaper = paper.x0 <= cx && cx <= paper.x1 && paper.y0 <= cy && cy <= paper.y1;
  return onPaper && outerGapMm(box, paper, dpi) <= kOuterMm;
}

std::vector<Element> selectElements(const std::vector<Detection>& dets,
                                    const Plane8& gray,
                                    const double dpi,
                                    const Box& paper) {
  std::vector<Element> out;
  for (const Detection& d : dets) {
    if (d.cls != QLatin1String("tab") && d.clsRule != QLatin1String("tab")) {
      continue;
    }
    if (!outerOk(d.box, paper, dpi)) {
      continue;
    }
    std::optional<Box> g = growTab(gray, dpi, d.box, paper);
    if (!g) {
      g = clip(d.box, paper);
    }
    const Box grown = g ? *g : d.box;
    const std::optional<Box> clipped = clip(grown, paper);
    out.push_back({QStringLiteral("tab"), d.raw, d.score, clipped ? *clipped : grown, d.box});
  }
  for (const Detection& d : dets) {
    if (!in(d.cls, {"page_number", "header", "marginalia"})) {
      continue;
    }
    const std::optional<Box> b = clip(d.box, paper);
    if (!b) {
      continue;
    }
    out.push_back({d.cls, d.raw, d.score, *b, d.box});
  }
  return out;
}
}  // namespace layout
}  // namespace select_content
