// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.
//
// Deskew fallback (scantailor-cli issue #15): the estimator on synthetic pages, the decision logic,
// and the diagnostics in the project XML.

#include <BinaryImage.h>
#include <Dpi.h>
#include <SkewFinder.h>
#include <filters/deskew/Params.h>
#include <filters/deskew/SkewFallback.h>

#include <QDomDocument>
#include <boost/test/unit_test.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Tests {
using namespace deskew;
using namespace imageproc;

namespace {
constexpr int kDpi = 300;
constexpr double kMm = kDpi / 25.4;
constexpr double kPi = 3.14159265358979323846;

// Deterministic pseudo-random numbers (no global rand() state).
class Lcg {
 public:
  explicit Lcg(uint32_t seed) : m_state(seed) {}

  double next() {  // [0, 1)
    m_state = m_state * 1664525u + 1013904223u;
    return (m_state >> 8) / double(1u << 24);
  }

 private:
  uint32_t m_state;
};

struct TextLine {
  double baseline;                             // mm
  std::vector<std::pair<double, double>> chars;  // [x0, x1) mm
};

struct PageSpec {
  double tiltDeg = 0.0;      // SkewFinder convention: + = lines fall to the right
  bool borderStripes = false;  // horizontal stripes along the top and bottom edge, not tilted
  double lineSpacingMm = 6.0;
  double charHeightMm = 2.6;
};

/**
 * A 150 x 210 mm page at 300 dpi with two text columns (15-70 mm and 80-135 mm) whose lines do not
 * line up (the right column is offset by half a line), characters as solid boxes standing on their
 * baseline, the whole text turned by tiltDeg about the page centre. borderStripes adds ruled
 * stripes along the top and bottom image edge (like scanner background and page-block edges on an
 * open-book scan): straight at 0 deg, touching the image border.
 */
BinaryImage makePage(const PageSpec& spec) {
  const int w = static_cast<int>(150 * kMm);
  const int h = static_cast<int>(210 * kMm);
  Lcg rng(15);
  std::vector<TextLine> lines;
  for (int col = 0; col < 2; ++col) {
    const double x0 = col == 0 ? 15.0 : 80.0;
    const double x1 = x0 + 55.0;
    const double offset = col == 0 ? 0.0 : spec.lineSpacingMm / 2;
    for (double base = 25.0 + offset; base < 190.0; base += spec.lineSpacingMm) {
      TextLine line;
      line.baseline = base;
      const double end = x1 - (rng.next() < 0.2 ? 20.0 * rng.next() : 0.0);
      for (double x = x0; x < end;) {
        const double cw = 1.2 + 1.4 * rng.next();
        line.chars.emplace_back(x, std::min(end, x + cw));
        x += cw + (rng.next() < 0.15 ? 2.0 : 0.4 + 0.4 * rng.next());
      }
      lines.push_back(line);
    }
  }

  BinaryImage img(w, h, WHITE);
  const double cx = w / 2.0, cy = h / 2.0;
  const double c = std::cos(spec.tiltDeg * kPi / 180), s = std::sin(spec.tiltDeg * kPi / 180);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      // image -> page coordinates: turn back by tiltDeg about the centre
      const double dx = x - cx, dy = y - cy;
      const double u = (c * dx + s * dy + cx) / kMm;
      const double v = (-s * dx + c * dy + cy) / kMm;
      bool ink = false;
      for (const TextLine& line : lines) {
        if (v > line.baseline || v < line.baseline - spec.charHeightMm) {
          continue;
        }
        for (const auto& ch : line.chars) {
          if (u >= ch.first && u < ch.second) {
            ink = true;
            break;
          }
        }
        if (ink) {
          break;
        }
      }
      if (spec.borderStripes) {
        const double ym = y / kMm;
        if ((ym < 12.0 || ym > 198.0) && static_cast<int>(ym) % 2 == 0) {
          ink = true;
        }
      }
      if (ink) {
        img.setPixel(x, y, BLACK);
      }
    }
  }
  return img;
}

FallbackSkew fakeFallback(double angle, bool sure, int components = 500) {
  FallbackSkew fb;
  fb.angle = angle;
  fb.sure = sure;
  fb.components = components;
  return fb;
}

struct CountingFallback {
  FallbackSkew result;
  int* calls;

  FallbackSkew operator()() const {
    ++*calls;
    return result;
  }
};
}  // namespace

BOOST_AUTO_TEST_SUITE(SkewFallbackTestSuite)

// The case of issue #15 as found on Grammar 2/3: a tilted two-column page on which SkewFinder
// locks onto straight page-edge structures and is sure of 0 (its unrounded confidence is high); the
// fallback measures the text and replaces the 0.
BOOST_AUTO_TEST_CASE(tilted_two_column_page_skewfinder_sure_of_zero) {
  PageSpec spec;
  spec.tiltDeg = 1.7;
  spec.borderStripes = true;
  const BinaryImage page = makePage(spec);

  SkewFinder finder;
  Skew unrounded;
  const Skew primary = finder.findSkew(page, &unrounded);
  const FallbackSkew fb = findSkewFallback(page, Dpi(kDpi, kDpi));
  BOOST_TEST_MESSAGE("SkewFinder " << primary.angle() << " conf " << primary.confidence() << " (unrounded "
                                   << unrounded.angle() << " conf " << unrounded.confidence() << "); fallback "
                                   << fb.angle << " sure " << fb.sure << " components " << fb.components
                                   << " baseline " << fb.baseline.value_or(NAN));
  BOOST_CHECK_EQUAL(primary.angle(), 0.0);
  BOOST_CHECK(unrounded.confidence() >= Skew::GOOD_CONFIDENCE);

  BOOST_CHECK_EQUAL(fb.columns, 2);
  BOOST_CHECK(fb.sure);
  BOOST_CHECK_SMALL(fb.angle - spec.tiltDeg, 0.05);
  BOOST_REQUIRE(fb.baseline.has_value());
  BOOST_CHECK_SMALL(*fb.baseline - spec.tiltDeg, 0.1);

  const DeskewDecision d = decideDeskew(primary, unrounded, true, [&] { return fb; });
  BOOST_CHECK(d.source == DeskewSource::FALLBACK);
  BOOST_CHECK_SMALL(d.skewAngle - spec.tiltDeg, 0.05);
  BOOST_CHECK(!d.uncertain);
  // Off: upstream's 0.
  BOOST_CHECK_EQUAL(decideDeskew(primary, unrounded, false, [&] { return fb; }).skewAngle, 0.0);
}

// The low-confidence path (Task.cpp: confidence < Skew::GOOD_CONFIDENCE, upstream sets 0): the
// same kind of page measured by the real estimator, with the result SkewFinder gives on pages it
// cannot read (low confidence, wrong angle). On G1-G3 only blank pages reached this path, and
// SkewFinder stays confident on every synthetic text page, so its result is given here.
BOOST_AUTO_TEST_CASE(tilted_two_column_page_low_confidence_path) {
  PageSpec spec;
  spec.tiltDeg = -1.4;
  const BinaryImage page = makePage(spec);
  const Skew lowConfidence(0.6, 1.1);

  int calls = 0;
  const auto measure = [&] {
    ++calls;
    return findSkewFallback(page, Dpi(kDpi, kDpi));
  };
  const DeskewDecision d = decideDeskew(lowConfidence, lowConfidence, true, measure);
  BOOST_CHECK_EQUAL(calls, 1);
  BOOST_CHECK(d.source == DeskewSource::FALLBACK);
  BOOST_CHECK_SMALL(d.skewAngle - spec.tiltDeg, 0.05);
  BOOST_CHECK(!d.uncertain);

  const DeskewDecision off = decideDeskew(lowConfidence, lowConfidence, false, measure);
  BOOST_CHECK(off.source == DeskewSource::DISABLED);
  BOOST_CHECK_EQUAL(off.skewAngle, 0.0);
  BOOST_CHECK_EQUAL(calls, 1);
}

// SkewFinder confident but off: on the same page without the stripes it reads 1.5 deg (its fine
// search resolves 1/16 deg at best); the fallback (sure, 0.2 deg away) overrules it.
BOOST_AUTO_TEST_CASE(tilted_two_column_page_confident_but_off) {
  PageSpec spec;
  spec.tiltDeg = 1.7;
  const BinaryImage page = makePage(spec);
  SkewFinder finder;
  Skew unrounded;
  const Skew primary = finder.findSkew(page, &unrounded);
  BOOST_TEST_MESSAGE("SkewFinder " << primary.angle() << " conf " << primary.confidence());
  BOOST_REQUIRE(primary.confidence() >= Skew::GOOD_CONFIDENCE);
  BOOST_REQUIRE(std::abs(primary.angle() - spec.tiltDeg) > MAX_CONFIDENT_DISAGREEMENT);
  const DeskewDecision d
      = decideDeskew(primary, unrounded, true, [&] { return findSkewFallback(page, Dpi(kDpi, kDpi)); });
  BOOST_CHECK(d.source == DeskewSource::FALLBACK);
  BOOST_CHECK_SMALL(d.skewAngle - spec.tiltDeg, 0.05);
}

BOOST_AUTO_TEST_CASE(straight_page_stays_straight) {
  PageSpec spec;
  spec.tiltDeg = 0.0;
  spec.borderStripes = true;
  const BinaryImage page = makePage(spec);
  SkewFinder finder;
  Skew unrounded;
  const Skew primary = finder.findSkew(page, &unrounded);
  const FallbackSkew fb = findSkewFallback(page, Dpi(kDpi, kDpi));
  BOOST_CHECK(fb.sure);
  BOOST_CHECK_SMALL(fb.angle, 0.05);
  const DeskewDecision d = decideDeskew(primary, unrounded, true, [&] { return fb; });
  BOOST_CHECK_EQUAL(d.skewAngle, 0.0);
  BOOST_CHECK(d.source == DeskewSource::PRIMARY);
  BOOST_CHECK(!d.uncertain);
}

BOOST_AUTO_TEST_CASE(blank_page_has_no_text) {
  const BinaryImage page(static_cast<int>(150 * kMm), static_cast<int>(210 * kMm), WHITE);
  const FallbackSkew fb = findSkewFallback(page, Dpi(kDpi, kDpi));
  BOOST_CHECK(!fb.hasText());
  BOOST_CHECK(!fb.sure);
}

BOOST_AUTO_TEST_CASE(reduces_600_dpi_input) {
  // The estimator works at <= 300 dpi; a 600 dpi page gives the same angle.
  PageSpec spec;
  spec.tiltDeg = -0.8;
  const BinaryImage page300 = makePage(spec);
  BinaryImage page600(page300.width() * 2, page300.height() * 2, WHITE);
  for (int y = 0; y < page600.height(); ++y) {
    for (int x = 0; x < page600.width(); ++x) {
      if (page300.getPixel(x / 2, y / 2) == BLACK) {
        page600.setPixel(x, y, BLACK);
      }
    }
  }
  const FallbackSkew fb = findSkewFallback(page600, Dpi(600, 600));
  BOOST_CHECK(fb.sure);
  BOOST_CHECK_SMALL(fb.angle - spec.tiltDeg, 0.05);
}

// Decision logic (fake estimates).

BOOST_AUTO_TEST_CASE(decide_confident_nonzero_checked_by_fallback) {
  int calls = 0;
  const Skew s(1.25, 5.0);
  // sure and more than 0.15 deg away: the fallback's angle
  DeskewDecision d = decideDeskew(s, s, true, CountingFallback{fakeFallback(1.5, true), &calls});
  BOOST_CHECK_EQUAL(d.skewAngle, 1.5);
  BOOST_CHECK(d.source == DeskewSource::FALLBACK);
  BOOST_CHECK(!d.uncertain);
  BOOST_CHECK_EQUAL(calls, 1);
  // sure and within 0.15 deg: SkewFinder's, as upstream
  d = decideDeskew(s, s, true, [] { return fakeFallback(1.12, true); });
  BOOST_CHECK_EQUAL(d.skewAngle, 1.25);
  BOOST_CHECK(d.source == DeskewSource::PRIMARY);
  // unsure and more than 0.3 deg away: SkewFinder's, page uncertain
  d = decideDeskew(s, s, true, [] { return fakeFallback(0.77, false); });
  BOOST_CHECK_EQUAL(d.skewAngle, 1.25);
  BOOST_CHECK(d.source == DeskewSource::PRIMARY);
  BOOST_CHECK(d.uncertain);
  // unsure within 0.3 deg, or with too little text: not uncertain
  d = decideDeskew(s, s, true, [] { return fakeFallback(1.0, false); });
  BOOST_CHECK(!d.uncertain);
  d = decideDeskew(s, s, true, [] { return fakeFallback(0.2, false, 25); });
  BOOST_CHECK(!d.uncertain);
}

BOOST_AUTO_TEST_CASE(decide_confident_zero_checked_by_fallback) {
  const Skew rounded(0.0, Skew::GOOD_CONFIDENCE);
  const Skew unrounded(0.06, 17.0);
  int calls = 0;
  // sure and disagreeing: the fallback's angle
  DeskewDecision d = decideDeskew(rounded, unrounded, true, CountingFallback{fakeFallback(-1.4, true), &calls});
  BOOST_CHECK_EQUAL(d.skewAngle, -1.4);
  BOOST_CHECK(d.source == DeskewSource::FALLBACK);
  BOOST_CHECK(!d.uncertain);
  // sure and within 0.1 of SkewFinder's unrounded angle: upstream's 0
  d = decideDeskew(rounded, unrounded, true, [] { return fakeFallback(0.13, true); });
  BOOST_CHECK_EQUAL(d.skewAngle, 0.0);
  BOOST_CHECK(d.source == DeskewSource::PRIMARY);
  BOOST_CHECK(!d.uncertain);
  // unsure and tilted: 0 kept, uncertain
  d = decideDeskew(rounded, unrounded, true, [] { return fakeFallback(0.9, false); });
  BOOST_CHECK_EQUAL(d.skewAngle, 0.0);
  BOOST_CHECK(d.uncertain);
  // unsure, within the 0.15 deg threshold: not uncertain
  d = decideDeskew(rounded, Skew(0.0, 17.0), true, [] { return fakeFallback(0.12, false); });
  BOOST_CHECK(!d.uncertain);
  // unsure with too little text: not uncertain
  d = decideDeskew(rounded, unrounded, true, [] { return fakeFallback(0.9, false, 20); });
  BOOST_CHECK(!d.uncertain);
  // no text: 0
  d = decideDeskew(rounded, unrounded, true, [] { return fakeFallback(0.0, false, 3); });
  BOOST_CHECK_EQUAL(d.skewAngle, 0.0);
  BOOST_CHECK(!d.uncertain);
  BOOST_CHECK_EQUAL(calls, 1);
}

BOOST_AUTO_TEST_CASE(decide_low_confidence) {
  const Skew low(2.7, 0.8);
  DeskewDecision d = decideDeskew(low, low, true, [] { return fakeFallback(1.6, true); });
  BOOST_CHECK_EQUAL(d.skewAngle, 1.6);
  BOOST_CHECK(d.source == DeskewSource::FALLBACK);
  BOOST_CHECK(!d.uncertain);
  // nothing confident: the fallback's profile angle as best guess, uncertain
  d = decideDeskew(low, low, true, [] { return fakeFallback(1.1, false); });
  BOOST_CHECK_EQUAL(d.skewAngle, 1.1);
  BOOST_CHECK(d.source == DeskewSource::BEST_GUESS);
  BOOST_CHECK(d.uncertain);
  // ... SkewFinder's own angle if the fallback had too little text or hit its range limit
  d = decideDeskew(low, low, true, [] { return fakeFallback(1.1, false, 25); });
  BOOST_CHECK_EQUAL(d.skewAngle, 2.7);
  BOOST_CHECK(d.uncertain);
  d = decideDeskew(low, low, true, [] { return fakeFallback(4.95, false); });
  BOOST_CHECK_EQUAL(d.skewAngle, 2.7);
  // no text (blank page): 0, not uncertain
  d = decideDeskew(low, low, true, [] { return fakeFallback(0.0, false, 4); });
  BOOST_CHECK_EQUAL(d.skewAngle, 0.0);
  BOOST_CHECK(d.source == DeskewSource::NO_TEXT);
  BOOST_CHECK(!d.uncertain);
  // a rounded 0 whose real confidence is low takes the same path
  d = decideDeskew(Skew(0.0, Skew::GOOD_CONFIDENCE), Skew(0.05, 0.5), true, [] { return fakeFallback(1.6, true); });
  BOOST_CHECK_EQUAL(d.skewAngle, 1.6);
}

BOOST_AUTO_TEST_CASE(decide_fallback_off_is_upstream) {
  int calls = 0;
  const CountingFallback fb{fakeFallback(1.6, true), &calls};
  DeskewDecision d = decideDeskew(Skew(2.7, 0.8), Skew(2.7, 0.8), false, fb);
  BOOST_CHECK_EQUAL(d.skewAngle, 0.0);
  BOOST_CHECK(d.source == DeskewSource::DISABLED);
  BOOST_CHECK(!d.uncertain);
  d = decideDeskew(Skew(0.0, Skew::GOOD_CONFIDENCE), Skew(0.0, 30.0), false, fb);
  BOOST_CHECK_EQUAL(d.skewAngle, 0.0);
  BOOST_CHECK(d.source == DeskewSource::PRIMARY);
  d = decideDeskew(Skew(1.2, 4.0), Skew(1.2, 4.0), false, fb);
  BOOST_CHECK_EQUAL(d.skewAngle, 1.2);
  BOOST_CHECK_EQUAL(calls, 0);
}

BOOST_AUTO_TEST_CASE(oblique_plausibility) {
  BOOST_CHECK(!isObliqueImplausible(1.0));
  BOOST_CHECK(!isObliqueImplausible(-1.0));
  BOOST_CHECK(isObliqueImplausible(1.01));
  BOOST_CHECK(isObliqueImplausible(-3.66));
}

// Diagnostics in the project XML.

BOOST_AUTO_TEST_CASE(diagnostics_xml_roundtrip) {
  Params p(0.62, 2.0, Dependencies(), MODE_AUTO, MODE_AUTO);
  AutoDiagnostics diag;
  diag.confidence = 17.5734;
  diag.source = DeskewSource::BEST_GUESS;
  diag.angleUncertain = true;
  diag.obliqueUncertain = true;
  diag.rejectedOblique = -3.6612;
  p.setDiagnostics(diag);

  QDomDocument doc;
  const QDomElement el = p.toXml(doc, "params");
  BOOST_CHECK(el.attribute("confidence") == "17.573");
  BOOST_CHECK(el.attribute("angle-source") == "best-guess");
  BOOST_CHECK(el.attribute("uncertain") == "angle oblique");
  BOOST_CHECK(el.attribute("rejected-oblique") == "-3.661");

  const Params r(el);
  BOOST_REQUIRE(r.confidence().has_value());
  BOOST_CHECK_CLOSE(*r.confidence(), 17.573, 1e-9);
  BOOST_CHECK(r.angleSource() == DeskewSource::BEST_GUESS);
  BOOST_CHECK(r.isAngleUncertain());
  BOOST_CHECK(r.isObliqueUncertain());
  BOOST_CHECK(r.isUncertain());
  BOOST_REQUIRE(r.diagnostics().rejectedOblique.has_value());
  BOOST_CHECK_CLOSE(*r.diagnostics().rejectedOblique, -3.661, 1e-9);
}

BOOST_AUTO_TEST_CASE(diagnostics_missing_in_old_projects) {
  QDomDocument doc;
  QDomElement el = doc.createElement("params");
  el.setAttribute("mode", "auto");
  el.setAttribute("angle", "0");
  el.setAttribute("oblique", "0");
  el.setAttribute("oblique-mode", "auto");
  const Params p(el);
  BOOST_CHECK(!p.confidence().has_value());
  BOOST_CHECK(p.angleSource() == DeskewSource::PRIMARY);
  BOOST_CHECK(!p.isUncertain());
  const QDomElement out = p.toXml(doc, "params");
  BOOST_CHECK(!out.hasAttribute("confidence"));
  BOOST_CHECK(!out.hasAttribute("angle-source"));
  BOOST_CHECK(!out.hasAttribute("uncertain"));
}

BOOST_AUTO_TEST_CASE(diagnostics_not_kept_for_manual_angles) {
  AutoDiagnostics diag;
  diag.confidence = 0.5;
  diag.source = DeskewSource::BEST_GUESS;
  diag.angleUncertain = true;
  diag.obliqueUncertain = true;
  Params p(1.0, 2.5, Dependencies(), MODE_MANUAL, MODE_AUTO);
  p.setDiagnostics(diag);
  BOOST_CHECK(!p.isAngleUncertain());
  BOOST_CHECK(p.isObliqueUncertain());
  QDomDocument doc;
  QDomElement el = p.toXml(doc, "params");
  BOOST_CHECK(!el.hasAttribute("confidence"));
  BOOST_CHECK(el.attribute("uncertain") == "oblique");

  Params q(1.0, 2.5, Dependencies(), MODE_MANUAL, MODE_MANUAL);
  q.setDiagnostics(diag);
  BOOST_CHECK(!q.isUncertain());
  el = q.toXml(doc, "params");
  BOOST_CHECK(!el.hasAttribute("uncertain"));
}

BOOST_AUTO_TEST_SUITE_END()
}  // namespace Tests
