// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "tst_ux_qt.h"

#include <CollapsibleGroupBox.h>
#include <ErrorWidget.h>
#include <ImageTransformation.h>
#include <ImageMetadata.h>
#include <PageInfo.h>
#include <PageSelectionAccessor.h>
#include <PageSelectionProvider.h>
#include <PageSequence.h>
#include <filters/select_content/ImageView.h>
#include <filters/select_content/LayoutDetector.h>
#include <filters/select_content/OptionsWidget.h>
#include <filters/select_content/Settings.h>

#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QWidget>
#include <QtTest/QtTest>

void TestUxQt::testButtonClickEmitsSignal() {
  QPushButton button("Click me");
  QSignalSpy spy(&button, &QPushButton::clicked);
  QVERIFY(spy.isValid());

  QTest::mouseClick(&button, Qt::LeftButton);
  QCOMPARE(spy.count(), 1);

  QTest::mouseClick(&button, Qt::LeftButton);
  QCOMPARE(spy.count(), 2);
}

void TestUxQt::testCheckBoxToggleChangesState() {
  QCheckBox box("Option");
  QVERIFY(!box.isChecked());

  box.setChecked(true);
  QVERIFY(box.isChecked());

  box.setChecked(false);
  QVERIFY(!box.isChecked());

  QSignalSpy spy(&box, &QCheckBox::toggled);
  box.setChecked(true);
  QCOMPARE(spy.count(), 1);
  QVERIFY(box.isChecked());
}

void TestUxQt::testLineEditAcceptsInput() {
  QLineEdit edit;
  QCOMPARE(edit.text(), QString());

  QTest::keyClicks(&edit, "Hello UX");
  QCOMPARE(edit.text(), QString("Hello UX"));

  edit.clear();
  QTest::keyClicks(&edit, "123");
  QCOMPARE(edit.text(), QString("123"));
}

void TestUxQt::testCollapsibleGroupBoxStateAndSignal() {
  CollapsibleGroupBox group("Options");
  QSignalSpy spy(&group, &CollapsibleGroupBox::collapsedStateChanged);
  QVERIFY(spy.isValid());

  QVERIFY(!group.isCollapsed());

  group.setCollapsed(true);
  QVERIFY(group.isCollapsed());
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.at(0).at(0).toBool(), true);

  group.setCollapsed(false);
  QVERIFY(!group.isCollapsed());
  QCOMPARE(spy.count(), 2);
  QCOMPARE(spy.at(1).at(0).toBool(), false);
}

void TestUxQt::testErrorWidgetCreation() {
  ErrorWidget widget("Error de prueba", Qt::PlainText);
  QVERIFY(widget.findChildren<QWidget*>().size() >= 1);
}

void TestUxQt::testLayoutOverlayDrawn() {
  // The stage-4 view draws the layout detector's elements as thin outlines, a colour per type.
  QImage image(400, 600, QImage::Format_RGB32);
  image.fill(Qt::white);
  const ImageTransformation xform(QRectF(image.rect()), Dpi(300, 300));
  select_content::ImageView view(image, select_content::ImageView::createDownscaledImage(image), ContentMask(), xform,
                                 QRectF(), QRectF(image.rect()), false);
  view.resize(400, 600);
  // Antialiased outlines: count pixels of the colour's hue, not exact values.
  auto count = [](const QImage& img, auto pred) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
      for (int x = 0; x < img.width(); ++x) {
        const QRgb p = img.pixel(x, y);
        n += pred(qRed(p), qGreen(p), qBlue(p));
      }
    }
    return n;
  };
  auto magenta = [](int r, int g, int b) { return r > g + 60 && b > g + 60; };
  auto green = [](int r, int g, int b) { return g > r + 60 && g > b + 60; };
  view.setAttribute(Qt::WA_DontShowOnScreen);
  view.show();
  QCoreApplication::processEvents();
  const QImage before = view.grab().toImage();
  QCOMPARE(count(before, magenta), 0);

  select_content::layout::Element tab;
  tab.type = "tab";
  tab.score = 0.9;
  tab.box = {340, 100, 390, 250};
  select_content::layout::Element number;
  number.type = "page_number";
  number.score = 0.8;
  number.box = {180, 560, 220, 590};
  view.setLayoutElements({tab, number});
  QCoreApplication::processEvents();
  const QImage after = view.grab().toImage();
  QVERIFY(count(after, magenta) > 50);
  QVERIFY(count(after, green) > 20);
}

namespace {
class NoPagesSelected : public PageSelectionProvider {
 public:
  PageSequence allPages() const override { return PageSequence(); }

  std::set<PageId> selectedPages() const override { return {}; }

  std::vector<PageRange> selectedRanges() const override { return {}; }
};
}  // namespace

void TestUxQt::testLayoutTogglesCommitted() {
  // Stage 4's "Include" checkboxes: shown from the page's params, greyed out without a layout
  // model, and a click stores the toggles in the page's params (the automatic box kept beside).
  using namespace select_content;
  LayoutDetector::unload();  // no model, whatever the environment says
  auto settings = std::make_shared<Settings>();
  OptionsWidget widget(settings, PageSelectionAccessor(std::make_shared<NoPagesSelected>()));
  const PageId pageId(ImageId("/nonexistent/page.png"), PageId::LEFT_PAGE);
  widget.preUpdateUI(PageInfo(pageId, ImageMetadata(QSize(2000, 3000), Dpi(600, 600)), 2, false, false));

  const QRectF autoBox(100, 200, 1500, 2400);
  OptionsWidget::UiData ui;
  ui.setPageRect(QRectF(0, 0, 2000, 3000));
  ui.setContentRect(QRectF(50, 200, 1550, 2500));  // automatic box + tab + page number
  ui.setContentDetectionMode(MODE_AUTO);
  ui.setLayoutInclude(layout::kIncludeTabs | layout::kIncludePageNumbers);
  ui.setAutoContentRect(autoBox);
  widget.postUpdateUI(ui);

  auto* group = widget.findChild<QWidget*>("layoutIncludeGroup");
  auto* numbers = widget.findChild<QCheckBox*>("includePageNumbersCB");
  auto* tabs = widget.findChild<QCheckBox*>("includeTabsCB");
  auto* headers = widget.findChild<QCheckBox*>("includeHeadersCB");
  auto* marginalia = widget.findChild<QCheckBox*>("includeMarginaliaCB");
  auto* clean = widget.findChild<QCheckBox*>("cleanPaperEdgeCB");
  QVERIFY(group && numbers && tabs && headers && marginalia && clean);
  QVERIFY(numbers->isChecked() && tabs->isChecked());
  QVERIFY(!headers->isChecked() && !marginalia->isChecked() && !clean->isChecked());
  QVERIFY(!group->isEnabled());  // no model
  QVERIFY(group->toolTip().contains(LayoutDetector::isBuilt() ? "No layout model" : "no layout detector"));

  // As with a model loaded: a click commits the toggles and reloads the page (automatic box).
  group->setEnabled(true);
  QSignalSpy reload(&widget, SIGNAL(reloadRequested()));
  clean->click();
  QCOMPARE(reload.count(), 1);
  std::unique_ptr<Params> params = settings->getPageParams(pageId);
  QVERIFY(params != nullptr);
  QCOMPARE(params->layoutInclude(), layout::kIncludeTabs | layout::kIncludePageNumbers | layout::kCleanPaperEdge);
  QVERIFY(params->autoContentRect().has_value());
  QCOMPARE(*params->autoContentRect(), autoBox);
  QCOMPARE(params->contentDetectionMode(), MODE_AUTO);

  numbers->click();
  tabs->click();
  params = settings->getPageParams(pageId);
  QCOMPARE(params->layoutInclude(), static_cast<unsigned>(layout::kCleanPaperEdge));
  QCOMPARE(reload.count(), 3);

  // A box set by hand: the toggles are stored, but nothing is reloaded (only the automatic box
  // follows them).
  ui.setContentDetectionMode(MODE_MANUAL);
  ui.setLayoutInclude(0);
  ui.setAutoContentRect(std::nullopt);
  widget.postUpdateUI(ui);
  QVERIFY(!clean->isChecked());
  group->setEnabled(true);
  reload.clear();
  headers->click();
  QCOMPARE(reload.count(), 0);
  params = settings->getPageParams(pageId);
  QCOMPARE(params->layoutInclude(), static_cast<unsigned>(layout::kIncludeHeaders));
  QCOMPARE(params->contentDetectionMode(), MODE_MANUAL);
  QCOMPARE(params->contentRect(), QRectF(50, 200, 1550, 2500));
}
