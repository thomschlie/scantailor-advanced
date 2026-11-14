// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include <StageSequence.h>

#include <boost/test/unit_test.hpp>

// The rule which data is propagated down the stage list when the current stage changes
// (StageSequence::propagationOnStageChange), with the stage order of StageSequence:
// 0 fix orientation, 1 split pages, 2 deskew, 3 select content, 4 margins, 5 output.
BOOST_AUTO_TEST_SUITE(CoreStageSequenceTestSuite)

namespace {
const int kFixOrientation = 0;
const int kSelectContent = 3;

StageSequence::Propagation rule(const int from, const int to) {
  return StageSequence::propagationOnStageChange(from, to, kFixOrientation, kSelectContent);
}
}  // namespace

BOOST_AUTO_TEST_CASE(test_moving_below_fix_orientation_propagates_orientations) {
  for (int to = 1; to <= kSelectContent; ++to) {
    const StageSequence::Propagation p = rule(kFixOrientation, to);
    BOOST_CHECK(p.pageOrientations);
    BOOST_CHECK(!p.contentBoxes);
  }
}

BOOST_AUTO_TEST_CASE(test_moving_below_select_content_propagates_content_boxes) {
  for (int from = 1; from <= kSelectContent; ++from) {
    for (int to = kSelectContent + 1; to <= 5; ++to) {
      const StageSequence::Propagation p = rule(from, to);
      BOOST_CHECK(p.contentBoxes);
      BOOST_CHECK(!p.pageOrientations);
    }
  }
}

BOOST_AUTO_TEST_CASE(test_jump_from_the_top_propagates_both) {
  for (int to = kSelectContent + 1; to <= 5; ++to) {
    const StageSequence::Propagation p = rule(kFixOrientation, to);
    BOOST_CHECK(p.contentBoxes);
    BOOST_CHECK(p.pageOrientations);
  }
}

BOOST_AUTO_TEST_CASE(test_no_previous_stage_counts_as_the_top) {
  // fromIdx = -1: the first stage of a batch run.
  BOOST_CHECK(!rule(-1, kFixOrientation).pageOrientations);
  BOOST_CHECK(!rule(-1, kFixOrientation).contentBoxes);
  BOOST_CHECK(rule(-1, 1).pageOrientations);
  BOOST_CHECK(!rule(-1, 1).contentBoxes);
  BOOST_CHECK(rule(-1, 5).pageOrientations);
  BOOST_CHECK(rule(-1, 5).contentBoxes);
}

BOOST_AUTO_TEST_CASE(test_no_propagation_within_or_upwards) {
  for (int from = 0; from <= 5; ++from) {
    for (int to = 0; to <= 5; ++to) {
      const StageSequence::Propagation p = rule(from, to);
      if (to <= from) {
        // Staying, or moving up the list: nothing new to pass on.
        BOOST_CHECK(!p.contentBoxes);
        BOOST_CHECK(!p.pageOrientations);
      }
      if (from > kSelectContent) {
        BOOST_CHECK(!p.contentBoxes);
      }
      if (from > kFixOrientation) {
        BOOST_CHECK(!p.pageOrientations);
      }
    }
  }
}

BOOST_AUTO_TEST_SUITE_END()
