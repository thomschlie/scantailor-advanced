// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef TST_BATCH_QT_H
#define TST_BATCH_QT_H

#include <QObject>
#include <QString>
#include <QTemporaryDir>

// Headless batch processing (BatchProcessor) end to end, on a synthetic tilted text page.
class TestBatchQt : public QObject {
  Q_OBJECT
 private slots:
  void initTestCase();
  void testAllStagesProduceOutput();
  void testManualSettingSurvives();
  void testTwoRunsAreIdentical();
  void testOuterBottomLayout();
  void testOuterBottomRightToLeft();
  void testFirstPageAndPageSide();
  void testResetLayout();

 private:
  QTemporaryDir m_dir;
  QString m_image;
};

#endif
