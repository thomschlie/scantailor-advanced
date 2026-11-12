// Copyright (C) 2026  scantailor-cli contributors
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "tst_deskew_qt.h"

#include <PageId.h>
#include <PageSelectionAccessor.h>
#include <PageSelectionProvider.h>
#include <PageSequence.h>
#include <filters/deskew/OptionsWidget.h>
#include <filters/deskew/Params.h>
#include <filters/deskew/Settings.h>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
class NoPages : public PageSelectionProvider {
 public:
  PageSequence allPages() const override { return PageSequence(); }

  std::set<PageId> selectedPages() const override { return {}; }

  std::vector<PageRange> selectedRanges() const override { return {}; }
};
}  // namespace

void TestDeskewQt::testUncertainShownAndFallbackSwitch() {
  // The "uncertain" line under the angle: shown for an uncertain automatic angle / oblique, gone
  // once the angle is set by hand; the fallback checkbox switches the project setting and reloads.
  using namespace deskew;
  auto settings = std::make_shared<Settings>();
  QVERIFY(settings->fallbackEnabled());  // default on
  OptionsWidget widget(settings, PageSelectionAccessor(std::make_shared<NoPages>()));
  const PageId pageId(ImageId("/nonexistent/page.png"), PageId::LEFT_PAGE);
  widget.preUpdateUI(pageId);

  auto* label = widget.findChild<QLabel*>("uncertainLabel");
  auto* fallback = widget.findChild<QCheckBox*>("fallbackCheckBox");
  auto* angle = widget.findChild<QDoubleSpinBox*>("angleSpinBox");
  QVERIFY(label && fallback && angle);
  QVERIFY(fallback->isChecked());
  QVERIFY(!label->isVisibleTo(&widget));

  OptionsWidget::UiData ui;
  ui.setEffectiveDeskewAngle(-0.62);
  ui.setEffectiveObliqueAngle(2.4);
  ui.setMode(MODE_AUTO);
  ui.setObliqueMode(MODE_AUTO);
  AutoDiagnostics diag;
  diag.confidence = 0.83;
  diag.source = DeskewSource::BEST_GUESS;
  diag.angleUncertain = true;
  diag.obliqueUncertain = true;
  ui.setDiagnostics(diag);
  widget.postUpdateUI(ui);
  QVERIFY(label->isVisibleTo(&widget));
  QVERIFY(label->text().contains("Deskew uncertain"));
  QVERIFY(label->text().contains("0.83"));
  QVERIFY(label->text().contains("Oblique uncertain"));

  // The switch: stored in the settings, and the automatic page is reloaded.
  QSignalSpy reload(&widget, SIGNAL(reloadRequested()));
  fallback->click();
  QVERIFY(!settings->fallbackEnabled());
  QCOMPARE(reload.count(), 1);
  fallback->click();
  QVERIFY(settings->fallbackEnabled());
  QCOMPARE(reload.count(), 2);

  // An angle set by hand is the user's: no longer uncertain (the oblique still is), and the switch
  // no longer reloads the page.
  angle->setValue(1.0);
  std::unique_ptr<Params> params = settings->getPageParams(pageId);
  QVERIFY(params != nullptr);
  QCOMPARE(params->mode(), MODE_MANUAL);
  QVERIFY(!params->isAngleUncertain());
  QVERIFY(params->isObliqueUncertain());
  QVERIFY(!label->text().contains("Deskew uncertain"));
  QVERIFY(label->text().contains("Oblique uncertain"));
  fallback->click();
  QVERIFY(!settings->fallbackEnabled());
  QCOMPARE(reload.count(), 2);
}
