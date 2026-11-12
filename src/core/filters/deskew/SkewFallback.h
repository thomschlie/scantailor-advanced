// Copyright (C) 2026  scantailor-cli contributors
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.
//
// Deskew fallback for pages on which SkewFinder has a low confidence (scantailor-cli issue #15).
// SkewFinder::findSkew() scores the whole page; on open-book scans (page-block edges, tabs, the
// gutter, two columns whose lines do not line up) its confidence can drop below
// Skew::GOOD_CONFIDENCE although the text is clearly tilted, and upstream then sets the angle to 0
// without telling anyone. This estimator measures only text-sized components, per column, and
// cross-checks its angle with an independent second estimate.

#ifndef SCANTAILOR_DESKEW_SKEWFALLBACK_H_
#define SCANTAILOR_DESKEW_SKEWFALLBACK_H_

#include <functional>
#include <optional>

class Dpi;

namespace imageproc {
class BinaryImage;
class Skew;
}  // namespace imageproc

namespace deskew {

/**
 * \brief Result of the fallback skew estimator.
 *
 * All angles in degrees in SkewFinder's convention: positive = the text lines fall to the right
 * (page turned clockwise). The deskew angle that straightens the page is the negated value.
 */
struct FallbackSkew {
  /** Projection-profile angle (the estimate). */
  double angle = 0.0;
  /** Best profile score / median score over the search range (1 = flat). */
  double contrast = 1.0;
  /** Text-sized connected components measured. */
  int components = 0;
  /** Text columns found. */
  int columns = 0;
  /** Second estimate: slope of the character bottoms per text line. */
  std::optional<double> baseline;
  /** Vertical references converted to a text angle: longest column rule, left text edge. */
  std::optional<double> rule;
  double ruleMm = 0.0;
  std::optional<double> edge;
  int edgePoints = 0;
  double edgeMm = 0.0;
  /** Enough text and a second estimate agrees with the profile angle. */
  bool sure = false;

  /** Fewer components than this: no text to measure (blank page, picture). */
  static constexpr int MIN_TEXT_COMPONENTS = 10;
  /** A sure estimate needs at least this many components. */
  static constexpr int MIN_SURE_COMPONENTS = 40;
  /** Search range of the profile, +-degrees. */
  static constexpr double MAX_ANGLE = 5.0;
  /** Angles beyond this are at the edge of the search range: never sure. */
  static constexpr double MAX_SURE_ANGLE = 4.9;
  /** Baseline vs profile, degrees. */
  static constexpr double AGREE_BASELINE = 0.25;
  /** Column rule / text edge vs profile (they include the page's shear), degrees. */
  static constexpr double AGREE_VERTICAL = 0.1;

  bool hasText() const { return components >= MIN_TEXT_COMPONENTS; }
};

/**
 * \brief Measure the skew of the text on a binarised page (black = ink).
 *
 * Port of scantailor-cli's tools/skewcheck.py measurement: text-sized components only
 * (0.6-7 mm high, 0.3-12 mm wide, not solid, not touching the image border, not inside a
 * frame or picture >= 20 x 20 mm), split into columns at vertical gaps >= 4 mm; the angle maximises
 * the sum of squared row profiles of all columns, each sheared about its own centre (-5..+5 deg in
 * 0.05 deg steps, parabola refinement). Sure if >= 40 components and the character baselines agree
 * within 0.25 deg, or, without baselines, the column rule or left text edge within 0.1 deg.
 * Images above 400 dpi are reduced 2x (or more) first.
 */
FallbackSkew findSkewFallback(const imageproc::BinaryImage& image, const Dpi& dpi);

/** Where a page's automatic deskew angle came from. */
enum class DeskewSource {
  PRIMARY,     ///< SkewFinder (upstream behaviour), confirmed by the fallback where it was checked
  FALLBACK,    ///< the fallback estimator's angle (it is sure, SkewFinder was not or disagreed)
  BEST_GUESS,  ///< nothing confident: the best low-confidence angle, page marked uncertain
  NO_TEXT,     ///< SkewFinder low confidence, no text to measure: 0, not uncertain
  DISABLED     ///< SkewFinder low confidence, fallback switched off: 0 (upstream behaviour)
};

struct DeskewDecision {
  /** Skew angle in SkewFinder's convention (deskew angle = -skewAngle). */
  double skewAngle = 0.0;
  DeskewSource source = DeskewSource::PRIMARY;
  bool uncertain = false;
};

/** The fallback's angle replaces SkewFinder's rounded 0 only if they differ by more than this (deg). */
constexpr double MAX_ROUNDED_ZERO_DISAGREEMENT = 0.1;
/** ... and a confident non-zero SkewFinder angle only if they differ by more than this (deg). */
constexpr double MAX_CONFIDENT_DISAGREEMENT = 0.15;
/** An unsure fallback angle this far (deg) from a confident non-zero SkewFinder angle marks it uncertain. */
constexpr double MIN_UNCERTAIN_CONFIDENT_DISAGREEMENT = 0.3;
/** An unsure fallback angle beyond this (deg, tools/skewcheck.py's threshold) makes a 0 uncertain. */
constexpr double MIN_UNCERTAIN_ZERO_ANGLE = 0.15;

/**
 * \brief Choose the automatic deskew angle.
 *
 * \p primary is SkewFinder::findSkew()'s result, \p unrounded the same before its minimum-angle
 * rule (SkewFinder returns 0 with GOOD_CONFIDENCE for any angle below 0.1 deg).
 *
 * - SkewFinder confident and its angle not 0: the fallback checks it; sure and more than
 *   MAX_CONFIDENT_DISAGREEMENT away: the fallback's angle, else SkewFinder's (SkewFinder's angle is
 *   only as fine as 1/16 deg and can lean on non-text structure). Unsure but more than
 *   MIN_UNCERTAIN_CONFIDENT_DISAGREEMENT away (with enough text): SkewFinder's angle, page uncertain.
 * - SkewFinder confident at 0: on open-book scans the page borders can give a strong, wrong peak at
 *   exactly 0, so the fallback checks it. Sure and more than MAX_ROUNDED_ZERO_DISAGREEMENT away from
 *   SkewFinder's unrounded angle: the fallback's angle. Unsure but text tilted by more than
 *   MIN_UNCERTAIN_ZERO_ANGLE (and away from SkewFinder's angle): 0 kept, page uncertain. Else 0.
 * - SkewFinder not confident (upstream: 0): the fallback's angle if it is sure; no text: 0;
 *   else the best low-confidence angle (the fallback's profile angle if it had enough text inside
 *   its search range, otherwise SkewFinder's own), page uncertain.
 *
 * With the fallback off: upstream behaviour (\p primary, or 0 below GOOD_CONFIDENCE).
 */
DeskewDecision decideDeskew(const imageproc::Skew& primary,
                            const imageproc::Skew& unrounded,
                            bool fallbackEnabled,
                            const std::function<FallbackSkew()>& fallback);

/**
 * Automatic oblique angles beyond this (degrees) are not trusted: the oblique is set to 0 and the
 * page marked uncertain. On Grammar 2/3 every automatic oblique above 1 deg was wrong by
 * tools/skewcheck.py's shear measurement (the text wanted ~0).
 */
constexpr double MAX_PLAUSIBLE_OBLIQUE = 1.0;

inline bool isObliqueImplausible(const double obliqueDeg) {
  return obliqueDeg > MAX_PLAUSIBLE_OBLIQUE || obliqueDeg < -MAX_PLAUSIBLE_OBLIQUE;
}

}  // namespace deskew

#endif  // SCANTAILOR_DESKEW_SKEWFALLBACK_H_
