// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "tst_ux_qt.h"

#include <CollapsibleGroupBox.h>
#include <ErrorWidget.h>
#include <ImageTransformation.h>
#include <filters/select_content/ImageView.h>

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
