// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "SkewFallback.h"

#include <BinaryImage.h>
#include <ConnectivityMap.h>
#include <Dpi.h>
#include <ReduceThreshold.h>
#include <SkewFinder.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace deskew {
using namespace imageproc;

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kStep = 0.05;

double deg(const double rad) {
  return rad * 180.0 / kPi;
}

double rad(const double deg) {
  return deg * kPi / 180.0;
}

// numpy.median
double median(std::vector<double> v) {
  const size_t n = v.size();
  std::nth_element(v.begin(), v.begin() + n / 2, v.end());
  const double hi = v[n / 2];
  if (n % 2 == 1) {
    return hi;
  }
  return 0.5 * (hi + *std::max_element(v.begin(), v.begin() + n / 2));
}

// numpy.percentile, linear interpolation
double percentile(std::vector<double> v, const double q) {
  std::sort(v.begin(), v.end());
  const double pos = q / 100.0 * static_cast<double>(v.size() - 1);
  const auto lo = static_cast<size_t>(std::floor(pos));
  const size_t hi = std::min(lo + 1, v.size() - 1);
  return v[lo] + (v[hi] - v[lo]) * (pos - static_cast<double>(lo));
}

struct Comp {
  int x0 = INT32_MAX, y0 = INT32_MAX, x1 = -1, y1 = -1;  // inclusive bounds
  int64_t area = 0;
  int w() const { return x1 - x0 + 1; }
  int h() const { return y1 - y0 + 1; }
};

struct Point {
  float x;  // in vertical-pixel units (square pixels)
  int y;
};

struct Column {
  double x0, x1;  // [x0, x1)
};

// Text columns: kept ink separated by >= gapPx of empty image columns; columns with < 3 % of the ink
// are dropped.
std::vector<Column> findColumns(const std::vector<int64_t>& prof, const double gapPx, const double xScale) {
  std::vector<Column> cols;
  int64_t total = 0;
  int start = -1, prev = -1;
  std::vector<std::pair<int, int>> runs;
  for (int x = 0; x < static_cast<int>(prof.size()); ++x) {
    if (prof[x] == 0) {
      continue;
    }
    total += prof[x];
    if (start < 0) {
      start = prev = x;
      continue;
    }
    if ((x - prev) * xScale > gapPx) {
      runs.emplace_back(start, prev + 1);
      start = x;
    }
    prev = x;
  }
  if (start >= 0) {
    runs.emplace_back(start, prev + 1);
  }
  for (const auto& [a, b] : runs) {
    const int64_t sum = std::accumulate(prof.begin() + a, prof.begin() + b, int64_t(0));
    if (static_cast<double>(sum) >= 0.03 * static_cast<double>(total)) {
      cols.push_back({a * xScale, b * xScale});
    }
  }
  return cols;
}

// Line labels of one column at the text angle (edge estimate): runs of non-empty sheared rows,
// gaps < 0.5 mm closed. Returns, per point, its line (0 = none) via rows -> label.
std::vector<int> lineLabels(const std::vector<int64_t>& rows, const int64_t nRows, const int k) {
  std::vector<int> prof(static_cast<size_t>(nRows), 0);
  for (const int64_t r : rows) {
    prof[r] = 1;
  }
  // convolve(on, ones(2k+1), "same") > 0
  std::vector<int> on(prof.size(), 0);
  std::vector<int> prefix(prof.size() + 1, 0);
  for (size_t i = 0; i < prof.size(); ++i) {
    prefix[i + 1] = prefix[i] + prof[i];
  }
  for (int64_t i = 0; i < nRows; ++i) {
    const int64_t a = std::max<int64_t>(0, i - k);
    const int64_t b = std::min<int64_t>(nRows, i + k + 1);
    on[i] = (prefix[b] - prefix[a]) > 0 ? 1 : 0;
  }
  std::vector<int> lab(prof.size(), 0);
  int cur = 0;
  int last = 0;
  for (size_t i = 0; i < on.size(); ++i) {
    if (on[i] && !last) {
      ++cur;
    }
    lab[i] = on[i] ? cur : 0;
    last = on[i];
  }
  return lab;
}
}  // namespace

FallbackSkew findSkewFallback(const BinaryImage& input, const Dpi& inputDpi) {
  FallbackSkew res;
  if (input.isNull()) {
    return res;
  }

  // Work at <= ~300 dpi (the resolution tools/skewcheck.py was validated at).
  BinaryImage image(input);
  double dpiX = inputDpi.horizontal() > 0 ? inputDpi.horizontal() : 300;
  double dpiY = inputDpi.vertical() > 0 ? inputDpi.vertical() : 300;
  {
    ReduceThreshold reductor(input);
    bool reduced = false;
    while (dpiX >= 400 && dpiY >= 400 && reductor.image().width() > 1 && reductor.image().height() > 1) {
      reductor.reduce(2);
      dpiX /= 2;
      dpiY /= 2;
      reduced = true;
    }
    if (reduced) {
      image = reductor.image();
    }
  }
  const int W = image.width();
  const int H = image.height();
  const double mm = dpiY / 25.4;      // pixels per mm, vertical units
  const double xScale = dpiY / dpiX;  // x pixels -> vertical-pixel units

  // 1. Connected components and their bounding boxes.
  const ConnectivityMap cmap(image, CONN8);
  const uint32_t maxLabel = cmap.maxLabel();
  std::vector<Comp> comps(maxLabel + 1);
  {
    const uint32_t* line = cmap.data();
    for (int y = 0; y < H; ++y, line += cmap.stride()) {
      for (int x = 0; x < W; ++x) {
        const uint32_t l = line[x];
        if (!l) {
          continue;
        }
        Comp& c = comps[l];
        c.x0 = std::min(c.x0, x);
        c.x1 = std::max(c.x1, x);
        c.y0 = std::min(c.y0, y);
        c.y1 = std::max(c.y1, y);
        ++c.area;
      }
    }
  }

  std::vector<char> keep(maxLabel + 1, 0);
  std::vector<int> ruleIdx(maxLabel + 1, -1);
  std::vector<uint32_t> rules;
  std::vector<const Comp*> containers;
  for (uint32_t l = 1; l <= maxLabel; ++l) {
    const Comp& c = comps[l];
    if (c.area == 0) {
      continue;
    }
    const double w = c.w() * xScale;
    const double h = c.h();
    const bool border = c.x0 <= 1 || c.y0 <= 1 || c.x1 + 1 >= W - 1 || c.y1 + 1 >= H - 1;
    const double fill = static_cast<double>(c.area) / std::max(1, c.w() * c.h());
    if (h >= 0.6 * mm && h <= 7 * mm && w >= 0.3 * mm && w <= 12 * mm && !border
        && !(fill > 0.85 && w > 2 * mm && h > 2 * mm)) {
      keep[l] = 1;
    }
    // Pictures and framed boxes: text-sized strokes inside them are left out. (On the uncropped
    // stage-3 image the scanner background and page-block edges touch the border; they are no
    // frames.)
    if (w >= 20 * mm && h >= 20 * mm && !border) {
      containers.push_back(&c);
    }
    // Column rules: thin and long.
    if (h >= 40 * mm && static_cast<double>(c.area) / std::max(1.0, h) * xScale <= 1.0 * mm && w <= 3 * mm + 0.1 * h
        && !border) {
      ruleIdx[l] = static_cast<int>(rules.size());
      rules.push_back(l);
    }
  }
  for (uint32_t l = 1; l <= maxLabel; ++l) {
    if (!keep[l]) {
      continue;
    }
    const Comp& c = comps[l];
    const double cx = 0.5 * (c.x0 + c.x1);
    const double cy = 0.5 * (c.y0 + c.y1);
    for (const Comp* f : containers) {
      if (f != &c && cx >= f->x0 && cx <= f->x1 && cy >= f->y0 && cy <= f->y1) {
        keep[l] = 0;
        break;
      }
    }
  }

  // Kept ink, column rule pixel sums.
  std::vector<Point> pts;
  std::vector<int64_t> xprof(static_cast<size_t>(W), 0);
  struct LineFit {
    double n = 0, sy = 0, sx = 0, syy = 0, sxy = 0;
  };
  std::vector<LineFit> ruleFit(rules.size());
  {
    const uint32_t* line = cmap.data();
    for (int y = 0; y < H; ++y, line += cmap.stride()) {
      for (int x = 0; x < W; ++x) {
        const uint32_t l = line[x];
        if (!l) {
          continue;
        }
        if (keep[l]) {
          pts.push_back({static_cast<float>(x * xScale), y});
          ++xprof[x];
        } else if (ruleIdx[l] >= 0) {
          LineFit& f = ruleFit[ruleIdx[l]];
          const double xs = x * xScale;
          f.n += 1;
          f.sy += y;
          f.sx += xs;
          f.syy += double(y) * y;
          f.sxy += double(y) * xs;
        }
      }
    }
  }
  int nKept = 0;
  for (uint32_t l = 1; l <= maxLabel; ++l) {
    nKept += keep[l] ? 1 : 0;
  }
  res.components = nKept;
  if (nKept < FallbackSkew::MIN_TEXT_COMPONENTS) {
    return res;
  }

  // 2. Columns.
  const std::vector<Column> cols = findColumns(xprof, 4.0 * mm, xScale);
  res.columns = static_cast<int>(cols.size());
  if (cols.empty()) {
    return res;
  }
  // Points per column, x relative to the column centre.
  std::vector<std::vector<Point>> colPts(cols.size());
  for (const Point& p : pts) {
    for (size_t i = 0; i < cols.size(); ++i) {
      if (p.x >= cols[i].x0 && p.x < cols[i].x1) {
        colPts[i].push_back({static_cast<float>(p.x - 0.5 * (cols[i].x0 + cols[i].x1)), p.y});
        break;
      }
    }
  }

  // 3. Projection profile: sum over columns of sum(row count^2).
  const int nAngles = static_cast<int>(std::lround(2 * FallbackSkew::MAX_ANGLE / kStep)) + 1;
  std::vector<double> scores(nAngles, 0.0);
  {
    std::vector<int32_t> hist(static_cast<size_t>(3) * H + 2, 0);
    for (int i = 0; i < nAngles; ++i) {
      const double t = std::tan(rad(-FallbackSkew::MAX_ANGLE + i * kStep));
      double s = 0.0;
      for (const auto& cp : colPts) {
        for (const Point& p : cp) {
          const auto r = static_cast<int64_t>(std::nearbyint(p.y - p.x * t)) + H;
          if (r >= 0 && r < static_cast<int64_t>(hist.size())) {
            ++hist[r];
          }
        }
        for (const Point& p : cp) {
          const auto r = static_cast<int64_t>(std::nearbyint(p.y - p.x * t)) + H;
          if (r >= 0 && r < static_cast<int64_t>(hist.size()) && hist[r]) {
            s += double(hist[r]) * hist[r];
            hist[r] = 0;
          }
        }
      }
      scores[i] = s;
    }
  }
  const int best = static_cast<int>(std::max_element(scores.begin(), scores.end()) - scores.begin());
  double a = -FallbackSkew::MAX_ANGLE + best * kStep;
  if (best > 0 && best < nAngles - 1) {
    const double y0 = scores[best - 1], y1 = scores[best], y2 = scores[best + 1];
    const double d = y0 - 2 * y1 + y2;
    if (d < 0) {
      a += 0.5 * (y0 - y2) / d * kStep;
    }
  }
  const double med = median(scores);
  res.angle = a;
  res.contrast = (scores[best] > 0 && med > 0) ? scores[best] / med : 1.0;
  const double t = std::tan(rad(a));

  // 4a. Column rule: the longest one, x fitted on y.
  {
    double bestSpan = -1;
    for (size_t i = 0; i < rules.size(); ++i) {
      const Comp& c = comps[rules[i]];
      const double span = c.y1 - c.y0;
      if (span > bestSpan) {
        const LineFit& f = ruleFit[i];
        const double den = f.n * f.syy - f.sy * f.sy;
        if (f.n < 2 || std::abs(den) < 1e-9) {
          continue;
        }
        const double m = (f.n * f.sxy - f.sy * f.sx) / den;
        bestSpan = span;
        res.rule = -deg(std::atan(m));
        res.ruleMm = span / mm;
      }
    }
  }

  // 4b. Left text edge: per column, the left end of each text line at the text angle; points within
  // 1.5 mm of the left-aligned lines (20th percentile); Theil-Sen over point pairs >= 20 mm apart.
  {
    std::vector<double> slopes;
    int npts = 0;
    double span = 0.0;
    const int k = std::max(1, static_cast<int>(0.5 * mm));
    for (size_t ci = 0; ci < cols.size(); ++ci) {
      const auto& cp = colPts[ci];
      if (cp.empty()) {
        continue;
      }
      std::vector<int64_t> rows(cp.size());
      int64_t rmin = INT64_MAX, rmax = INT64_MIN;
      for (size_t j = 0; j < cp.size(); ++j) {
        rows[j] = static_cast<int64_t>(std::nearbyint(cp[j].y - cp[j].x * t));
        rmin = std::min(rmin, rows[j]);
        rmax = std::max(rmax, rows[j]);
      }
      for (int64_t& r : rows) {
        r -= rmin;
      }
      const std::vector<int> lab = lineLabels(rows, rmax - rmin + 1, k);
      // leftmost point of each line (smallest x, then smallest y)
      std::vector<std::pair<double, double>> first;  // line -> (y, x)
      std::vector<char> seen;
      for (size_t j = 0; j < cp.size(); ++j) {
        const int line = lab[rows[j]];
        if (line <= 0) {
          continue;
        }
        if (static_cast<size_t>(line) >= seen.size()) {
          seen.resize(line + 1, 0);
          first.resize(line + 1);
        }
        const double x = cp[j].x;
        const double y = cp[j].y;
        if (!seen[line] || x < first[line].second || (x == first[line].second && y < first[line].first)) {
          first[line] = {y, x};
          seen[line] = 1;
        }
      }
      std::vector<std::pair<double, double>> P;
      for (size_t l = 0; l < seen.size(); ++l) {
        if (seen[l]) {
          P.push_back(first[l]);
        }
      }
      if (P.size() < 6) {
        continue;
      }
      std::vector<double> xsv;
      for (const auto& p : P) {
        xsv.push_back(p.second);
      }
      const double left = percentile(xsv, 20);
      std::vector<std::pair<double, double>> Q;
      for (const auto& p : P) {
        if (std::abs(p.second - left) <= 1.5 * mm) {
          Q.push_back(p);
        }
      }
      if (Q.size() < 6) {
        continue;
      }
      double ymin = Q[0].first, ymax = Q[0].first;
      for (size_t i = 0; i < Q.size(); ++i) {
        ymin = std::min(ymin, Q[i].first);
        ymax = std::max(ymax, Q[i].first);
        for (size_t j = 0; j < Q.size(); ++j) {
          const double dy = Q[i].first - Q[j].first;
          if (dy >= 20 * mm) {
            slopes.push_back((Q[i].second - Q[j].second) / dy);
          }
        }
      }
      npts += static_cast<int>(Q.size());
      span = std::max(span, (ymax - ymin) / mm);
    }
    if (!slopes.empty() && npts >= 6) {
      res.edge = -deg(std::atan(median(slopes)));
      res.edgePoints = npts;
      res.edgeMm = span;
    }
  }

  // 4c. Baseline: per text line the slope of the bottoms of its characters (components >= 1.5 mm
  // high: no furigana), Theil-Sen per line, median over lines weighted by the line's length.
  {
    struct Bottom {
      double bot, cx;
    };
    std::vector<Bottom> bottoms;
    for (uint32_t l = 1; l <= maxLabel; ++l) {
      if (!keep[l]) {
        continue;
      }
      const Comp& c = comps[l];
      if (c.y1 - c.y0 >= 1.5 * mm) {
        bottoms.push_back({double(c.y1), 0.5 * (c.x0 + c.x1) * xScale});
      }
    }
    if (bottoms.size() >= 10) {
      std::vector<double> slopes, weights;
      for (const Column& col : cols) {
        const double centre = 0.5 * (col.x0 + col.x1);
        std::vector<std::pair<double, Bottom>> sel;  // (r, bottom)
        for (const Bottom& b : bottoms) {
          if (b.cx >= col.x0 && b.cx < col.x1) {
            sel.push_back({b.bot - (b.cx - centre) * t, b});
          }
        }
        if (sel.size() < 4) {
          continue;
        }
        std::sort(sel.begin(), sel.end(), [](const auto& p, const auto& q) { return p.first < q.first; });
        size_t s0 = 0;
        for (size_t i = 1; i <= sel.size(); ++i) {
          if (i < sel.size() && sel[i].first - sel[i - 1].first <= 2 * mm) {
            continue;
          }
          // segment [s0, i)
          const size_t n = i - s0;
          if (n >= 4) {
            double xmin = sel[s0].second.cx, xmax = xmin;
            for (size_t j = s0; j < i; ++j) {
              xmin = std::min(xmin, sel[j].second.cx);
              xmax = std::max(xmax, sel[j].second.cx);
            }
            if (xmax - xmin >= 20 * mm) {
              std::vector<double> sl;
              for (size_t p = s0; p < i; ++p) {
                for (size_t q = s0; q < i; ++q) {
                  const double dx = sel[p].second.cx - sel[q].second.cx;
                  if (dx > 5 * mm) {
                    sl.push_back((sel[p].second.bot - sel[q].second.bot) / dx);
                  }
                }
              }
              if (sl.size() >= 3) {
                slopes.push_back(median(sl));
                weights.push_back(xmax - xmin);
              }
            }
          }
          s0 = i;
        }
      }
      if (slopes.size() >= 2) {
        std::vector<size_t> order(slopes.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::sort(order.begin(), order.end(), [&](size_t p, size_t q) { return slopes[p] < slopes[q]; });
        std::vector<double> cw(order.size());
        double acc = 0;
        for (size_t i = 0; i < order.size(); ++i) {
          acc += weights[order[i]];
          cw[i] = acc;
        }
        const size_t at = std::lower_bound(cw.begin(), cw.end(), acc / 2) - cw.begin();
        res.baseline = deg(std::atan(slopes[order[std::min(at, order.size() - 1)]]));
      }
    }
  }

  // 5. Sure: enough text and a second estimate agrees.
  bool agree;
  if (res.baseline) {
    agree = std::abs(*res.baseline - a) <= FallbackSkew::AGREE_BASELINE;
  } else {
    agree = (res.rule && std::abs(*res.rule - a) <= FallbackSkew::AGREE_VERTICAL)
            || (res.edge && std::abs(*res.edge - a) <= FallbackSkew::AGREE_VERTICAL);
  }
  res.sure = nKept >= FallbackSkew::MIN_SURE_COMPONENTS && res.contrast >= 1.0 && agree
             && std::abs(a) <= FallbackSkew::MAX_SURE_ANGLE;
  return res;
}  // findSkewFallback

DeskewDecision decideDeskew(const Skew& primary,
                            const Skew& unrounded,
                            const bool fallbackEnabled,
                            const std::function<FallbackSkew()>& fallback) {
  DeskewDecision d;
  const bool confident = primary.confidence() >= Skew::GOOD_CONFIDENCE;
  const bool roundedZero = primary.angle() == 0.0;
  if (confident && (!fallbackEnabled || !fallback)) {
    d.skewAngle = primary.angle();
    d.source = DeskewSource::PRIMARY;
    return d;
  }
  if (confident && !roundedZero) {
    // SkewFinder sure of a non-zero angle: the fallback overrules it only if it is sure too and
    // clearly elsewhere.
    d.skewAngle = primary.angle();
    d.source = DeskewSource::PRIMARY;
    const FallbackSkew fb = fallback();
    const double disagreement = std::abs(fb.angle - primary.angle());
    if (fb.sure && disagreement > MAX_CONFIDENT_DISAGREEMENT) {
      d.skewAngle = fb.angle;
      d.source = DeskewSource::FALLBACK;
    } else if (!fb.sure && fb.components >= FallbackSkew::MIN_SURE_COMPONENTS
               && disagreement > MIN_UNCERTAIN_CONFIDENT_DISAGREEMENT) {
      d.uncertain = true;
    }
    return d;
  }
  if (!confident && (!fallbackEnabled || !fallback)) {
    d.skewAngle = 0.0;
    d.source = DeskewSource::DISABLED;
    return d;
  }

  if (confident && unrounded.confidence() >= Skew::GOOD_CONFIDENCE) {
    // SkewFinder sure of (nearly) 0: check it.
    d.skewAngle = 0.0;
    d.source = DeskewSource::PRIMARY;
    const FallbackSkew fb = fallback();
    if (!fb.hasText()) {
      return d;
    }
    const double disagreement = std::abs(fb.angle - unrounded.angle());
    if (fb.sure) {
      if (disagreement > MAX_ROUNDED_ZERO_DISAGREEMENT) {
        d.skewAngle = fb.angle;
        d.source = DeskewSource::FALLBACK;
      }
    } else if (fb.components >= FallbackSkew::MIN_SURE_COMPONENTS && std::abs(fb.angle) > MIN_UNCERTAIN_ZERO_ANGLE
               && disagreement > MAX_ROUNDED_ZERO_DISAGREEMENT) {
      d.uncertain = true;
    }
    return d;
  }

  // SkewFinder not confident (upstream: a silent 0).
  const FallbackSkew fb = fallback();
  if (!fb.hasText()) {
    d.skewAngle = 0.0;
    d.source = DeskewSource::NO_TEXT;
    return d;
  }
  if (fb.sure) {
    d.skewAngle = fb.angle;
    d.source = DeskewSource::FALLBACK;
    return d;
  }
  if (fb.components >= FallbackSkew::MIN_SURE_COMPONENTS && std::abs(fb.angle) <= FallbackSkew::MAX_SURE_ANGLE) {
    d.skewAngle = fb.angle;
  } else {
    d.skewAngle = unrounded.angle();
  }
  d.source = DeskewSource::BEST_GUESS;
  d.uncertain = true;
  return d;
}

}  // namespace deskew
